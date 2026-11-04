"""Exercise the installed patched client against the untouched local base master."""
from __future__ import annotations

import ctypes
import hashlib
import importlib
import json
from pathlib import Path
import resource
import socket
import subprocess
import sys
import time

root = Path(sys.argv[1] if len(sys.argv) > 1 else "/opt/testdata/native-build")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
work = root / "cpu-smoke"
work.mkdir(parents=True, exist_ok=True)
(work / "master").mkdir(exist_ok=True)

import mooncake.engine as engine
import mooncake.store as store_module
from mooncake.store import MooncakeDistributedStore

installed = Path(store_module.__file__).parent
assert str(installed).startswith("/opt/sglang/lib/python3.12/site-packages/mooncake")
assert Path(engine.__file__).parent == installed
out = {"engine": engine.__file__, "store": store_module.__file__, "objects": [], "passed": False}

# Import the official modules that share the native package, after the patched
# engine/store. No CUDA device or GPU computation is requested.
for name in ("torch", "mooncake._ep", "mooncake.buffer_pool"):
    module = importlib.import_module(name)
    out.setdefault("official_imports", {})[name] = getattr(module, "__file__", None)
try:
    module = importlib.import_module("mooncake.pg")
    out["official_imports"]["mooncake.pg"] = module.__file__
except ImportError as exc:
    # The fixed official base ships torch 2.14.1 and PG bindings for older
    # torch versions. Preserve and report this existing, unused PG limit.
    if "Mooncake PG was not built against torch==" not in str(exc):
        raise
    out["official_pg_baseline_limit"] = str(exc)

master = None
clients = []
buffers = []
registered = []
with (work / "master.log").open("w") as master_log:
    try:
        master = subprocess.Popen(
            [str(installed / "mooncake_master"), "--rpc_address=127.0.0.1",
             "--rpc_port=55051", "--rpc_thread_num=2", "--metrics_host=127.0.0.1",
             "--metrics_port=55052", "--default_kv_lease_ttl=10ms",
             "--root_fs_dir=" + str(work / "master"), "--logtostderr=true"],
            cwd=work, stdout=master_log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 20
        while True:
            assert master.poll() is None, "local official master exited"
            try:
                with socket.create_connection(("127.0.0.1", 55051), timeout=0.2):
                    break
            except OSError:
                assert time.monotonic() < deadline, "local master did not listen"
                time.sleep(0.1)
        for index, segment in enumerate((64 * 1024 * 1024, 0)):
            client = MooncakeDistributedStore()
            clients.append(client)
            code = client.setup("127.0.0.1:" + str(55053 + index), "P2PHANDSHAKE",
                                segment, 16 * 1024 * 1024, "tcp", "", "127.0.0.1:55051")
            assert code == 0, ("setup", index, code)
        provider, consumer = clients
        for size in (1, 4096, 65536, 1048576):
            data = bytes((index * 17 + size) % 251 for index in range(size))
            key = "k31-native-cpu-smoke/bytes/" + str(size)
            assert consumer.put(key, data) == 0
            assert provider.get(key) == data
            assert consumer.get(key) == data
            out["objects"].append({"bytes": size, "sha256": hashlib.sha256(data).hexdigest()})
            assert consumer.remove(key, True) == 0

        # SGLang uses the registered CPU pointer/batch surface, so check it
        # independently of the Python bytes convenience methods.
        keys = ["k31-native-cpu-smoke/pointer/" + str(index) for index in range(3)]
        sizes = [8192, 32768, 131072]
        inputs = [ctypes.create_string_buffer(bytes([31 + index]) * size, size)
                  for index, size in enumerate(sizes)]
        outputs = [ctypes.create_string_buffer(size) for size in sizes]
        buffers = inputs + outputs
        for buffer in buffers:
            address, size = ctypes.addressof(buffer), ctypes.sizeof(buffer)
            assert consumer.register_buffer(address, size) == 0
            registered.append(address)
        codes = consumer.batch_put_from(keys, [ctypes.addressof(value) for value in inputs], sizes)
        assert codes == [0] * len(keys), ("batch_put_from", codes)
        codes = consumer.batch_get_into(keys, [ctypes.addressof(value) for value in outputs], sizes)
        assert codes == sizes, ("batch_get_into", codes)
        assert [value.raw for value in inputs] == [value.raw for value in outputs]
        assert consumer.batch_remove(keys, True) == [0] * len(keys)
        out["registered_batch_bytes"] = sizes
        out["passed"] = True
    finally:
        if clients:
            for address in reversed(registered):
                clients[-1].unregister_buffer(address)
        for client in reversed(clients):
            client.close()
        if master is not None:
            master.terminate()
            try:
                master.wait(timeout=10)
            except subprocess.TimeoutExpired:
                master.kill()
                master.wait(timeout=5)
        (root / "cpu-smoke-result.json").write_text(json.dumps(out, indent=2))
print(json.dumps(out, indent=2))
