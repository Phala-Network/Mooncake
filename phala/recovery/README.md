# Kimi Mooncake recovery image

One image derives from the pinned SGLang phala-3 base, retaining its SGLang/PIG identity and configuration. Replace only `mooncake_master`, `mooncake_client`, `engine.so`, `store.so` and their private dependency closure. This is an explicit native overlay, not a complete wheel replacement; base wheel metadata remains 0.3.13. Optional EP, PG and bench components remain base versions and do not receive the rail fix.

1. Build the `dependencies` stage of `Dockerfile`; retain its image and caches.
2. Mount frozen source, build/tmp/cache/artifacts under `/work` in the isolated builder. Use Python `/opt/sglang/bin/python` (3.12, CUDA 13 base ABI). Run `bash /work/source/phala/recovery/build-native.sh`, then `LD_LIBRARY_PATH=/work/build/mooncake-common:/work/build/mooncake-common/etcd /opt/sglang/bin/python /work/source/phala/recovery/export-runtime.py`. The library search override applies only to the exporter so it can inspect build-tree dependencies; exported artifacts receive a private `$ORIGIN/recovery-libs` RUNPATH. On Windows, create source archives with `git -c core.autocrlf=false archive` to preserve shell-script LF line endings.
3. Copy `/work/artifacts/runtime` to `phala/recovery/artifacts/runtime`, preserving manifest bytes. Build the `runtime` stage with the published Mooncake source revision/release arguments.
4. The build checks hashes and dependency closure, permitting only an absent CUDA driver. Run `/opt/sglang/bin/python /opt/phala/verify-mooncake-recovery.py` with NVIDIA driver capability mounted and no model/GPU allocation. Retain `readelf`, import and CLI output, ABI identity, source/features, manifest and image digest.
5. Run the isolated restart and rail fixtures before any authorized production rollout. CUDA/RDMA execution and full service stability are separate acceptance gates; import success is not GPU validation.

No global LD_LIBRARY_PATH changes, driver stubs, Python package reinstall, model arguments or service configuration changes are included. Production must not select optional Mooncake EP/PG paths until those separately receive the recovery patch and validation.

## SSD eviction reader windows (recovery-2)

PrepareEviction now releases the metadata lock between complete bucket transitions and copies notification keys outside it. A prepare-only mutex preserves admission ordering; pending keys and bytes stay reserved before each reader window. Physical-space credits apply only to buckets visible at the initial snapshot, preventing newly committed buckets from incorrectly reducing that snapshot.

The exact two-file v2 candidate passed 24 deterministic CPU test instances and 14 adjacent safety regressions without instrumentation. Controls reproduced reader blocking with both buffered and direct I/O, and reproduced an unsafe physical credit in the rejected v1 candidate. A 400,000-key CPU fixture reduced the first queued reader's wait from 388–481 ms to 1.14–1.75 ms. This is not a production latency guarantee or a throughput result.

Physical scans, large buckets, Commit, Restore and shared-mutex fairness remain possible sources of latency. The production 13.783-second event was not reproduced in this fixture. Keep the restore-buffer TTL and lease checks unchanged; deployment must preserve SSD data and verify actual eviction/read behavior. Production validation is separate from these source tests.
