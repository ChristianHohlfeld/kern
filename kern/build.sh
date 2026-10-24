#!/usr/bin/env bash
# Builds a Kern WebAssembly module: the engine plus the optional components and
# game sources given.
#
#   kern/build.sh [--app] [--physics] [--game] [--studio] [--lehrer] <out.wasm> [compiler flags / game sources...]
#
# Components (the core renderer, geometry and JSON are always linked):
#   --app      application runtime (kern/src/app) for games written entirely in
#              C++ against kern::App; the host is kern/web/app.mjs.
#   --physics  Box3D (kern/third_party/box3d, compiled as C17) and the C++ layer
#              kern/src/physics.
#   --game     levels, actors and components, the game runtime and the
#              procedural city (kern/src/game); implies --physics.
#   --studio   Kern Studio, the engine editor (kern/src/studio) and its C ABI;
#              implies --game.
#   --commercial  leaves out the assets that may not be used commercially: Pollen Robotics'
#              Micro Duck 3D models (CC BY-NC-SA 4.0). Everything else in the module is
#              Apache 2.0 or permissive (kern/LICENSES.md).
#   --lehrer   the headless Lehrer (kern/src/lehrer: environments, PPO, ONNX
#              export) and its C ABI for the browser's Web Workers; implies --game.
#
# Toolchain: wasi-sdk (clang + wasm-ld + wasi-libc + libc++). Set WASI_SDK to
# its install directory; the CI workflow downloads release 25 from GitHub.
# The module is a WASI "reactor": the host calls _initialize once, then the
# exported kern_* / game functions. No Emscripten runtime is involved.
#
# Every source compiles to its own object in parallel (KERN_JOBS, default: all
# cores) and ThinLTO links them. Objects are cached in $KERN_OBJ_CACHE (default:
# $TMPDIR/kern-obj) under a hash of the flags, the source and all headers, so a
# rebuild only compiles what changed and the Studio and example games share the
# engine's objects (CI keeps the cache between runs).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
with_app=0 with_physics=0 with_game=0 with_studio=0 with_lehrer=0 commercial=0
while [ $# -gt 0 ]; do
  case "$1" in
    --app) with_app=1 ;;
    --physics) with_physics=1 ;;
    --game) with_game=1; with_physics=1 ;;
    --studio) with_studio=1; with_game=1; with_physics=1 ;;
    --lehrer) with_lehrer=1; with_game=1; with_physics=1 ;;
    --commercial) commercial=1 ;;
    *) break ;;
  esac
  shift
done
out="${1:?usage: kern/build.sh [--app] [--physics] [--game] [--studio] <out.wasm> [game sources...]}"
shift || true
WASI_SDK="${WASI_SDK:-/opt/wasi-sdk}"
CXX="$WASI_SDK/bin/clang++"
CC="$WASI_SDK/bin/clang"
[ -x "$CXX" ] || { echo "wasi-sdk clang++ not found at $CXX (set WASI_SDK)" >&2; exit 1; }

mode="${KERN_BUILD:-release}"
if [ "$mode" = debug ]; then opt=(-O1 -g); lto=(); else opt=(-O3 -flto=thin -DNDEBUG); lto=(-flto=thin); fi
jobs="${KERN_JOBS:-$(nproc)}"
cache_root="${KERN_OBJ_CACHE:-${TMPDIR:-/tmp}/kern-obj}"

# Conservative feature set: runs on every WebGL2-capable browser (Safari 15+).
features=(-mcpu=mvp -msign-ext -mbulk-memory -mnontrapping-fptoint -mmutable-globals)
target=(--target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot")

component_dirs=(app physics game studio lehrer)
sources=()
exclude=()
for dir in "${component_dirs[@]}"; do exclude+=(-not -path "$here/src/$dir/*"); done
while IFS= read -r f; do sources+=("$f"); done < <(find "$here/src" -name '*.cpp' "${exclude[@]}" | sort)
add_component() { while IFS= read -r f; do sources+=("$f"); done < <(find "$here/src/$1" -name '*.cpp' | sort); }
[ "$with_app" = 1 ] && add_component app
[ "$with_physics" = 1 ] && add_component physics
[ "$with_game" = 1 ] && add_component game
[ "$with_studio" = 1 ] && add_component studio
[ "$with_lehrer" = 1 ] && add_component lehrer

objects=()
if [ "$with_physics" = 1 ]; then
  cflags=("${target[@]}" -std=c17 "${opt[@]}" "${features[@]}" -fvisibility=hidden -ffunction-sections -fdata-sections -DBOX3D_DISABLE_SIMD -ffp-contract=off
          -I"$here/third_party/box3d/include" -I"$here/third_party/box3d/src" -w)
  key="$(printf '%s ' "${cflags[@]}" "$("$CC" --version | head -1)" | sha256sum | cut -c1-16)"
  cache="$cache_root/box3d-$key"
  mkdir -p "$cache"
  pids=()
  for c in "$here"/third_party/box3d/src/*.c; do
    o="$cache/$(basename "$c" .c).o"
    objects+=("$o")
    newest_header="$(ls -t "$here"/third_party/box3d/src/*.h "$here"/third_party/box3d/src/*.inl "$here"/third_party/box3d/include/box3d/*.h | head -1)"
    if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ "$newest_header" -nt "$o" ]; then
      "$CC" "${cflags[@]}" -c "$c" -o "$o" &
      pids+=($!)
      if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
    fi
  done
  for p in "${pids[@]}"; do wait "$p"; done
fi
# Deterministic libm (kern/core/dmath.h): musl's sin, exp, atan2 ... compiled the same way everywhere.
mflags=("${target[@]}" -std=c17 "${opt[@]}" "${features[@]}" -fvisibility=hidden -ffunction-sections -fdata-sections -ffp-contract=off -fno-math-errno -w)
mkey="$(printf '%s ' "${mflags[@]}" "$("$CC" --version | head -1)" | sha256sum | cut -c1-16)"
mcache="$cache_root/musl-math-$mkey"
mkdir -p "$mcache"
pids=()
for c in "$here"/third_party/musl_math/src/*.c; do
  o="$mcache/$(basename "$c" .c).o"
  objects+=("$o")
  newest_header="$(ls -t "$here"/third_party/musl_math/src/*.h | head -1)"
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ "$newest_header" -nt "$o" ]; then
    "$CC" "${mflags[@]}" -c "$c" -o "$o" &
    pids+=($!)
    if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
  fi
done
for p in "${pids[@]}"; do wait "$p"; done

includes=(-I"$here/include")
[ "$with_physics" = 1 ] && includes+=(-I"$here/third_party/box3d/include")

# Game sources (*.cpp) compile like the engine's; other arguments are compiler flags.
extra_flags=()
[ "$commercial" = 1 ] && extra_flags+=(-DKERN_NO_NONCOMMERCIAL_ASSETS)
for a in "$@"; do
  case "$a" in
    *.cpp|*.cc) sources+=("$(cd "$(dirname "$a")" && pwd)/$(basename "$a")") ;;
    *) extra_flags+=("$a") ;;
  esac
done
cxxflags=("${target[@]}" -std=c++20 "${opt[@]}" "${features[@]}"
          -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -ffp-contract=off
          -Wall -Wextra -Wno-unused-parameter -Werror=return-type
          "${includes[@]}" "${extra_flags[@]}")
flags_key="$(printf '%s ' "${cxxflags[@]}" "$("$CXX" --version | head -1)" | sha256sum | cut -c1-16)"
# Any header or included data (.inc) change rebuilds everything (simple and safe).
headers_key="$(find "$here/include" "$here/src" "$here/third_party/box3d/include" "$here/third_party/microduck" -name '*.h' -o -name '*.inl' -o -name '*.inc' | sort | xargs cat | sha256sum | cut -c1-16)"
cpp_cache="$cache_root/cpp-$flags_key"
mkdir -p "$cpp_cache"
cpp_objects=()
todo=()
for f in "${sources[@]}"; do
  key="$( { echo "$headers_key $f"; cat "$f"; } | sha256sum | cut -c1-24)"
  o="$cpp_cache/$(basename "$f" .cpp)-$key.o"
  cpp_objects+=("$o")
  [ -f "$o" ] || todo+=("$f|$o")
done
if [ "${#todo[@]}" -gt 0 ]; then
  echo "compiling ${#todo[@]} of ${#sources[@]} C++ sources ($jobs jobs)"
  export CXX
  printf '%s\n' "${todo[@]}" | xargs -P "$jobs" -I{} bash -c 'src="${1%%|*}"; obj="${1##*|}"; shift; "$CXX" "$@" -c "$src" -o "$obj.tmp" && mv "$obj.tmp" "$obj"' _ {} "${cxxflags[@]}"
fi

mkdir -p "$(dirname "$out")"
"$CXX" "${target[@]}" "${opt[@]}" "${features[@]}" "${lto[@]}" \
  "${cpp_objects[@]}" "${objects[@]}" \
  -mexec-model=reactor \
  -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=1048576 -Wl,--export-table -Wl,--growable-table \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=2147483648 \
  ${lto[@]:+-Wl,--thinlto-jobs=$jobs -Wl,--thinlto-cache-dir=$cache_root/thinlto} \
  -o "$out"
echo "built $out ($(wc -c < "$out") bytes)"
