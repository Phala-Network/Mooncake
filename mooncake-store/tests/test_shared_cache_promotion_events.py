"""Run real MasterService fixtures and assert the actual emitted hook events."""

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
    ("NotifySuccessDecrementsCounter", "k_first", 1, 0),
    ("NotifyRejectsNonHolder", "k_cold", 1, 0),
    ("NotifyUnknownKey", "nonexistent", 0, 0),
    ("ReaperPopsStagedMemoryReplicaOnExpiry", "k_cold", 0, 0),
    ("NotifyFailureReleasesStateImmediately", "k_a", 0, 1),
    ("NotifyFailureRejectsNonHolder", "k_cold", 0, 1),
]
for test, key, successes, failures in cases:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        output, manifest = root / "capture.jsonl", root / "manifest.json"
        fields = [b"promotion-fixture", b"epoch1", b"default", key.encode()]
        message = b"phala.shared-cache-key.v1\0" + b"".join(
            len(value).to_bytes(4, "big") + value for value in fields
        )
        key_id = hmac.new(b"fixture-salt-0123456789", message, hashlib.sha256).hexdigest()
        config = dict(
            case_id="promotion-fixture", epoch="epoch1", tenant_id="default",
            key_salt="fixture-salt-0123456789", rank="0", component="master",
            key_ids=[key_id], max_keys=1, max_events=16, max_bytes=16384,
            max_duration_ms=120000, max_requests=1, output_path=str(output),
        )
        manifest.write_text(json.dumps(config))
        manifest.chmod(0o600)
        env = dict(os.environ, MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST=str(manifest))
        result = subprocess.run(
            [binary, f"--gtest_filter=PromotionOnHitTest.{test}"],
            env=env, capture_output=True, timeout=120,
        )
        if result.returncode:
            sys.stderr.buffer.write(result.stdout + result.stderr)
            raise SystemExit(result.returncode)
        assert b"[  PASSED  ] 1 test." in result.stdout, (test, result.stdout.decode(errors="replace"))
        assert b"[  SKIPPED ]" not in result.stdout, test
        records = [json.loads(line) for line in output.read_text().splitlines()]
        assert records[-1]["kind"] == "capture_summary" and records[-1]["complete"], test
        committed = [r for r in records if r["kind"] == "promotion_commit"]
        failed = [r for r in records if r["kind"] == "promotion_failure"]
        assert len(committed) == successes and len(failed) == failures, (test, records)
        assert all(r["committed"] and r["replica_id"] > 0 and r["key_id"] == key_id for r in committed)
        assert all(not r["committed"] and r["error"] != 0 for r in failed)
        print(f"{test}: {successes} commit / {failures} failure events verified")
