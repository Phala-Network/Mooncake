# Build dependencies and final runtime share the exact Python/CUDA ABI.
FROM ghcr.io/phala-network/sglang:v0.5.21-phala-3@sha256:d5abbec01e3e6d6f571cf8250871d72373a178dca1d05b398965c759798139cf AS dependencies
USER root
COPY phala/directio/install-deps.sh /opt/install-directio-deps.sh
RUN sed -i 's/\r$//' /opt/install-directio-deps.sh && bash /opt/install-directio-deps.sh
ENV PATH=/opt/sglang/bin:/usr/local/go/bin:$PATH

FROM ghcr.io/phala-network/sglang:v0.5.21-phala-3@sha256:d5abbec01e3e6d6f571cf8250871d72373a178dca1d05b398965c759798139cf AS runtime
ARG MOONCAKE_REVISION
ARG MOONCAKE_RELEASE
LABEL io.phala.mooncake.revision=$MOONCAKE_REVISION \
      io.phala.mooncake.source="https://github.com/Phala-Network/Mooncake" \
      io.phala.mooncake.release=$MOONCAKE_RELEASE \
      io.phala.mooncake.change="owner re-registration, rail recovery and direct SSD writes"
# Inherit SGLang/PIG identity, command, entrypoint and configuration from phala-3.
COPY phala/recovery/artifacts/runtime/ /opt/sglang/lib/python3.12/site-packages/mooncake/
COPY phala/recovery/verify-runtime.py /opt/phala/verify-mooncake-recovery.py
RUN /opt/sglang/bin/python /opt/phala/verify-mooncake-recovery.py --allow-missing-driver
