# Dependency build is reusable; final runtime assembles independently frozen bytes.
FROM ghcr.io/phala-network/sglang:v0.5.21-phala-1@sha256:b4b08139f9180cbd09ddf14e48d6ea43dce084aa22b2ef084560ea18b25a7420 AS dependencies
USER root
COPY phala/directio/install-deps.sh /opt/install-directio-deps.sh
RUN bash /opt/install-directio-deps.sh
ENV PATH=/usr/local/go/bin:$PATH

FROM ghcr.io/phala-network/sglang:v0.5.21-phala-1@sha256:b4b08139f9180cbd09ddf14e48d6ea43dce084aa22b2ef084560ea18b25a7420 AS runtime
ARG SOURCE_COMMIT=uncommitted-development
LABEL org.opencontainers.image.revision=$SOURCE_COMMIT \
      io.phala.mooncake.change="opt-in POSIX direct DATA writes; native owner only"
# Preserve the existing executable wrapper, master, Python extension and engine.
COPY phala/directio/artifacts/mooncake_client /opt/sglang/lib/python3.12/site-packages/mooncake/mooncake_client
COPY phala/directio/artifacts/directio-libs/ /opt/sglang/lib/python3.12/site-packages/mooncake/directio-libs/
RUN ldd /opt/sglang/lib/python3.12/site-packages/mooncake/mooncake_client > /tmp/directio-ldd \
    && awk '/not found/ && $1 != "libcuda.so.1" {bad=1} END {exit bad}' /tmp/directio-ldd \
    && rm /tmp/directio-ldd
# Final runtime gate must resolve libcuda.so.1 using the NVIDIA runtime (no GPU devices), never a packaged driver stub.
