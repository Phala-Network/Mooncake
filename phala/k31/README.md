# Kimi K3.1 native client

This client retains the upstream `v0.3.13.post1` API and adds the opt-in
`MC_RDMA_RAIL_GROUPS=10.80.20.0/24;10.80.21.0/24` policy. Initial selection,
redispatch and local worker handoff use the declared GID group; different
device names are supported. Unknown or missing GIDs fail closed. Unset policy
retains upstream behavior.

Build with the fixed CUDA 13 SGLang base in `Dockerfile.builder`. Mount an
isolated task directory at `/opt/testdata/native-build`, put this checkout in
its `source/` directory, and initialize the pinned `extern/pybind11` submodule.
Run `bash source/phala/k31/build.sh` in the builder. The script first runs the
26 hardware-free worker-policy regressions, then builds `store` and `engine`.
Etcd, Redis and EP builds are disabled; runtime metadata uses
`P2PHANDSHAKE`.

`package_native.py` writes `native-client.tar.gz`, `package-receipt.json` and
`output/`. Copy only `output/mooncake/` into the base image's installed
`mooncake/` directory. This replaces `store.so` and `engine.so` and adds their
private ELF dependencies; keep the official torch, EP, PG, allocator, master,
Python and helper modules. CUDA runtime and host driver libraries are provided
by the fixed base and NVIDIA container runtime. Driver stubs stay outside the
artifact.

Run `cpu_smoke.py` against that installed package in an isolated container with
no source overlays. It launches the untouched official master on container
loopback and checks P2P/TCP bytes and registered CPU buffer batch operations
between a provider and a consumer. It also imports the retained torch/EP/PG
modules. This verifies CPU API and packaging behavior; hardware RoCE and GPU
acceptance remain separate runtime gates.
