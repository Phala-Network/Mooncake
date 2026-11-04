#!/bin/bash
set -euo pipefail
export TMPDIR=/opt/testdata/native-build/tmp
export XDG_CACHE_HOME=/opt/testdata/native-build/cache
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME"
cmake -S /opt/testdata/native-build/source -B /opt/testdata/native-build/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_SYMBOLS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DPython3_EXECUTABLE=/opt/sglang/bin/python3 -DPYTHON_EXECUTABLE=/opt/sglang/bin/python3 \
  -DUSE_CUDA=ON -DUSE_TENT=OFF -DUSE_ETCD=OFF -DUSE_REDIS=OFF \
  -DSTORE_USE_ETCD=OFF -DSTORE_USE_REDIS=OFF -DWITH_EP=OFF -DWITH_STORE_RUST=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=ON
cmake --build /opt/testdata/native-build/build --target worker_pool_rail_policy_test -j16
cd /opt/testdata/native-build/build
# The regression executes no GPU work. The CPU builder has no host driver;
# expose a toolkit stub only to this test process, never to packaged output.
mkdir -p /opt/testdata/native-build/cpu-stubs
ln -sfn /usr/local/cuda/lib64/stubs/libcuda.so /opt/testdata/native-build/cpu-stubs/libcuda.so.1
LD_LIBRARY_PATH="/opt/testdata/native-build/cpu-stubs:${LD_LIBRARY_PATH:-}" \
  ctest -R '^worker_pool_rail_policy_test$' --output-on-failure
cmake --build /opt/testdata/native-build/build --target store engine -j16
/opt/sglang/bin/python3 /opt/testdata/native-build/source/phala/k31/package_native.py
