#!/usr/bin/env bash
# Builds kern_train, the headless trainer (native, optimised, all cores):
#   kern/tools/train.sh [out-dir]      -> <out-dir>/kern_train (default kern/build)
# Then e.g.: kern/build/kern_train --task jump --envs 64 --iterations 1500 --out jump.onnx
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/build}"
mkdir -p "$out/obj/box3d" "$out/obj/cpp"
cc="${CC:-cc}" cxx="${CXX:-c++}"
jobs="${KERN_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
opt=(-O3 -DNDEBUG)
[ "${KERN_NATIVE:-1}" = 1 ] && opt+=(-march=native)
# Box3D's SIMD solver path (SSE/AVX/NEON) natively; the browser build runs the scalar path. The
# physics is the same model either way, only the float rounding differs.
box3d_simd=""
[ "${KERN_BOX3D_SIMD:-1}" = 1 ] || box3d_simd="-DBOX3D_DISABLE_SIMD -ffp-contract=off"
for c in "$root"/third_party/box3d/src/*.c; do
  o="$out/obj/box3d/$(basename "$c" .c).o"
  [ -f "$o" ] && [ "$o" -nt "$c" ] || echo "$c"
done | xargs -r -P "$jobs" -I{} sh -c '"$0" -std=c17 '"${opt[*]} ${box3d_simd}"' -w -I"$1/third_party/box3d/include" -I"$1/third_party/box3d/src" -c "{}" -o "$2/obj/box3d/$(basename "{}" .c).o"' "$cc" "$root" "$out"
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/src" -name '*.cpp' -not -path "*/studio/*" -not -path "*/app/*" | sort)
sources+=("$root/tests/gl_stub.cpp" "$here/kern_train.cpp")
flags=(-std=c++20 "${opt[@]}" -fno-exceptions -fno-rtti -I"$root/include" -I"$root/third_party/box3d/include" -I"$root/tests")
newest_header="$(find "$root/include" "$root/src" -name '*.h' -o -name '*.inc' | xargs ls -t | head -1)"
for f in "${sources[@]}"; do
  o="$out/obj/cpp/$(echo "$f" | sed "s|$root/||; s|/|_|g").o"
  [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt "$newest_header" ] || echo "$f|$o"
done | xargs -r -P "$jobs" -I{} sh -c 'src="${1%%|*}"; obj="${1##*|}"; shift; "$0" "$@" -c "$src" -o "$obj"' "$cxx" {} "${flags[@]}"
objects=()
for f in "${sources[@]}"; do objects+=("$out/obj/cpp/$(echo "$f" | sed "s|$root/||; s|/|_|g").o"); done
"$cxx" "${opt[@]}" "${objects[@]}" "$out"/obj/box3d/*.o -lm -lpthread -o "$out/kern_train"
echo "built $out/kern_train"
