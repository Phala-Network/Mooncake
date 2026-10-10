# Kimi Mooncake recovery image

One image derives from the pinned SGLang phala-3 base, retaining its SGLang/PIG identity and configuration. Replace only `mooncake_master`, `mooncake_client`, `engine.so`, `store.so` and their private dependency closure. This is an explicit native overlay, not a complete wheel replacement; base wheel metadata remains 0.3.13. Optional EP, PG and bench components remain base versions and do not receive the rail fix.

1. Build the `dependencies` stage of `Dockerfile`; retain its image and caches.
2. Mount frozen source, build/tmp/cache/artifacts under `/work` in the isolated builder. Use Python `/opt/sglang/bin/python` (3.12, CUDA 13 base ABI). Run `bash /work/source/phala/recovery/build-native.sh`, then `python /work/source/phala/recovery/export-runtime.py`.
3. Copy `/work/artifacts/runtime` to `phala/recovery/artifacts/runtime`, preserving manifest bytes. Build the `runtime` stage with the published Mooncake source revision/release arguments.
4. The build checks hashes and dependency closure, permitting only an absent CUDA driver. Run `/opt/sglang/bin/python /opt/phala/verify-mooncake-recovery.py` with NVIDIA driver capability mounted and no model/GPU allocation. Retain `readelf`, import and CLI output, ABI identity, source/features, manifest and image digest.
5. Run the isolated restart and rail fixtures before any authorized production rollout. CUDA/RDMA execution and full service stability are separate acceptance gates; import success is not GPU validation.

No global LD_LIBRARY_PATH changes, driver stubs, Python package reinstall, model arguments or service configuration changes are included. Production must not select optional Mooncake EP/PG paths until those separately receive the recovery patch and validation.
