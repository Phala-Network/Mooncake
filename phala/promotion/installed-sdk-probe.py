"""Run inside the final image: isolated master and two installed SDK clients."""
import json
import os
import pathlib
import subprocess
import tempfile
import time

from mooncake.store import MooncakeDistributedStore, ReplicaStatus


def wait_for(fn, label, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(label)


with tempfile.TemporaryDirectory(prefix="promotion-image-probe-") as root:
    os.environ.update({
        "MOONCAKE_OFFLOAD_HEARTBEAT_INTERVAL_SECONDS": "1",
        "MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES": str(16 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES": str(128 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT": "1000",
        "MOONCAKE_OFFLOAD_USE_URING": "false",
    })
    log = open(pathlib.Path(root) / "master.log", "w+")
    master = subprocess.Popen([
        "/opt/sglang/lib/python3.12/site-packages/mooncake/mooncake_master",
        "--port=19510", "--metrics_port=19513", "--enable_offload=true",
        "--promotion_on_hit=true", "--promotion_admission_threshold=1",
        "--promotion_queue_limit=16", "--default_kv_lease_ttl=10ms",
        "--put_start_release_timeout_sec=3",
    ], stdout=log, stderr=subprocess.STDOUT)
    memory = MooncakeDistributedStore()
    disk = MooncakeDistributedStore()
    try:
        time.sleep(1)
        assert master.poll() is None
        assert memory.setup("127.0.0.1:19511", "P2PHANDSHAKE", 128 * 1024 * 1024,
                            16 * 1024 * 1024, "tcp", "", "127.0.0.1:19510") == 0
        assert disk.setup("127.0.0.1:19512", "P2PHANDSHAKE", 0, 0, "tcp", "",
                          "127.0.0.1:19510", enable_ssd_offload=True,
                          ssd_offload_path=root) == 0
        key = "isolated-installed-sdk-promotion"
        payload = bytes(range(256)) * 4096
        assert memory.put(key, payload) == 0
        wait_for(lambda: any(r.is_local_disk_replica() for r in memory.get_replica_desc(key)),
                 "SSD offload did not complete")
        time.sleep(0.1)
        cleared = memory.batch_replica_clear([key], memory.get_hostname())
        assert key in cleared, cleared
        replicas = memory.get_replica_desc(key)
        assert any(r.is_local_disk_replica() for r in replicas)
        assert not any(r.is_memory_replica() for r in replicas), "control must start SSD-only"
        start = time.monotonic()
        wait_for(lambda: any(r.is_memory_replica() and r.status == ReplicaStatus.COMPLETE
                            for r in memory.get_replica_desc(key)),
                 "V2 promotion did not complete")
        assert memory.get(key) == payload
        assert memory.remove(key) == 0
        print(json.dumps({"result": "PASS", "bytes": len(payload),
                          "promotion_seconds": time.monotonic() - start,
                          "path": "installed SDK -> actual RPC -> SSD -> MEMORY -> byte readback"}))
    finally:
        disk.close()
        memory.close()
        master.terminate()
        master.wait(timeout=10)
        log.seek(0)
        if master.returncode not in (0, -15):
            print(log.read()[-4000:])
        log.close()
