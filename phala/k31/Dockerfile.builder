FROM lmsysorg/sglang:dev-kimi-k31-pro@sha256:4a098ad7414702a2271296ac148854448d10710ec3554ac93b57ef194999305a
USER root
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libibverbs-dev libgoogle-glog-dev \
    libjsoncpp-dev libnuma-dev libgflags-dev libyaml-cpp-dev libssl-dev \
    libcurl4-openssl-dev libzstd-dev libxxhash-dev liburing-dev libaio-dev \
    zlib1g-dev libmsgpack-dev patchelf \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/testdata/native-build
