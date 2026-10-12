# Fenced background promotion

This repair starts from the deployed public source `489ad87003035ced0a0f2038a1fa3c9baf50c684`. The cached native build was stamped `f5431b7be29a41315b690bae44f8e5e1b830dae5`: the only tree differences are CRLF in `master_service.h` and `master_service.cpp`, whose contents are byte-identical after newline normalization. The candidate retains the deployed metadata key budget and its CLI option.

## Design and compatibility

- A single FileStorage worker handles SSD-to-DRAM promotion independently from its offload/remount/watermark heartbeat. Client liveness Ping was already independent; this change does not claim to cure a historical Ping timeout.
- No client-side unbounded queue or additional arena is introduced. Successful batches drain immediately; empty/error batches use the existing heartbeat interval as interruptible backoff.
- The master mailbox is FIFO with indexed removal, so repeated hits cannot starve older work and dequeue cost scales with the returned batch.
- Each admission has a UUID in both its task and mailbox envelope. Dedicated V2 heartbeat/allocation/success/failure RPCs carry this identity. The master compares it under the task's shard lock, including after dequeue. Legacy RPC names and wire layouts remain unchanged; the new store does not fall back to unfenced RPCs. Upgrade master and store together.
- SSD staging/read precedes remote allocation. Failed reads therefore do not consume the target transfer lease. Failure notifications use the captured UUID and cannot cancel a replacement admission.
- Cancellation and expiry remove untaken mailbox entries under the shard lock, preserves all cumulative metrics, and emits at most one key-free summary warning per master per minute.
- Shutdown stops intake, interrupts idle backoff, and joins current synchronous work before destroying buffers. It never detaches I/O. This API cannot forcibly cancel a hung kernel disk read. Existing transfer leases still govern active remote writes; generation fencing protects control-plane callbacks, not an arbitrarily paused writer's physical address.
- Explicit client close rejects new offload RPCs and drains admitted asynchronous I/O before stopping its server. It then calls idempotent FileStorage shutdown, joins heartbeat/promotion/GC and metadata rescan, releases the retained Client reference, and unregisters the Client before freeing segment memory. A retained FileStorage no longer postpones worker shutdown past pool release. This fixes a demonstrated ownership-order hazard, without attributing the observed old-store shutdown SIGSEGV to it or promising arbitrary concurrent SDK close safety.

## Adjacent SSD backports

The worker reads concurrently with watermark eviction, so this branch carries the source changes from `9c5aee20` (reader windows between bucket transitions) and `2214a3ca` (bounded bucket callbacks, rollback lock scope and error summaries). It excludes mainline direct-write, RDMA rail, wheel/CI, and key-budget-removal changes. The backport explicitly preserves `keys_over_cap` and `own_pending_eviction_keys` accounting. Existing key-budget tests remain mandatory controls.

## Focused validation

Build on the existing native cache with the same `/build/source` and `/build/cmake` paths, two CPUs, 24 GiB memory and no additional swap. Preserve the exact source revision in the overlay manifest.

```
cmake -S /build/source -B /build/cmake
cmake --build /build/cmake -j 2 --target local_ssd_test file_storage_promotion_test promotion_on_hit_test storage_backend_test disk_eviction_logging_test metadata_key_budget_test mooncake_master store
```

Tests cover FIFO under continuous hot admissions; captured queue generations; expired mailbox cleanup; stale allocation/success/failure callbacks; blocked SSD read versus heartbeat; read-before-allocation; allocation/write/commit failures; multi-item shutdown; drain during read; backlog and error/idle backoff. Run storage-backend and metadata-key-budget controls after the adapted backport. Actual-image validation additionally uses `installed-sdk-probe.py` for a real SSD-only object to completed MEMORY promotion and byte readback via the installed SDK. Build `promotion_rpc_probe` and invoke it against a separate isolated actual-image master with threshold 1 and processing TTL 3 seconds: it checks expired UUID rejection over all V2 RPCs and survival of the fresh task. The latter uses synthetic metadata and never transfers bytes; it is not a data-path proof.

The latch tests cover FileStorage drain state and captured mailbox generations, not the complete Unmount RPC path or a paused master dequeue-to-shard interleaving. The production transport can choose LOCAL_MEMCPY, whose completion wait is not bounded by the Transfer Engine 60-second timeout; this patch does not assert a universal transfer bound.

Use the existing native packaging script and Dockerfile under `phala/key-budget`, supplying the exact `MOONCAKE_REVISION` and a release-specific `MOONCAKE_VERSION`. Publish the source tag and GitHub Release before pushing its image. Deployment must verify registered capacity, metadata recovery, shared reads, promotion progress, and resource/NUMA observations independently.

## Design ablation

A thread per task or local queue is unnecessary and would add memory/concurrency risk; one worker with server-side batching suffices. Recency sorting is removed in favor of FIFO rather than adding an aging scheduler. Updating the legacy wire type is avoided by separate V2 endpoints. UUID fencing is retained because key/holder identity cannot distinguish re-admission. SSD reader-window and rollback backports are retained for concurrency with the new worker, while unrelated mainline changes are excluded.
