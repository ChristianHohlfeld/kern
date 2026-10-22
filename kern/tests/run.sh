#!/usr/bin/env bash
# Builds and runs the Kern unit tests natively (host compiler, sanitizers on).
#   kern/tests/run.sh            (CXX overrides the compiler, default c++)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${KERN_TEST_OUT:-${TMPDIR:-/tmp}/kern-tests}"
mkdir -p "$out"
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/src" -name '*.cpp' | sort)
"${CXX:-c++}" -std=c++20 -O1 -g -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Wall -Wextra -Wno-unused-parameter -Werror \
  -I"$root/include" -I"$here" "${sources[@]}" "$here/gl_stub.cpp" "$here/test_main.cpp" -o "$out/kern_tests"
"$out/kern_tests"
