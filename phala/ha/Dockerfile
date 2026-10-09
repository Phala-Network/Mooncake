# syntax=docker/dockerfile:1
# CPU-only master build. Serving engines/owners retain their accepted images.
FROM ghcr.io/phala-network/sglang:v0.5.21-phala-1@sha256:b4b08139f9180cbd09ddf14e48d6ea43dce084aa22b2ef084560ea18b25a7420 AS build
USER root
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    ca-certificates curl git build-essential pkg-config libibverbs-dev \
    libgoogle-glog-dev libjsoncpp-dev libnuma-dev libgflags-dev libyaml-cpp-dev \
    libssl-dev libcurl4-openssl-dev libgtest-dev libzstd-dev libxxhash-dev \
    liburing-dev libaio-dev zlib1g-dev python3-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://go.dev/dl/go1.25.10.linux-amd64.tar.gz -o /tmp/go.tar.gz \
    && echo '42d4f7a32316aa66591eca7e89867256057a4264451aca10570a715b3637ba70  /tmp/go.tar.gz' | sha256sum -c - \
    && tar -C /usr/local -xzf /tmp/go.tar.gz && rm /tmp/go.tar.gz
ENV PATH=/usr/local/go/bin:$PATH
RUN git clone --branch 1.11.908 --depth 1 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws \
    && test "$(git -C /tmp/aws rev-parse HEAD)" = 765f3eec667c66643522a37278e7920e48b0b821 \
    && cmake -S /tmp/aws -B /tmp/aws-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_ONLY=s3 -DENABLE_TESTING=OFF -DBUILD_SHARED_LIBS=OFF \
       -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/ha-deps \
    && cmake --build /tmp/aws-build -j6 && cmake --install /tmp/aws-build \
    && rm -rf /tmp/aws /tmp/aws-build
COPY . /src
RUN git clone https://github.com/pybind/pybind11.git /src/extern/pybind11 \
    && git -C /src/extern/pybind11 checkout 58c382a8e3d7081364d2f5c62e7f429f0412743b
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/opt/ha-deps -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DENABLE_DEBUG_SYMBOLS=OFF -DUSE_CUDA=OFF -DUSE_TENT=OFF \
    -DUSE_ETCD=OFF -DSTORE_USE_ETCD=ON -DUSE_REDIS=OFF -DSTORE_USE_REDIS=OFF \
    -DWITH_STORE_RUST=OFF -DWITH_EP=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_BENCHMARK=OFF -DBUILD_UNIT_TESTS=ON \
    && grep -q 'AWSSDK_DIR:PATH=/opt/ha-deps' /build/CMakeCache.txt
RUN cmake --build /build --target mooncake_master -j16
# Keep the build target available for focused CPU tests; runtime is separate.
FROM build AS tests

FROM ghcr.io/phala-network/sglang:v0.5.21-phala-1@sha256:b4b08139f9180cbd09ddf14e48d6ea43dce084aa22b2ef084560ea18b25a7420 AS runtime
USER root
COPY --from=build /build/mooncake-store/src/mooncake_master /opt/sglang/bin/mooncake_master
COPY --from=build /build/mooncake-common/etcd/libetcd_wrapper.so /usr/local/lib/libetcd_wrapper.so
RUN ldconfig && ! ldd /opt/sglang/bin/mooncake_master | grep 'not found'
ENTRYPOINT ["/opt/sglang/bin/mooncake_master"]
