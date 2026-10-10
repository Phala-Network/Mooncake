#!/usr/bin/env python3
"""Check scoped ELF linkage and frozen bytes; strict imports require real driver."""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--allow-missing-driver', action='store_true')
args = parser.parse_args()
root = Path('/opt/sglang/lib/python3.12/site-packages/mooncake')
manifest = json.loads((root / 'recovery-manifest.json').read_text())
for name, digest in manifest.items():
    path = root / name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
    linkage = subprocess.check_output(['ldd', str(path)], text=True)
    missing = [line.split()[0] for line in linkage.splitlines() if 'not found' in line]
    if args.allow_missing_driver:
        missing = [name for name in missing if name != 'libcuda.so.1']
    assert not missing, (name, missing)
if not args.allow_missing_driver:
    for module in ['mooncake.engine', 'mooncake.store']:
        imported = importlib.import_module(module)
        print(module, imported.__file__)
    for binary in ['mooncake_master', 'mooncake_client']:
        result = subprocess.run([str(root / binary), '--help'], capture_output=True, text=True, timeout=30)
        assert 'Flags' in result.stdout + result.stderr or 'flags' in result.stdout + result.stderr, binary
print('Scoped Mooncake recovery verification passed')
