#!/usr/bin/env python3
"""Export four scoped targets and their private dynamic dependency closure."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sysconfig

build = Path('/work/build')
out = Path('/work/artifacts/runtime')
out.mkdir(parents=True, exist_ok=True)
private = out / 'recovery-libs'
private.mkdir(exist_ok=True)
suffix = sysconfig.get_config_var('EXT_SUFFIX')
artifacts = {
    'mooncake_master': build / 'mooncake-store/src/mooncake_master',
    'mooncake_client': build / 'mooncake-store/src/mooncake_client',
    'engine.so': build / ('mooncake-integration/engine' + suffix),
    'store.so': build / ('mooncake-integration/store' + suffix),
}
# These components must remain supplied by the identical phala-3 base/runtime.
base = re.compile(r'^(?:ld-linux|libc\.|libm\.|libpthread\.|librt\.|libdl\.|libgcc_s\.|libstdc\+\+\.|libcuda\.|libcudart\.|libnvidia)')
original_hashes = {}
for name, source in artifacts.items():
    linkage = subprocess.check_output(['ldd', str(source)], text=True)
    missing = [line.split()[0] for line in linkage.splitlines() if 'not found' in line]
    if any(lib != 'libcuda.so.1' for lib in missing):
        raise SystemExit(f'{name}: unresolved dependency: {missing}')
    for line in linkage.splitlines():
        match = re.match(r'\s*(\S+) => (/\S+) \(', line)
        if not match or base.match(match[1]):
            continue
        soname, path = match.groups()
        digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
        if soname in original_hashes and original_hashes[soname] != digest:
            raise SystemExit(f'Conflicting dependency: {soname}')
        original_hashes[soname] = digest
        target = private / soname
        shutil.copyfile(path, target)
        subprocess.run(['patchelf', '--set-rpath', '$ORIGIN', str(target)], check=True)
    target = out / name
    shutil.copyfile(source, target)
    target.chmod(0o755)
    subprocess.run(['patchelf', '--set-rpath', '$ORIGIN/recovery-libs', str(target)], check=True)
manifest = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(out.rglob('*')) if path.is_file() and path.name != 'recovery-manifest.json'}
(out / 'recovery-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
