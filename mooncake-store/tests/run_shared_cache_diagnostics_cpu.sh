#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=${1:?Pass an isolated output directory}
mkdir -p "$build"
${CXX:-c++} -std=c++20 -Wall -Wextra -Werror -pthread \
  -I"$root/mooncake-store/include" $(pkg-config --cflags jsoncpp) \
  "$root/mooncake-store/src/shared_cache_diagnostics.cpp" \
  "$root/mooncake-store/tests/shared_cache_diagnostics_test.cpp" \
  $(pkg-config --libs jsoncpp) -lcrypto -o "$build/shared_cache_diagnostics_test"
"$build/shared_cache_diagnostics_test"
python3 "$root/mooncake-store/tests/test_shared_cache_diagnostics_runtime.py" "$build/shared_cache_diagnostics_test"
