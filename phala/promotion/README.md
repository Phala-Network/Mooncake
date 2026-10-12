# Fenced background promotion

FileStorage runs one serial promotion worker independently from the offload/remount heartbeat. It reuses the fixed staging arena, immediately drains successful batches, and backs off empty/error batches. SSD read precedes target allocation; shutdown joins current synchronous I/O before freeing buffers.

FIFO mailbox entries retain their admission UUID through dequeue. Separate V2 heartbeat/allocation/success/failure RPCs compare this identity under the metadata shard lock, rejecting stale work after expiry or re-admission. Legacy RPC names and wire layouts remain unchanged. Upgrade master and store together; new stores do not fall back to unfenced RPCs. Cancellation/expiry removes pending mailbox entries before releasing the shard, and expiry warning summaries are rate-limited without dropping metrics.

Tests cover starvation under hot arrivals, queue generation capture, cancelled/expired mailbox cleanup, stale callbacks, slow reads versus heartbeat, allocation/write/commit failures, batch shutdown, drain state, and success/idle/error scheduling. The release build on deployed-compatible source is distinct from this main integration; main retains its current metadata and SSD accounting.

One worker avoids unbounded concurrency or additional local queues. FIFO replaces recency sorting rather than adding scheduler complexity. Separate V2 RPCs preserve legacy serialization. Existing SSD reader-window and bounded rollback changes on main remain prerequisites for concurrent watermark eviction.

Generation fencing protects control-plane callbacks, not an arbitrarily paused physical writer. Synchronous disk I/O cannot be forcibly cancelled; LOCAL_MEMCPY waits are not bounded by the Transfer Engine timeout. These changes do not establish the cause of historical Ping timeouts or NUMA OOMs.
