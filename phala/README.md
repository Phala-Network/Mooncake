# Mooncake TCP pump recovery

This source starts at upstream `v0.3.13.post1` (`719735896c86b56fabec6cf3e825fb2ea640597a`) and backports the runtime change from upstream commit [`74d26b56`](https://github.com/kvcache-ai/Mooncake/commit/74d26b56e535b02427e94c3a6087c31a4cfc4963), merged in [#4063](https://github.com/kvcache-ai/Mooncake/pull/4063).

A hard rejection can reserve a pump epoch without posting its handler. The fix posts that handler independently of the rejected work's completion. Queue capacities, failure results and exactly-once ownership remain unchanged. It does not retry failed transfers or claim that every oversized batch succeeds.

The item-cap regression fills the admitted and pending queues before forcing rejection after the pump clears its scheduled flag. CI requires failure with the original line, success with the fix, repeated recovery, and the existing TCP suite. CUDA13 packaging uses the unchanged upstream wheel workflow for CPython 3.12. No production host is used for compilation.

Wheel version: `0.3.13.post1+phala.1`. Runtime rollout acceptance remains separate from source tests and wheel publication.

## Kimi K3.1 client compatibility release

The separate K3.1 client is fixed at
[`phala-v0.3.13.post1-k31-client-1`](https://github.com/Phala-Network/Mooncake/releases/tag/phala-v0.3.13.post1-k31-client-1),
commit `467533b2e80a101dfabd4789ed5aa8ab251c24a6`, on the same upstream
`v0.3.13.post1` baseline. Its opt-in `MC_RDMA_RAIL_GROUPS` policy constrains
RDMA selection, retries and local handoff by GID CIDR, including peers whose
HCA names differ. The unset policy retains upstream selection.

The [fixed build and validation entrypoint](https://github.com/Phala-Network/Mooncake/tree/phala-v0.3.13.post1-k31-client-1/phala/k31)
exports only `store.so`, `engine.so` and their private ELF dependencies for the
official CUDA13 K3.1 SGLang base. The released `native-client.tar.gz` has SHA256
`d29e00d21997f98658eba12be29ea402b8387d4ad991482fdd8f0d1d9cb5d2a4`.
The build passes 26 worker-policy tests; installed-package CPU tests cover
P2P/TCP bytes and registered-buffer batch reads, preserving the other 12
official native modules. Hardware RoCE, GPU/model and production acceptance
remain rollout gates. The main TCP recovery runtime above remains available.
