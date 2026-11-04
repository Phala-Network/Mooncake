# syntax=docker/dockerfile:1
FROM lmsysorg/sglang:dev-kimi-k31-pro@sha256:4a098ad7414702a2271296ac148854448d10710ec3554ac93b57ef194999305a AS toolchain
USER root
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libibverbs-dev libgoogle-glog-dev \
    libjsoncpp-dev libnuma-dev libgflags-dev libyaml-cpp-dev libssl-dev \
    libcurl4-openssl-dev libzstd-dev libxxhash-dev liburing-dev libaio-dev \
    zlib1g-dev libmsgpack-dev patchelf \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/testdata/native-build

# Build inside the dedicated task builder and export only client artifacts.
# This avoids loading another complete SGLang base image into Docker.
FROM toolchain AS native-build
ARG NATIVE_SOURCE_REV
RUN test -n "${NATIVE_SOURCE_REV}" && \
    git init source && \
    git -C source remote add origin https://github.com/Phala-Network/Mooncake.git && \
    git -C source fetch --depth=1 origin "${NATIVE_SOURCE_REV}" && \
    git -C source checkout --detach FETCH_HEAD && \
    test "$(git -C source rev-parse HEAD)" = "${NATIVE_SOURCE_REV}" && \
    git -C source submodule update --init --depth=1 extern/pybind11
RUN --mount=type=cache,id=kimi-k31-native-build-20261105,target=/opt/testdata/native-build/build \
    bash source/phala/k31/build.sh && \
    cp build/Testing/Temporary/LastTest.log /opt/testdata/native-build/worker-policy-test.log

FROM scratch AS native-artifacts
COPY --from=native-build /opt/testdata/native-build/output /output
COPY --from=native-build /opt/testdata/native-build/native-client.tar.gz /native-client.tar.gz
COPY --from=native-build /opt/testdata/native-build/package-receipt.json /package-receipt.json
COPY --from=native-build /opt/testdata/native-build/worker-policy-test.log /worker-policy-test.log
