# Bucket physical-cap admission

`MOONCAKE_OFFLOAD_BUCKET_MAX_PHYSICAL_BYTES` enables the bucket backend's
physical admission guard. Its recursive `st_blocks * 512` scan includes nested
content and orphan files and now runs without the metadata mutex used by
readers. The default scan cache retains approximate shared-directory accounting.

`MOONCAKE_OFFLOAD_BUCKET_INCREMENTAL_PHYSICAL_ACCOUNTING` defaults to `false`.
Opting in makes successful backend writes and actual deletions update a
physical ledger immediately. Enable it only when one `BucketStorageBackend`
is the sole writer and deleter of the directory. Another backend's writes
would be detected only by reconciliation. Unwritten admitted buckets retain
separate reservations across scans and metadata commits. Master callbacks and
waiting for in-flight reads hold neither physical nor metadata locks.

`MOONCAKE_OFFLOAD_BUCKET_DISK_SCAN_CACHE_MS` keeps its `500` millisecond default.
A scan's start time starts this interval. Legacy mode invalidates the cache
after eviction; incremental mode updates the ledger instead. Reconciliation
occurs on subsequent capacity checks, not on a background timer. Values at or
below zero disable scan reuse. Scan duration also affects observation age.

For a verified dedicated store, set incremental accounting to `true` and the
scan interval to `60000` to reduce repeated full scans. Keep the default legacy
mode for shared TP-rank directories, or separate their directories and budgets.
Neither cache mode provides a strict combined quota for concurrent processes.

Reservations round data and metadata separately to filesystem allocation
units. In incremental mode, both files are synchronized and closed before
their actual allocation replaces the reservation. Unusual filesystem
preallocation can exceed the estimate: publication is rejected, but transient
allocation cannot be undone retroactively. Keep headroom below the volume's
real limit and use a filesystem quota when strict enforcement is required.

Failed scans, including entry-stat errors, reject new writes. Failures retain
budget until actual allocation is accounted for or cleanup confirms that both
files are absent. If synchronization and cleanup both fail, the reservation
remains charged across later scans until restart recovery; this can cause
conservative admission refusals.
