"""Run inside the final image: isolated master and two installed SDK clients."""
import json
import os
import pathlib
import subprocess
import tempfile
import time
import urllib.request

def counters():
    try:
        text = urllib.request.urlopen("http://127.0.0.1:19513/metrics", timeout=3).read().decode()
        return [line for line in text.splitlines() if line.startswith("master_promotion_")]
    except Exception as exc:
        return [type(exc).__name__]


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
        "MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES": str(256 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT": "1000",
        "MOONCAKE_OFFLOAD_USE_URING": "false",
        "MOONCAKE_OFFLOAD_BUCKET_SIZE_LIMIT_BYTES": str(8 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_BUCKET_KEYS_LIMIT": "8",
        "MOONCAKE_OFFLOAD_BUCKET_MAX_TOTAL_SIZE": str(256 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_BUCKET_MAX_PHYSICAL_BYTES": str(256 * 1024 * 1024),
        "MOONCAKE_OFFLOAD_BUCKET_EVICTION_POLICY": "lru",
    })
    log = open(pathlib.Path(root) / "master.log", "w+")
    print(json.dumps({"bucket_environment": {k: v for k, v in os.environ.items() if k.startswith("MOONCAKE_OFFLOAD_BUCKET_")}}), flush=True)
    master = subprocess.Popen([
        "/opt/sglang/lib/python3.12/site-packages/mooncake/mooncake_master",
        "--port=19510", "--metrics_port=19513", "--enable_offload=true",
        "--offload_on_evict=true", "--client_ttl=60", "--default_kv_soft_pin_ttl=10ms",
        "--promotion_on_hit=true", "--promotion_admission_threshold=1",
        "--promotion_queue_limit=16", "--default_kv_lease_ttl=10ms",
        "--put_start_release_timeout_sec=3", "--put_start_discard_timeout_sec=1",
    ], stdout=log, stderr=subprocess.STDOUT)
    memory = MooncakeDistributedStore()
    disk = MooncakeDistributedStore()
    try:
        time.sleep(1)
        assert master.poll() is None
        print(json.dumps({"event": "start", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "offload_on_evict": True, "processing_ttl_seconds": 3, "pool_bytes": 128 * 1024 * 1024, "counters": counters()}), flush=True)
        assert memory.setup("127.0.0.1:19511", "P2PHANDSHAKE", 128 * 1024 * 1024,
                            16 * 1024 * 1024, "tcp", "", "127.0.0.1:19510",
                            enable_ssd_offload=True, ssd_offload_path=root) == 0
        assert disk.setup("127.0.0.1:19512", "P2PHANDSHAKE", 0, 16 * 1024 * 1024,
                          "tcp", "", "127.0.0.1:19510") == 0
        prefix = "isolated-installed-sdk-promotion-"
        payload = bytes(range(256)) * 4096
        keys = []
        # This master contains only private probe keys and this small pool.
        # Never point this fill workload at a production master.
        for index in range(120):
            key = prefix + str(index)
            for attempt in range(50):
                if memory.put(key, payload) == 0:
                    keys.append(key)
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("private-pool pressure did not make progress")
            time.sleep(0.02)
        def choose_ssd_key():
            for candidate in keys:
                replicas = memory.get_replica_desc(candidate)
                if any(r.is_local_disk_replica() for r in replicas) and not any(r.is_memory_replica() for r in replicas):
                    return candidate
            return None
        key = wait_for(choose_ssd_key, "eviction did not offload a private key", 60)
        # choose_ssd_key observed actual SSD-only metadata after natural eviction.
        # Do not clear the combined store's segment: that would also remove SSD.
        start = time.monotonic()
        wait_for(lambda: any(r.is_memory_replica() and r.status == ReplicaStatus.COMPLETE
                            for r in memory.get_replica_desc(key)),
                 "V2 promotion did not complete")
        assert disk.get(key) == payload
        for private_key in keys:
            memory.remove(private_key)
        print(json.dumps({"result": "PASS", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "counters": counters(), "bytes": len(payload),
                          "promotion_seconds": time.monotonic() - start,
                          "path": "offload_on_evict=true private-pool pressure -> SSD -> V2 promotion -> MEMORY -> byte readback"}))
    finally:
        disk.close()
        memory.close()
        master.terminate()
        master.wait(timeout=10)
        log.seek(0)
        print("ISOLATED_MASTER_LOG_TAIL", log.read()[-12000:])
        log.close()
