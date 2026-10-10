#!/bin/bash
set -euo pipefail
export PATH=/opt/sglang/bin:/usr/local/go/bin:$PATH
export GOMAXPROCS=${GOMAXPROCS:-24}
export TMPDIR=/work/tmp GOCACHE=/work/cache/go-build GOMODCACHE=/work/cache/go-mod XDG_CACHE_HOME=/work/cache
mkdir -p "$TMPDIR" "$GOCACHE" "$GOMODCACHE"
if [ ! -e /work/source/extern/pybind11/CMakeLists.txt ]; then
  cp -a /opt/pybind11/. /work/source/extern/pybind11/
fi
cmake -S /work/source -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_PREFIX_PATH=/opt/directio-deps -DCMAKE_CXX_FLAGS="-isystem /opt/directio-deps/include" \
 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PROJECT_INCLUDE=/opt/directio-cmake-init.cmake \
 -DPython3_EXECUTABLE=/opt/sglang/bin/python -DPYTHON_EXECUTABLE=/opt/sglang/bin/python \
 -DENABLE_DEBUG_SYMBOLS=OFF -DUSE_TENT=OFF -DUSE_CUDA=ON -DUSE_ETCD=ON \
 -DUSE_INTRA_NVLINK=ON -DWITH_EP=ON -DSTORE_USE_ETCD=ON \
 -DUSE_REDIS=OFF -DSTORE_USE_REDIS=OFF -DWITH_STORE_RUST=OFF \
 -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=OFF
cmake --build /work/build --target mooncake_master mooncake_client engine store -j"${BUILD_JOBS:-24}"
