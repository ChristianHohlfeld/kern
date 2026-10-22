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
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ -n "$(find "$root/third_party/box3d" -name '*.h' -newer "$o" -o -name '*.inl' -newer "$o" | head -1)" ]; then
    "$cc" -std=c17 -O2 -g -DBOX3D_DISABLE_SIMD -ffp-contract=off -w -I"$root/third_party/box3d/include" -I"$root/third_party/box3d/src" -c "$c" -o "$o" &
    pids+=($!)
  fi
done
for p in "${pids[@]}"; do wait "$p"; done
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/src" -name '*.cpp' | sort)
sources+=("$here/gl_stub.cpp" "$here/test_main.cpp")
# One object per source, compiled in parallel (all cores).
cxx="${CXX:-c++}"
flags=(-std=c++20 -O1 -g -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -Wall -Wextra -Wno-unused-parameter -Werror -I"$root/include" -I"$root/third_party/box3d/include" -I"$here")
mkdir -p "$out/obj"
export cxx
printf '%s\n' "${sources[@]}" | xargs -P "${KERN_JOBS:-$(nproc)}" -I{} bash -c 'src="$1"; shift; o="'"$out"'/obj/$(echo "$src" | sed "s|/|_|g").o"; "$cxx" "$@" -c "$src" -o "$o"' _ {} "${flags[@]}"
cpp_objects=()
for f in "${sources[@]}"; do cpp_objects+=("$out/obj/$(echo "$f" | sed "s|/|_|g").o"); done
"$cxx" -fsanitize=address,undefined "${cpp_objects[@]}" "${objects[@]}" -lm -o "$out/kern_tests"
(cd "$here" && "$out/kern_tests" "$@")
# Logic graphs exported as C++ must behave exactly like the interpreter.
if [ $# -eq 0 ]; then
  gen="$out/logic_gen"
  rm -rf "$gen" && mkdir -p "$gen"
  "$out/kern_tests" --emit-logic "$gen"
  core=()
  for f in "${cpp_objects[@]}"; do case "$f" in *_src_logic_*|*_src_core_*) core+=("$f") ;; esac; done
  "$cxx" "${flags[@]}" "$here/logic_native_test.cpp" "$gen"/logic_*.cpp "${core[@]}" -lm -o "$out/logic_native_test"
  "$out/logic_native_test" "$gen/logic_expected.txt"
fi
