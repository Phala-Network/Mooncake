# Mooncake TCP pump recovery

This source starts at upstream `v0.3.13.post1` (`719735896c86b56fabec6cf3e825fb2ea640597a`) and backports the runtime change from upstream commit [`74d26b56`](https://github.com/kvcache-ai/Mooncake/commit/74d26b56e535b02427e94c3a6087c31a4cfc4963), merged in [#4063](https://github.com/kvcache-ai/Mooncake/pull/4063).

A hard rejection can reserve a pump epoch without posting its handler. The fix posts that handler independently of the rejected work's completion. Queue capacities, failure results and exactly-once ownership remain unchanged. It does not retry failed transfers or claim that every oversized batch succeeds.

The item-cap regression fills the admitted and pending queues before forcing rejection after the pump clears its scheduled flag. CI requires failure with the original line, success with the fix, repeated recovery, and the existing TCP suite. CUDA13 packaging uses the unchanged upstream wheel workflow for CPython 3.12. No production host is used for compilation.

Wheel version: `0.3.13.post1+phala.1`. Runtime rollout acceptance remains separate from source tests and wheel publication.
