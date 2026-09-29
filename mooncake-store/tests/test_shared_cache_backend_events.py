"""Check diagnostic bytes/errors from real offset and io_uring backend reads."""

import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
cases = [
    ("OffsetAllocatorStorageBackend_BasicPutGet", ["key1", "key2", "key3"], "offset_allocator", False),
    ("BucketBatchLoadUsesUringAcrossQueueDepth", [f"uring_batch_key_{i}" for i in range(40)], "bucket", False),
    ("BucketBatchLoadRejectsShortRead", ["short_read_key_0", "short_read_key_1"], "bucket", True),
]
for test, keys, backend, expect_short in cases:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        output, manifest = root / "capture.jsonl", root / "manifest.json"
        ids = []
        for key in keys:
            fields = [b"backend-fixture", b"epoch1", b"default", key.encode()]
            message = b"phala.shared-cache-key.v1\0" + b"".join(
                len(value).to_bytes(4, "big") + value for value in fields
            )
            ids.append(hmac.new(b"fixture-salt-0123456789", message, hashlib.sha256).hexdigest())
        config = dict(
            case_id="backend-fixture", epoch="epoch1", tenant_id="default",
            key_salt="fixture-salt-0123456789", rank="0", component="ssd-owner",
            key_ids=ids, max_keys=64, max_events=128, max_bytes=262144,
            max_duration_ms=120000, max_requests=1, output_path=str(output),
        )
        manifest.write_text(json.dumps(config))
        manifest.chmod(0o600)
        env = dict(os.environ, MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST=str(manifest), TMPDIR=directory)
        result = subprocess.run([binary, f"--gtest_filter=StorageBackendTest.{test}"],
                                env=env, cwd=directory, capture_output=True, timeout=120)
        if result.returncode:
            sys.stderr.buffer.write(result.stdout + result.stderr)
            raise SystemExit(result.returncode)
        assert b"[  PASSED  ] 1 test." in result.stdout, (test, result.stdout.decode(errors="replace"))
        assert b"[  SKIPPED ]" not in result.stdout, test
        records = [json.loads(line) for line in output.read_text().splitlines()]
        assert records[-1]["kind"] == "capture_summary" and records[-1]["complete"], test
        reads = [r for r in records if r["kind"] == "backend_read_return"]
        assert reads and all(r["backend"] == backend and r["key_id"] in ids for r in reads), test
        assert all(len(r["file_id"]) == 64 for r in reads)
        if expect_short:
            assert any(r["error"] != 0 and r["returned_bytes"] < r["requested_bytes"] for r in reads)
        else:
            assert {r["key_id"] for r in reads} == set(ids)
            assert all(r["error"] == 0 and r["returned_bytes"] == r["requested_bytes"] for r in reads)
        print(f"{test}: {len(reads)} actual backend read events verified")
