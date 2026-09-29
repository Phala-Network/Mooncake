"""Exercise the actual native manifest parser, output writer and shutdown."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    manifest = root / "manifest.json"
    output = root / "capture.jsonl"
    config = dict(
        case_id="case1",
        epoch="epoch1",
        tenant_id="default",
        key_salt="fixture-salt-0123456789",
        rank="0",
        component="fixture",
        key_ids=["6ef3b9cff0cca0e84d254e750accc0d9ddf413eb8fda38d47c58e461106ac6b3"],
        max_keys=64,
        max_events=32,
        max_bytes=8192,
        max_duration_ms=100,
        max_requests=1,
        output_path=str(output),
    )
    manifest.write_text(json.dumps(config))
    manifest.chmod(0o600)
    env = dict(os.environ, MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST=str(manifest))
    result = subprocess.run(
        [binary, "--runtime"], env=env, capture_output=True, check=True
    )
    data = output.read_text()
    records = [json.loads(line) for line in data.splitlines()]
    assert len(records) == 3
    assert {(r["read_purpose"], r["returned_bytes"]) for r in records[:-1]} == {
        ("consumer_get", 123),
        ("promotion", 456),
    }
    assert records[-1]["kind"] == "capture_summary" and records[-1]["complete"]
    assert output.stat().st_mode & 0o777 == 0o600
    assert "component-key" not in data and config["key_salt"] not in data
    assert str(output) not in data
    # Never truncate a prior receipt; invalid output fails capture closed.
    result = subprocess.run([binary, "--runtime"], env=env, capture_output=True)
    assert result.returncode == 3 and output.read_text() == data
    output.unlink()
    manifest.chmod(0o644)
    result = subprocess.run([binary, "--runtime"], env=env, capture_output=True)
    assert result.returncode == 3 and not output.exists()
    assert config["key_salt"].encode() not in result.stderr
print("native manifest/output fixtures passed")

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for mode in ["--late", "--late-delayed", "--late-invalid"]:
        manifest, staged, recovery, output = [
            root / (mode + suffix)
            for suffix in [".json", ".staged", ".recovery", ".jsonl"]
        ]
        config["output_path"] = str(output)
        config["max_duration_ms"] = 1000
        staged.write_text(json.dumps(config))
        staged.chmod(0o600)
        recovery.write_text(json.dumps(config))
        recovery.chmod(0o600)
        if mode == "--late-invalid":
            staged.chmod(0o644)
        env = dict(
            os.environ,
            MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST=str(manifest),
        )
        result = subprocess.run(
            [binary, mode, str(staged), str(recovery)], env=env, capture_output=True
        )
        assert result.returncode == (
            3 if mode == "--late-invalid" else 0
        ), result.stderr
        if mode != "--late-invalid":
            records = [json.loads(line) for line in output.read_text().splitlines()]
            assert len(records) == 9 and records[-1]["complete"]
        else:
            assert not output.exists()
print(
    "native sealed-manifest late arm, concurrency, delayed bootstrap and no-reload fixtures passed"
)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    path = root / "snapshot.json"
    config = dict(
        case_id="case1",
        epoch="epoch1",
        tenant_id="default",
        key_salt="fixture-salt-0123456789",
        max_snapshots=1,
        max_duration_ms=1000,
        max_response_bytes=65536,
        max_total_logical_bytes=1048576,
        keys=[
            dict(
                key="component-key",
                key_id="6ef3b9cff0cca0e84d254e750accc0d9ddf413eb8fda38d47c58e461106ac6b3",
            )
        ],
    )
    cases = [
        ({}, 0),
        ({"tenant_id": "other"}, 3),
        ({"max_snapshots": 0}, 3),
        ({"max_duration_ms": 120001}, 3),
        ({"keys": config["keys"] * 2}, 3),
        ({"keys": [dict(key="raw*wildcard", key_id=config["keys"][0]["key_id"])]}, 3),
    ]
    for overrides, expected in cases:
        path.write_text(json.dumps(dict(config, **overrides)))
        path.chmod(0o600)
        env = dict(os.environ, MOONCAKE_SHARED_CACHE_SNAPSHOT_MANIFEST=str(path))
        result = subprocess.run(
            [binary, "--snapshot-policy"], env=env, capture_output=True
        )
        assert result.returncode == expected, result.stderr
    path.write_text(json.dumps(config))
    path.chmod(0o644)
    result = subprocess.run([binary, "--snapshot-policy"], env=env, capture_output=True)
    assert result.returncode == 3
    path.unlink()
    os.mkfifo(path, 0o600)
    result = subprocess.run(
        [binary, "--snapshot-policy"], env=env, capture_output=True, timeout=2
    )
    assert result.returncode == 3
    fifo_env = dict(os.environ, MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST=str(path))
    result = subprocess.run(
        [binary, "--runtime"], env=fifo_env, capture_output=True, timeout=2
    )
    assert result.returncode == 3
    path.unlink()
    target = root / "valid.json"
    target.write_text(json.dumps(config))
    target.chmod(0o600)
    path.symlink_to(target)
    result = subprocess.run(
        [binary, "--snapshot-policy"], env=env, capture_output=True, timeout=2
    )
    assert result.returncode == 3
print("finite snapshot manifest bounds, tenant, replay and privacy fixtures passed")
