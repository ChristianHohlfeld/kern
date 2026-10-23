#!/usr/bin/env bash
# Builds kern_lehrer, the headless Lehrer (native, optimised, all cores; Linux and macOS, x86 and ARM):
#   kern/tools/lehrer.sh [out-dir]      -> <out-dir>/kern_lehrer (default kern/build)
# Then e.g.: kern/build/kern_lehrer --task jump --iterations 600 --out jump.onnx
#
# Bit-identical with the browser and the Studio (docs/lehrer.md, determinism): Box3D's scalar path,
# no FMA contraction anywhere (-ffp-contract=off), the deterministic libm (third_party/musl_math).
# The CPU-specific flags (KERN_NATIVE=1, the default) only let the compiler pick instructions; they
# never change a result.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/build}"
mkdir -p "$out/obj/box3d" "$out/obj/musl_math" "$out/obj/cpp"
cc="${CC:-cc}" cxx="${CXX:-c++}"
jobs="${KERN_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
opt=(-O3 -DNDEBUG -ffp-contract=off)
if [ "${KERN_NATIVE:-1}" = 1 ]; then
  for f in -march=native -mcpu=native; do
    if echo 'int main(){return 0;}' | "$cxx" -x c++ "$f" -o /dev/null - 2>/dev/null; then opt+=("$f"); break; fi
  done
fi

# A small job pool (portable to macOS' bash 3.2 and BSD tools): at most $jobs compilers at once,
# and any failing one fails the build.
pids=()
run() {
  "$@" &
  pids+=($!)
  if [ "${#pids[@]}" -ge "$jobs" ]; then
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
}
finish() {
  local p
  for p in ${pids[@]+"${pids[@]}"}; do wait "$p"; done
  pids=()
}
# Is the object $1 older than its source $2 or the header $3?
stale() { [ ! -f "$1" ] || [ "$2" -nt "$1" ] || [ "$3" -nt "$1" ]; }

box_header="$(ls -t "$root"/third_party/box3d/src/*.h "$root"/third_party/box3d/include/box3d/*.h | head -1)"
for c in "$root"/third_party/box3d/src/*.c; do
  o="$out/obj/box3d/$(basename "$c" .c).o"
  if stale "$o" "$c" "$box_header"; then
    run "$cc" -std=c17 "${opt[@]}" -DBOX3D_DISABLE_SIMD -w -I"$root/third_party/box3d/include" -I"$root/third_party/box3d/src" -c "$c" -o "$o"
  fi
done
musl_header="$(ls -t "$root"/third_party/musl_math/src/*.h | head -1)"
for c in "$root"/third_party/musl_math/src/*.c; do
  o="$out/obj/musl_math/$(basename "$c" .c).o"
  if stale "$o" "$c" "$musl_header"; then run "$cc" -std=c17 "${opt[@]}" -fno-math-errno -w -c "$c" -o "$o"; fi
done
sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/src" -name '*.cpp' -not -path "*/studio/*" -not -path "*/app/*" | sort)
sources+=("$root/tests/gl_stub.cpp" "$here/kern_lehrer.cpp")
flags=(-std=c++20 "${opt[@]}" -fno-exceptions -fno-rtti -I"$root/include" -I"$root/third_party/box3d/include" -I"$root/tests")
newest_header="$(ls -t $(find "$root/include" "$root/src" -name '*.h' -o -name '*.inc') | head -1)"
objects=()
for f in "${sources[@]}"; do
  rel="${f#"$root"/}"
  o="$out/obj/cpp/$(echo "$rel" | tr '/' '_').o"
  objects+=("$o")
  if stale "$o" "$f" "$newest_header"; then run "$cxx" "${flags[@]}" -c "$f" -o "$o"; fi
done
finish
"$cxx" "${opt[@]}" "${objects[@]}" "$out"/obj/box3d/*.o "$out"/obj/musl_math/*.o -lm -lpthread -o "$out/kern_lehrer"
echo "built $out/kern_lehrer"
