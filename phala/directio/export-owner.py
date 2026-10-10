#!/usr/bin/env python3
"""Export the frozen native owner and private dependencies; run in build container."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

binary = Path('/work/build/mooncake-store/src/mooncake_client')
output = Path('/work/artifacts/runtime')
output.mkdir(parents=True, exist_ok=True)
private = output / 'directio-libs'
private.mkdir(exist_ok=True)
# Core runtime and GPU driver/runtime remain supplied by the pinned base image.
base_libraries = re.compile(r'^(?:ld-linux|libc\.|libm\.|libpthread\.|librt\.|libdl\.|libgcc_s\.|libstdc\+\+\.|libcuda\.|libcudart\.|libnvidia)')
ldd = subprocess.check_output(['ldd', str(binary)], text=True)
missing = [line.split()[0] for line in ldd.splitlines() if 'not found' in line]
if any(name != 'libcuda.so.1' for name in missing):
    raise SystemExit('Unresolved non-driver owner dependency before packaging')
# libcuda.so.1 is supplied by the production NVIDIA runtime, never by a stub.
if missing:
    print('Build-only driver absence: libcuda.so.1; final runtime gate required.')
for line in ldd.splitlines():
    match = re.match(r'\s*(\S+) => (/\S+) \(', line)
    if not match or base_libraries.match(match[1]):
        continue
    target = private / match[1]
    shutil.copyfile(match[2], target)
    subprocess.run(['patchelf', '--set-rpath', '$ORIGIN', str(target)], check=True)
owner = output / 'mooncake_client'
shutil.copyfile(binary, owner)
owner.chmod(0o755)
subprocess.run(['patchelf', '--set-rpath', '$ORIGIN/directio-libs:$ORIGIN/../mooncake_transfer_engine_cuda13.libs', str(owner)], check=True)
manifest = {str(f.relative_to(output)): hashlib.sha256(f.read_bytes()).hexdigest()
            for f in sorted(output.rglob('*')) if f.is_file()}
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
