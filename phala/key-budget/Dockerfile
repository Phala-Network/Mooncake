ARG BASE_IMAGE=ghcr.io/phala-network/sglang@sha256:82293d51ff2fda7922f7d37b9ebbe0dbcfa15497d4a8c979da59c27edc32ae88
FROM ${BASE_IMAGE}
ARG MOONCAKE_REVISION
LABEL org.opencontainers.image.source="https://github.com/Phala-Network/Mooncake" \
      io.phala.mooncake.native.revision="${MOONCAKE_REVISION}" \
      io.phala.mooncake.profile="host-tcp-ssd-key-budget" \
      io.phala.mooncake.provenance="/opt/phala/mooncake/native-overlay.json"
COPY native/ /opt/sglang/lib/python3.12/site-packages/mooncake/
COPY native-overlay.json /opt/phala/mooncake/native-overlay.json
COPY licenses/ /opt/phala/mooncake/licenses/
RUN python3 -c "import json, os; manifest = json.load(open('/opt/phala/mooncake/native-overlay.json')); assert manifest['source_revision'] == os.environ['MOONCAKE_REVISION']; from mooncake.store import MooncakeDistributedStore; assert callable(MooncakeDistributedStore)"
