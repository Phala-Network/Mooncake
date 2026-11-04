"""Package only the patched Store/Transfer Engine and their private ELF deps."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sysconfig
import tarfile

root = Path(sys.argv[1] if len(sys.argv) > 1 else "/opt/testdata/native-build")
build = root / "build"
output = root / "output"
package = output / "mooncake"
private = package / ".phala_k31_native"
private.mkdir(parents=True, exist_ok=True)

# These are supplied by the fixed SGLang base and the host NVIDIA driver.
# In particular, a CUDA driver stub must never enter the runtime artifact.
base_libraries = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
    "libresolv.so.2", "libstdc++.so.6", "libgcc_s.so.1", "libcuda.so.1",
}


def command(*args: str, env=None) -> str:
    return subprocess.check_output(args, text=True, env=env).strip()


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


env = dict(os.environ)
env["LD_LIBRARY_PATH"] = str(build / "mooncake-common") + ":" + env.get("LD_LIBRARY_PATH", "")
dependencies = {}
original_ldd = {}
for name in ("store", "engine"):
    source = build / "mooncake-integration" / (name + sysconfig.get_config_var("EXT_SUFFIX"))
    assert source.is_file(), source
    target = package / (name + ".so")
    shutil.copy2(source, target)
    resolved = command("ldd", str(source), env=env)
    original_ldd[name] = resolved
    for line in resolved.splitlines():
        if "=> not found" in line:
            assert line.strip().startswith("libcuda.so.1 "), line
        match = re.match(r"\s*(\S+) => (/\S+) ", line)
        if not match:
            continue
        soname, value = match.groups()
        if soname in base_libraries or soname.startswith("libcudart.so."):
            continue
        dependency = Path(value).resolve()
        assert "stubs" not in dependency.parts, dependency
        prior = dependencies.setdefault(soname, str(dependency))
        assert prior == str(dependency), (soname, prior, dependency)
    command("patchelf", "--set-rpath", "$ORIGIN/.phala_k31_native:$ORIGIN", str(target))

for soname, value in dependencies.items():
    target = private / soname
    shutil.copy2(value, target)
    command("patchelf", "--set-rpath", "$ORIGIN", str(target))

elf = {}
for path in sorted(package.rglob("*.so*")):
    resolved = command("ldd", str(path))
    missing = [line.strip() for line in resolved.splitlines() if "=> not found" in line]
    assert all(line.startswith("libcuda.so.1 ") for line in missing), (path, missing)
    dynamic = command("readelf", "-d", str(path))
    assert "/opt/testdata/" not in dynamic and "/stubs" not in dynamic, path
    elf[str(path.relative_to(output))] = {
        "sha256": digest(path),
        "bytes": path.stat().st_size,
        "needed": command("patchelf", "--print-needed", str(path)).splitlines(),
        "rpath": command("patchelf", "--print-rpath", str(path)),
    }

manifest = {
    "source_commit": command("git", "-C", str(root / "source"), "rev-parse", "HEAD"),
    "pybind_commit": command("git", "-C", str(root / "source/extern/pybind11"), "rev-parse", "HEAD"),
    "python": sys.version,
    "extension_suffix": sysconfig.get_config_var("EXT_SUFFIX"),
    "base": "lmsysorg/sglang:dev-kimi-k31-pro@sha256:4a098ad7414702a2271296ac148854448d10710ec3554ac93b57ef194999305a",
    "replaced_modules": ["mooncake/store.so", "mooncake/engine.so"],
    "preserve_official_modules": "All other official Python, torch, EP, PG, allocator and helper modules",
    "private_libraries": dependencies,
    "elf": elf,
    "builder_ldd": original_ldd,
}
(output / "native-manifest.json").write_text(json.dumps(manifest, indent=2))
archive = root / "native-client.tar.gz"
with tarfile.open(archive, "w:gz") as handle:
    handle.add(package, arcname="mooncake")
    handle.add(output / "native-manifest.json", arcname="native-manifest.json")
receipt = {"artifact": str(archive), "sha256": digest(archive), "bytes": archive.stat().st_size,
           "source_commit": manifest["source_commit"], "modules": {name: elf["mooncake/" + name + ".so"] for name in ("store", "engine")}}
(root / "package-receipt.json").write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))
