#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends ca-certificates curl git build-essential cmake ninja-build pkg-config libibverbs-dev libgoogle-glog-dev libjsoncpp-dev libnuma-dev libgflags-dev libyaml-cpp-dev libssl-dev libcurl4-openssl-dev libgtest-dev libzstd-dev libxxhash-dev liburing-dev libaio-dev zlib1g-dev python3-dev libmsgpack-dev patchelf
curl -fsSL https://go.dev/dl/go1.25.10.linux-amd64.tar.gz -o /tmp/go.tar.gz
echo '42d4f7a32316aa66591eca7e89867256057a4264451aca10570a715b3637ba70  /tmp/go.tar.gz' | sha256sum -c -
tar -C /usr/local -xzf /tmp/go.tar.gz
rm /tmp/go.tar.gz
git clone --branch 1.11.908 --depth 1 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws
test "$(git -C /tmp/aws rev-parse HEAD)" = 765f3eec667c66643522a37278e7920e48b0b821
cmake -S /tmp/aws -B /tmp/aws-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY=s3 -DENABLE_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/directio-deps
cmake --build /tmp/aws-build -j8
cmake --install /tmp/aws-build
git clone https://github.com/pybind/pybind11.git /opt/pybind11
git -C /opt/pybind11 checkout 58c382a8e3d7081364d2f5c62e7f429f0412743b
printf 'find_package(ZLIB REQUIRED)\n' > /opt/directio-cmake-init.cmake
