# Direct-I/O native owner CPU development

## Scope
Official Mooncake `e5598b0992cc258b06d22f24d875480e8931a28e`, with only the current opt-in bucket DATA-write change. No prior custom HA code. CPU development is not production or GPU acceptance.

## Reproducible dependencies
`Dockerfile` and `install-deps.sh` declare the pinned SGLang base, AWS SDK 1.11.908 at `765f3eec667c66643522a37278e7920e48b0b821`, verified Go 1.25.10 tarball and pybind11 commit. On the isolated builder the same script installs these in a reusable capped container. Future accepted runtime bytes must be assembled by Dockerfile, not by modifying a running service.

## Resources
- Node 104; owned root `/home/phala/kimi-directio-build-20261010`.
- Container `kimi-directio-dev-20261010`, ID `9fb58bcbf5037df2d114f3efdd6fde29551277739cb00b591ed0f0f784871a7a`.
- Started `2026-10-10T01:24:58.253098343Z`.
- 8 CPU quota; CPUs 0–7, NUMA memory node 0; memory and memory+swap both 24 GiB; PID limit 1024.
- Bridge network for dependency fetches; no ports, GPUs, production mounts or host sysctl changes.
- Only bind mount: owned root to `/work`. No BuildKit builder created.
- Before setup: host available 182 GiB; node0 free approximately 66 GiB (other NUMA nodes lower).

## Incremental interface
Upload the exact reviewed changed files into `source`, preserving the clean official baseline. Record their hashes before compiling. Copy `build-native.sh` to `scripts`.

```bash
docker exec kimi-directio-dev-20261010 bash /work/scripts/build-native.sh mooncake_client storage_backend_test
# Add any new focused native target explicitly.
docker exec kimi-directio-dev-20261010 ctest --test-dir /work/build --output-on-failure -R '^storage_backend_test$'
```

`build-native.sh` uses CUDA OFF, URING ON, RDMA dependency enabled, official etcd store support and AWS S3, with 8 compile jobs. Source and build tree persist under the owned root. Dependency output: `logs/dependencies.log`. No source changes should be uploaded during a final frozen-owner build.

## Pending gates
Finish dependencies, configure and compile focused tests, resolve any failures, then freeze source and build owner. Assemble final image by replacing only owner and required missing shared libraries in the pinned original image; preserve master, wheels and engine. Publication and production changes belong to the parent task and are not authorized here.

Dependency development snapshot: `local/kimi-directio-deps:20261010` (development cache only, not accepted runtime). Isolated formatter is clang-format 20.1.8 under `/opt/directio-formatter`; no production security-policy change is permitted. New POSIX O_DIRECT tests run under default Docker seccomp. Existing io_uring tests may encounter EPERM and do not validate the new write path.

Official baseline tar SHA-256: `c55a420279a9203446141d551df0c4812c6f80cfc26f285c146d892d94819079`. Overlay manifests live in `artifacts/posix-overlay-hashes.json`; formatting ranges in `artifacts/format-ranges.json`. These identify development inputs, not a released source commit.

## Final owner feature parity
The pinned original command `/opt/sglang/bin/mooncake_client` is a Python execv wrapper; the real original ELF is `/opt/sglang/lib/python3.12/site-packages/mooncake/mooncake_client`. Replace only that ELF and its required private shared libraries; do not replace the wrapper, master, Python extension/wheel metadata or engine. The original ELF requires libcudart.so.13 and libcuda.so.1 and uses wheel-private hashed dependencies.

CPU regression defaults to `DIRECTIO_BUILD_MODE=cpu-test`, with CUDA discovery explicitly disabled. Final owner mode `DIRECTIO_BUILD_MODE=native-owner` restores official CUDA13 wheel features `USE_CUDA=ON`, `USE_ETCD=ON`, `USE_INTRA_NVLINK=ON`, `WITH_EP=ON`, `USE_HTTP=ON` (existing default), `STORE_USE_ETCD=ON` and toolkit discovery. Build only `mooncake_client`; this does not build or replace EP kernels or Python wheels. CPU test success does not prove feature parity or GPU execution; final mode requires a separate ELF dependency/help check and package comparison.

## Reusable build and assembly
Use the capped reusable dependency container for development, or build the `dependencies` target of this Dockerfile on an equivalently capped isolated builder. Supply clean official source plus the frozen reviewed overlay at `/work/source`, and `build-native.sh` in `/work/scripts`.

```bash
# CPU regression, default seccomp; tests execute with /work/test-run as cwd.
docker exec kimi-directio-dev-20261010 bash /work/scripts/build-native.sh mooncake_client storage_backend_test
docker exec -w /work/test-run kimi-directio-dev-20261010 /work/build/mooncake-store/tests/storage_backend_test --gtest_filter='StorageBackendTest.BucketDirectWrite*'
# Original native feature mode, retaining dependency/build caches.
docker exec -e DIRECTIO_BUILD_MODE=native-owner kimi-directio-dev-20261010 bash /work/scripts/build-native.sh mooncake_client
docker exec kimi-directio-dev-20261010 python3 /work/source/phala/directio/export-owner.py
# Copy /work/artifacts/runtime/* to the isolated packaging context's
# phala/directio/artifacts/; never commit generated binaries.
docker build --target runtime -f phala/directio/Dockerfile --build-arg SOURCE_COMMIT=FROZEN_SOURCE_COMMIT -t local/kimi-directio-owner:20261010 .
```

`export-owner.py` bundles resolved required non-core shared libraries in an owner-private directory and sets private RUNPATHs. It leaves the base core system libraries and CUDA runtime/driver libraries in place. Final Dockerfile retains base entrypoint/environment, verifies owner `ldd` with no unresolved non-driver libraries, and copies no source overlay. Exact private-library manifest and source hashes must be retained alongside the final image identity. Rebuilding only the source label after a reviewed source commit does not require repeating native compilation when source/recipe byte identity is unchanged.

## CPU result
2026-10-10 01:45 UTC: native CPU build passed. Nine direct-write tests passed (625 ms); 26 POSIX adjacent bucket regressions passed (2065 ms) in default Docker seccomp. Existing io_uring-only read tests were excluded because the production/default seccomp profile disallows io_uring; no security setting was changed. These results do not prove the untouched original binary can read new files; final-image fixture owns that separate gate.

Build-time `ldd` may lack only `libcuda.so.1` because build containers have no NVIDIA runtime. Any other missing library fails export/assembly. Do not package a driver stub. Before acceptance, run final-image full `ldd`, owner help and unchanged Python import with the existing NVIDIA runtime driver injection and `NVIDIA_VISIBLE_DEVICES=none`; no source mounts or package installation. Full runtime resolution is required.
