# p11 metadata key budgets

This maintenance line starts at `cd971227fabc03a79fc33dff3bfb9bbd1a679084`, the Mooncake implementation in the pinned SGLang p11 base. It does not replace the fork's newer `main` line.

The master flag `--metadata_key_limit` limits live metadata plus concurrent insertion reservations. Its default is `0` (unlimited). New objects at capacity receive the existing `NO_AVAILABLE_HANDLE` resource error. Existing reads and disk-replica refreshes continue; deletion, revocation and failed insertion release slots automatically. Snapshot and disk registration paths also consume slots.

The bucket backend enforces `MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT` during LRU/FIFO eviction, including pending writes and victims another transaction might restore. It uses the existing notification-before-file-delete callback and rollback. Failure to reserve capacity produces a transient `FILE_WRITE_FAIL`, not the `KEYS_ULTRA_LIMIT` path that disables offloading. **Compatibility:** the existing default of 10 million keys now takes effect for LRU/FIFO; explicitly configure the required capacity. The production model profile uses 150 million SSD keys and 160 million total master keys. These are key bounds, not a universal resident-memory bound.

## Native overlay build

The recipe replaces only the Python 3.12 `mooncake/store*.so`, native `mooncake_master`, and their staged loader dependencies. It preserves the base Python distribution metadata `0.3.13+phala.clean.cd97122`: this is **not a newly installed wheel**. `/opt/phala/mooncake/native-overlay.json` and the OCI `io.phala.mooncake.native.revision` label identify the new native source commit. The Docker build verifies that the two identities agree.

Use the exact released commit as `REVISION`. Run the build script inside the pinned p11 image with an isolated checkout, persistent build directory and fresh output directory. Limit the builder to 2 CPUs and 24 GiB without GPU devices. The two io_uring backend regression tests require permission to call io_uring; use that permission only in this disposable test builder, not as a production configuration change.

```sh
bash phala/key-budget/build-key-budget-native-public.sh \
  /checkout /build /output "$REVISION"
docker build --build-arg MOONCAKE_REVISION="$REVISION" \
  --tag "$IMAGE_TAG" /output
```

The script compiles and runs `metadata_key_budget_test`, configuration and SSD master regressions, and the bucket backend regressions before packaging. `USE_CUDA=OFF` disables transfer-engine GPU features; detected CUDAToolkit libraries may still support the store's existing D2H staging code. The host TCP/SSD owner requires no GPU device. Existing SGLang requester images remain unchanged and must pass the final-image protocol/reconnect test against this overlay.

Publish the source tag and Release for the exact frozen commit before pushing the derived image. Record the image's immutable digest and validate its installed implementation without source mounts. Whole-disk filling is not required for the boundary and recovery tests.
