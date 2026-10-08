"""Run inside the bounded p11 builder after tests; stage a native-only overlay."""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

source, build, out = map(Path, sys.argv[1:4])
expected = sys.argv[4]
actual = subprocess.check_output(
    ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
).strip()
assert actual == expected
out.mkdir(parents=True, exist_ok=True)
native = out / "native"
native.mkdir(exist_ok=True)
libs = native / "native-libs"
libs.mkdir(exist_ok=True)
licenses = out / "licenses"
licenses.mkdir(exist_ok=True)
store = list((build / "mooncake-integration").glob("store.cpython-312-*.so"))
assert len(store) == 1, store
inputs = [store[0], build / "mooncake-store/src/mooncake_master"]
excluded = re.compile(
    r"^(libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libresolv\.so|libstdc\+\+\.so|libgcc_s\.so|ld-linux|libcuda\.so)"
)
bundled = {}
raw_ldd = {}
ldd_env = os.environ.copy()
# The build extension already uses its install-time $ORIGIN search path.
# Resolve the colocated build library before staging it into native-libs.
ldd_env["LD_LIBRARY_PATH"] = str(build / "mooncake-common") + (
    os.pathsep + ldd_env["LD_LIBRARY_PATH"] if ldd_env.get("LD_LIBRARY_PATH") else ""
)
for f in inputs:
    raw = subprocess.check_output(["ldd", str(f)], text=True, env=ldd_env)
    assert "not found" not in raw, raw
    raw_ldd[f.name] = raw
    shutil.copy2(f, native / f.name)
    for line in raw.splitlines():
        m = re.match(r"\s*(\S+)\s+=>\s+(/\S+)", line)
        if not m or excluded.match(m[1]):
            continue
        soname, path = m.groups()
        src = Path(path).resolve()
        if soname not in bundled:
            shutil.copy2(src, libs / soname)
            bundled[soname] = str(src)
            for query in [str(src), str(src).removeprefix("/usr")]:
                result = subprocess.run(
                    ["dpkg-query", "-S", query], capture_output=True, text=True
                )
                if result.returncode == 0:
                    package = result.stdout.split(": ", 1)[0].split(":", 1)[0]
                    lic = Path("/usr/share/doc") / package / "copyright"
                    if lic.is_file():
                        shutil.copy2(lic, licenses / (package + ".copyright"))
                    break
for f in libs.iterdir():
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", str(f)], check=True)
for f in inputs:
    subprocess.run(
        [
            "patchelf",
            "--set-rpath",
            "$ORIGIN/native-libs:$ORIGIN",
            str(native / f.name),
        ],
        check=True,
    )
shutil.copy2(source / "LICENSE", licenses / "Mooncake.LICENSE")
manifest = {
    "source_repository": "https://github.com/Phala-Network/Mooncake",
    "source_revision": actual,
    "base_image": "ghcr.io/phala-network/sglang@sha256:82293d51ff2fda7922f7d37b9ebbe0dbcfa15497d4a8c979da59c27edc32ae88",
    "profile": {
        "USE_CUDA": False,
        "WITH_EP": False,
        "WITH_STORE": True,
        "WITH_STORE_RUST": False,
        "USE_HTTP": True,
        "USE_ETCD": False,
        "STORE_USE_ETCD": False,
        "note": "Host TCP/SSD server overlay; detected CUDAToolkit may enable store D2H staging. No GPU devices used.",
    },
    "python_distribution_metadata": "Inherited 0.3.13+phala.clean.cd97122 is unchanged; only listed native components and their loader dependencies are replaced. This is not a newly installed wheel.",
    "files": {
        str(f.relative_to(out)): hashlib.sha256(f.read_bytes()).hexdigest()
        for f in native.rglob("*")
        if f.is_file()
    },
    "bundled_dependency_origins": bundled,
}
(out / "native-overlay.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out / "build-ldd.json").write_text(json.dumps(raw_ldd, indent=2) + "\n")
print(
    json.dumps(
        {
            "source_revision": actual,
            "native_files": len(manifest["files"]),
            "output": str(out),
        }
    )
)
