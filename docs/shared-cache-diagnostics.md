# Bounded shared cache diagnostic candidate

Base: Mooncake `e5598b0992cc258b06d22f24d875480e8931a28e`.
This is an unpublished source candidate. It does not qualify a GPU request or
authorize a live clear. The Python API and cache selection remain unchanged; nondefault clear RPC routing is made tenant-aware.

## Capture contract

Capture is disabled unless `MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST` names a
regular, current-user-owned, owner-only readable JSON file (maximum 64 KiB).
An invalid manifest fails capture closed with a fixed message, without echoing
the input. `output_path` must not already exist; a new mode-0600 JSONL file is
created. No public control or destructive endpoint is added.

Required manifest fields are `case_id`, `epoch`, `tenant_id`, `key_salt`,
`rank`, `component`, `key_ids`, `max_keys`, `max_events`, `max_bytes`,
`max_duration_ms`, `max_requests`, `output_path`. Labels are bounded ASCII
identifiers. Salt is 16–256 UTF-8 bytes and is never emitted. Key IDs are full
lowercase HMAC-SHA256 digests. Hard maxima: 256 keys, 8192 events, 16 MiB output,
120 seconds, 1024 queued events (current queue default 256). Hash work is also
bounded to four times `max_events` attempts. The final summary reserves 1 KiB.

`max_requests` must be 1 for this candidate: one engine request in the epoch,
with at most one native read of each allowlisted component key. Native batches
are not user requests: several disjoint pool batches are accepted, and unknown
keys consume no per-key read allowance. A repeated or concurrent read of the
same allowed key marks the epoch incomplete; it is not silently attributed to
the first request. Planned promotion hit warmup must be outside this capture
epoch. A multi-request or same-key concurrent proof needs a propagated RPC
operation identity and is not supported by this patch.

Key HMAC input is `phala.shared-cache-key.v1` plus NUL, followed by `case_id`,
`epoch`, normalized tenant (`default` when unspecified), and the actual native
key. Each field is UTF-8 preceded by its uint32 big-endian byte length. Owner
and file IDs use independent `phala.shared-cache-owner.v1` and
`phala.shared-cache-file.v1` domains. Backend scoped keys are decoded with
`TenantId::ParseScopedKey`; raw keys, endpoints, paths, prompts, bytes and salt
are not emitted. Output includes configured rank/component and actual PID;
the installed acceptance receipt must bind PID/config to the real SSD owner.

The bounded queue is drained by a dedicated writer. A capture summary is
written at shutdown/deadline; any dropped event, ambiguous repeated key or
writer failure makes `complete=false`. Missing summary is inconclusive.
Unknown keys are counted as rejected, never emitted. Disabling capture does
not hash keys or serialize objects; the io_uring event loop is also gated.

## Sealed-manifest activation and finite metadata snapshot

A missing configured event manifest (ENOENT) can be atomically published after
model readiness and seed/warmup completion without restarting the store. The
fixed path is retained on first `Capture::Global()`; absent means unarmed, so
no event duration is consumed during model loading. Later hooks retry only
that path, without background polling. An invalid existing manifest/output
disables capture permanently. Successful activation is one-time: no reload,
salt change or allowlist replacement. The bounded event duration starts on
activation. Actual donor PUT provenance and whole-backup ACK remain the engine
seed producer's responsibility; native pre-seal PUT capture is not claimed.

Existing admin GET `/batch_query_keys` accepts a default-off finite mode with
exactly six unique parameters: `finite_snapshot=1`, `tenant_id`, `case_id`,
`epoch`, `sample_id`, `key_ids` (comma-separated full HMACs). URL <=18000 bytes;
raw `keys`, unknown/duplicate fields, duplicate/subset/unknown IDs are rejected.
The independent `MOONCAKE_SHARED_CACHE_SNAPSHOT_MANIFEST` must be an owned
regular mode0600 file <=64KiB, no symlink. It contains case/epoch/tenant/salt,
`keys:[{key_id,key}]`, `max_snapshots` (1..16), `max_duration_ms` (1..120000),
`max_response_bytes` (4096..1048576), `max_total_logical_bytes` (1..17179869184).
The exact <=256 keys must be nonempty, <=4096 bytes and contain no control,
comma or wildcard characters. First request loads once; samples cannot replay.

`phala.shared-cache.snapshot.v1` includes case/epoch/sample, actual master PID,
sample times, requested/effective tenant, actual `master_multi_tenant_enabled`,
and each key's existence, logical bytes, original writer HMAC, authoritative
`IsLeaseExpired`, all replica IDs/types/statuses/readability/refcounts, segment
HMACs, SSD owner/scope HMACs, and actual processing/offload/promotion/candidate/
replication/dynamic-replication membership. Segment HMAC domain is
`phala.shared-cache-segment.v1`; writer and SSD UUIDs use canonical
`UuidToString` and the owner domain. Owner scope uses the actual endpoint.
Unsupported lease owner, generation and backend drain are explicitly marked
unavailable. Per-key state is sampled under one RO accessor; the entire batch
is not atomic. No Query/Exist/GET, lease renewal, promotion or backend I/O occurs.

Native master defaults to single-tenant mode: ordinary requests then resolve
to `default`, regardless of the worker's tenant configuration. Finite snapshot
and the new internal tenant-aware clear reject nondefault tenants when mode is
disabled. An isolated experiment must enable multi-tenancy and supply its quota
policy before seeding, or explicitly use actual single-tenant semantics. This
patch does not migrate existing objects or prove a live tenant configuration.
`MasterClient::BatchReplicaClear` preserves the old three-argument RPC for
`default`; nondefault uses `BatchReplicaClearForTenant` with no fallback on an
old master. Writer/lease checks remain authoritative. Clear request/result
logs and the reviewed descriptor-query error log contain no raw identifiers.

## Actual hooks and limits

- `RealClient::batch_get_into_internal` and
  `batch_get_into_multi_buffers_internal`: record actual selected descriptor
  tier/replica and final per-key return bytes/error after transfer/checksum.
  Selection is not a success event. Query failures retain unknown tier/error.
  These are the two SGLang zero-copy binding paths. Other single/offset/read
  overloads are not source-qualified by this candidate.
- File-per-key payload decode, bucket vector read/io_uring descriptor, and
  offset allocator value read: record actual backend result and short reads,
  with logical length and hashed file/offset identity. Synchronous
  `FileStorage::LoadBatch` and promotion `BatchLoad` calls use a narrowly scoped
  RAII thread-local purpose (`consumer_get` or `promotion`); these calls contain
  no coroutine suspension. Overlapping threads and nested restoration are
  exercised by the native runtime fixture. These are backend read
  events, not device-level IOPS or allocated physical-byte measurements.
  Failures before payload I/O are visible as native read failures without a
  successful backend payload event. No backend event implies success.
- `NotifyPromotionSuccess`: emit only after the holder/precise staged replica
  commit, accounting, task removal and mailbox cleanup successfully finish.
  Failure notification is separate, after its real cleanup. Wrong holder,
  missing task and failed commit do not emit promotion success.

No actual output event establishes a whole SGLang request or all pools by
itself. Engine whole-group/forward/ack evidence and unchanged exact output
remain separate required gates.

## CPU and complete-hook build gates

Minimal collector and real manifest/output fixture:

```sh
bash mooncake-store/tests/run_shared_cache_diagnostics_cpu.sh /absolute/isolated/build
```

Dependencies: C++20 compiler, pthread, Python 3, pkg-config, JsonCpp 1.9.5 and
OpenSSL libcrypto 3. The script compiles the actual collector with
`-Wall -Wextra -Werror`. Fixtures cover HMAC vectors/domain separation,
disabled mode, allowlist/tenant checks, multiple pool batches, ambiguous
repeated keys, short/error byte preservation, all capture limits, concurrency,
manifest permissions, exclusive output, shutdown.

Complete changed translation-unit and integration gate, in a prepared native
Mooncake dependency environment:

```sh
cmake -S . -B /absolute/isolated/hooks -DWITH_STORE=ON -DWITH_STORE_RUST=OFF \
  -DUSE_CUDA=OFF -DUSE_ETCD=OFF -DBUILD_UNIT_TESTS=ON
cmake --build /absolute/isolated/hooks --parallel 1 --target \
  mooncake_store_shared_objects mooncake_store_client_objects mooncake_store_master_objects
cmake --build /absolute/isolated/hooks --parallel 1 --target \
  promotion_on_hit_test file_storage_promotion_test storage_backend_test
/absolute/isolated/hooks/mooncake-store/tests/file_storage_promotion_test
python3 mooncake-store/tests/test_shared_cache_promotion_events.py \
  /absolute/isolated/hooks/mooncake-store/tests/promotion_on_hit_test
python3 mooncake-store/tests/test_shared_cache_backend_events.py \
  /absolute/isolated/hooks/mooncake-store/tests/storage_backend_test
```

The standalone fixture compiles only the collector. The full integration build
also compiles the actual `real_client.cpp`, `storage_backend.cpp`,
`file_storage.cpp` and `master_service.cpp` hooks. Promotion event fixtures use
real MasterService success, wrong-holder, missing-task, reaped-task and failure
paths; backend fixtures use real offset payload, bucket io_uring and short-read
paths. They reject missing or skipped tests. On 2026-09-29 the isolated CPU
build and collector/runtime fixture passed, along with 11 FileStorage tests,
6 real master event cases and 3 actual backend event cases (3 offset reads,
40 io_uring reads, 2 short-read outcomes). The existing bucket NONE watermark
fixture also passed. This is CPU evidence only.

## Smallest clear-to-read retention decision

The legacy original-writer API checks writer and expired lease, but lacks an
atomic MEMORY-only/retained-COMPLETE-SSD predicate. Query then clear is racy.
No disconnected Boolean policy helper or simulated clear fixture is shipped.
A real clear gate must validate writer/lease/MEMORY/SSD/epoch/owner/inflight
and retained backend state under the actual lock/hold. No live clear is
qualified by the diagnostic fixtures.

The bucket backend supports the first option under a finite static contract:

1. **Source-supported static experiment.** Before constructing the dataset,
   initialize the owner with
   `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR=bucket_storage_backend`,
   `MOONCAKE_OFFLOAD_BUCKET_EVICTION_POLICY=none` and
   `MOONCAKE_OFFLOAD_ENABLE_DISK_WATERMARK_EVICTION=false`.
   `PrepareEviction` and `EvictAboveDiskWatermark` return no eviction under
   NONE; `RunDiskWatermarkEviction` also returns when its flag is false.
   Capacity checks in PrepareEviction are bypassed by NONE, so finish a bounded
   dataset within measured free space before the window. Init's invalid/orphan
   cleanup has no disable switch; finish Init/ScanMeta before the window and
   exclude restart. ScanMeta/HandleNext registration failure does not delete
   buckets. Exclude new writes/rollback, remove/reset/unmount and file mutation;
   drain every offload, promotion and read. Keep master/owner healthy and fail
   on timeout, remount or identity change. Master processing expiry retains
   COMPLETE replicas, but client-loss cleanup still removes LOCAL_DISK
   metadata; `enable_metadata_cleanup_on_timeout=false` does not disable it.
   Read back the actual owner configuration/identity and exact nonempty MEMORY
   targets and retained COMPLETE SSD before the reviewed original-writer API.
   The current diagnostics patch contains no clear command. This quiescent
   contract excludes mutation across query/clear only if operational ownership
   is real; any subset clear or continuity failure ends the attempt. Under
   those conditions no new hold service is needed for the finite read.
2. **Small finite-key hold when background exclusion cannot be proven.**
   Reuse the backend's existing `BucketReadGuard` or offset `AllocationPtr`
   plus shared file handle and retain the exact read plan before clear.
   Add only a bounded holder for those handles (manifest+epoch+expected
   replica IDs, maximum keys and timeout), rather than a general lease
   service. Under the same object metadata lock validate original writer,
   expired lease, exact COMPLETE MEMORY and SSD, owner availability and no
   processing/promotion/read; establish the matching SSD metadata hold before
   removing MEMORY. Actual backend eviction must honor the held read plan,
   and the reader must use that same held plan rather than re-resolve a key
   whose mapping was evicted. Release on read completion or timeout. Do not
   hold the metadata lock across network I/O. Owner failure/expiry fails the
   experiment; no fault-tolerance promise is needed.

Existing master hard pins/refcounts do not prove physical retention:
File/bucket watermark eviction calls its master-notification handler before
physical deletion and restores prepared eviction if the handler fails. However,
that handler reaches `MasterService::EvictDiskReplica`, whose owner/type-matched
removal has no hard-pin, lease or replica-refcount gate. Thus the pin checks in
other master eviction paths do not cover this actual backend callback. Existing backend read guards retain storage only while
`BatchLoad` is active; they do not cover the preceding clear-to-read gap.
Explicit `RemoveAll` and owner failure can be excluded/failed operationally,
but real automatic backend eviction must be disabled with evidence or honor
the finite hold. Source implementation of that hold and an authenticated
original-writer command remain a separate decision; no unsafe shortcut or
new public deletion API is included here.

## Optional owner drain observation

Set `capture_owner_drain=true` in the sealed manifest selected by `MOONCAKE_SHARED_CACHE_OWNER_DRAIN_MANIFEST` to emit `kind=owner_drain`, `schema=phala.shared-cache.owner-drain.v1` records into its own bounded private JSONL. This is a process-local FileStorage observer; the master metadata endpoint still cannot report backend drain. Unavailable backends report unavailable and never synthesize a quiet state.

Each record has actual bucket metadata counts and FileStorage activity counts for loads, offloads, promotions, removes, heartbeats, rescans and leased read buffers. Buffer ownership remains active until its actual remote/local lease is destroyed. Bucket writes, evictions, ungrouped offloads and read guards are sampled under their existing locks, with a finite256bucket limit. An owner activity revision brackets that sample; raced snapshots are marked inconsistent. Init/Scan timestamps are recorded only after actual successful completion, and heartbeat success, resync/draining flags, bucket eviction policy and watermark setting describe actual state.

`owner_id`, runtime-generated `store_instance_id`, `scope_id` and `backend_path_id` use existing case/epoch/tenant HMAC framing. The configured tenant is only capture correlation; `owner_client_requested_tenant_id` records the client request tenant separately. Establish the owner mapping from independent process/container/backend identity and master replica metadata, then require the same instance across the scene. The mapping is not a new storage generation or ownership protocol.

The consumer requires an available, consistent, initialized owner with full bucket coverage, successful heartbeat, completed Init/Scan, selected static-retention settings, no resync/draining and all active/pending values zero over its existing quiet window. `owner_sample_sequence` orders snapshots; `activity_sequence` changes for actual work and buffer lifetime, excluding idle heartbeats. Concurrent JSONL output may be reordered. Final `capture_summary` must be present and complete with zero drops; damaged/missing/truncated evidence cannot establish drain. This observer supplies evidence and does not hold the storage lifecycle or guarantee future quiescence.

### Separate finite owner and event windows

Owner drain uses `Capture::OwnerGlobal`; backend read and promotion events continue to use `Capture::Global` and `MOONCAKE_SHARED_CACHE_DIAGNOSTICS_MANIFEST`. Both use the same Runtime/collector implementation, but each snapshots its own fixed manifest path on first use and can activate once. Each has its own queue, finite deadline, private output and final summary. Both are disabled unless configured; neither rearms or reloads after activation or invalid input.

Provide both fixed environment paths before process startup. Seal the owner manifest after seed/warmup drain, obtain its genuine complete final summary, and use that evidence for the existing pre-clear gate. Then seal the previously absent ordinary event manifest for post-clear reads/promotions. Use the same case/epoch/tenant/salt and distinct output paths; select each capture budget within its consumer's limits. The ordinary absent manifest consumes no capture duration while waiting. Closing the owner window does not close or activate the ordinary event window.

Native UUID identity strings are `UuidToString` decimal unsigned64 pairs (`first-second`), not RFC4122 UUID text. HMAC framing is unchanged.
