#!/bin/bash
set -euo pipefail
export PATH=/usr/local/go/bin:$PATH
export GOMAXPROCS=8
export TMPDIR=/work/tmp
export GOCACHE=/work/cache/go-build
export GOMODCACHE=/work/cache/go-mod
export XDG_CACHE_HOME=/work/cache
mkdir -p "$TMPDIR" "$GOCACHE" "$GOMODCACHE"
if [ ! -e /work/source/extern/pybind11/CMakeLists.txt ]; then
  cp -a /opt/pybind11/. /work/source/extern/pybind11/
fi
if [ "${DIRECTIO_BUILD_MODE:-cpu-test}" = "native-owner" ]; then
  feature_args=(-DUSE_CUDA=ON -DUSE_ETCD=ON -DUSE_INTRA_NVLINK=ON -DWITH_EP=ON -DCMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit=OFF)
else
  feature_args=(-DUSE_CUDA=OFF -DUSE_ETCD=OFF -DUSE_INTRA_NVLINK=OFF -DWITH_EP=OFF -DCMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit=ON)
fi
cmake -S /work/source -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_PREFIX_PATH=/opt/directio-deps -DCMAKE_CXX_FLAGS="-isystem /opt/directio-deps/include" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
 -DCMAKE_PROJECT_INCLUDE=/opt/directio-cmake-init.cmake \
 -DENABLE_DEBUG_SYMBOLS=OFF -DUSE_TENT=OFF "${feature_args[@]}" \
 -DSTORE_USE_ETCD=ON -DUSE_REDIS=OFF -DSTORE_USE_REDIS=OFF \
 -DWITH_STORE_RUST=OFF -DBUILD_EXAMPLES=OFF \
 -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=ON
cmake --build /work/build --target "${@:-mooncake_client}" -j8
