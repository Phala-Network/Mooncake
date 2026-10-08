#!/bin/bash
# Run inside a p11-based, no-GPU builder limited to 2 CPU and 24 GiB.
set -euo pipefail
SOURCE_DIR=${1:?source checkout required}
BUILD_DIR=${2:?persistent build directory required}
OUTPUT_DIR=${3:?output directory required}
SOURCE_REVISION=${4:?exact source commit required}
test "$(git -C "$SOURCE_DIR" rev-parse HEAD)" = "$SOURCE_REVISION"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends libyaml-cpp-dev libxxhash-dev libzstd-dev liburing-dev libibverbs-dev librdmacm-dev libnuma-dev libssl-dev libasio-dev libfmt-dev libmsgpack-dev patchelf
git -C "$SOURCE_DIR" submodule update --init --depth 1 extern/pybind11
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_SYMBOLS=OFF \
  -DWITH_STORE=ON -DWITH_STORE_RUST=OFF -DWITH_EP=OFF \
  -DUSE_CUDA=OFF -DUSE_ETCD=OFF -DSTORE_USE_ETCD=OFF \
  -DUSE_HTTP=ON -DBUILD_UNIT_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF
cmake --build "$BUILD_DIR" -j 2 --target metadata_key_budget_test storage_backend_test master_service_config_test master_service_ssd_test mooncake_master store
"$BUILD_DIR/mooncake-store/tests/metadata_key_budget_test"
"$BUILD_DIR/mooncake-store/tests/master_service_config_test"
"$BUILD_DIR/mooncake-store/tests/master_service_ssd_test"
"$BUILD_DIR/mooncake-store/tests/storage_backend_test" --gtest_filter='StorageBackendTest.Bucket*'
python3 "$SOURCE_DIR/phala/key-budget/package-key-budget-native.py" "$SOURCE_DIR" "$BUILD_DIR" "$OUTPUT_DIR" "$SOURCE_REVISION"
cp "$SOURCE_DIR/phala/key-budget/Dockerfile" "$OUTPUT_DIR/Dockerfile"
