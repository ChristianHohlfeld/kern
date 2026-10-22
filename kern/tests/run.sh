#!/usr/bin/env bash
# Builds and runs the Kern unit tests natively (host compiler, sanitizers on).
#   kern/tests/run.sh            (CXX / CC override the compilers, default c++ / cc)
#
# Box3D is compiled once without sanitizers (third-party C code, scalar path as
# in WebAssembly) and cached; all Kern C++ is built with ASan and UBSan.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${KERN_TEST_OUT:-${TMPDIR:-/tmp}/kern-tests}"
mkdir -p "$out/box3d"
cc="${CC:-cc}"
objects=()
pids=()
for c in "$root"/third_party/box3d/src/*.c; do
  o="$out/box3d/$(basename "$c" .c).o"
  objects+=("$o")
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
    "$cc" -std=c17 -O2 -g -DBOX3D_DISABLE_SIMD -w -I"$root/third_party/box3d/include" -I"$root/third_party/box3d/src" -c "$c" -o "$o" &
    pids+=($!)
  fi
done
for p in "${pids[@]}"; do wait "$p"; done
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/src" -name '*.cpp' | sort)
"${CXX:-c++}" -std=c++20 -O1 -g -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Wall -Wextra -Wno-unused-parameter -Werror \
  -I"$root/include" -I"$root/third_party/box3d/include" -I"$here" "${sources[@]}" "$here/gl_stub.cpp" "$here/test_main.cpp" \
  "${objects[@]}" -lm -o "$out/kern_tests"
(cd "$here" && "$out/kern_tests" "$@")
