#!/usr/bin/env bash
# Builds a Kern WebAssembly module: the engine plus the optional components and
# game sources given.
#
#   kern/build.sh [--app] [--physics] [--world] [--editor] <out.wasm> [compiler flags / game sources...]
#
# Components (the core renderer, geometry and JSON are always linked):
#   --app      application runtime (kern/src/app) for games written entirely in
#              C++ against kern::App; the host is kern/web/app.mjs.
#   --physics  Box3D (kern/third_party/box3d, compiled as C17) and the C++ layer
#              kern/src/physics.
#   --world    scene documents (.e3d), the scene runtime and game components
#              (kern/src/world); implies --physics.
#   --editor   the Kern editor (kern/src/editor) and its C ABI; implies --world.
#
# Toolchain: wasi-sdk (clang + wasm-ld + wasi-libc + libc++). Set WASI_SDK to
# its install directory; the CI workflow downloads release 25 from GitHub.
# The module is a WASI "reactor": the host calls _initialize once, then the
# exported kern_* / game functions. No Emscripten runtime is involved.
#
# C objects of Box3D are cached in $KERN_OBJ_CACHE (default: $TMPDIR/kern-obj)
# and rebuilt only when their source or the flags change.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
with_app=0 with_physics=0 with_world=0 with_editor=0
while [ $# -gt 0 ]; do
  case "$1" in
    --app) with_app=1 ;;
    --physics) with_physics=1 ;;
    --world) with_world=1; with_physics=1 ;;
    --editor) with_editor=1; with_world=1; with_physics=1 ;;
    *) break ;;
  esac
  shift
done
out="${1:?usage: kern/build.sh [--app] [--physics] [--world] [--editor] <out.wasm> [game sources...]}"
shift || true
WASI_SDK="${WASI_SDK:-/opt/wasi-sdk}"
CXX="$WASI_SDK/bin/clang++"
CC="$WASI_SDK/bin/clang"
[ -x "$CXX" ] || { echo "wasi-sdk clang++ not found at $CXX (set WASI_SDK)" >&2; exit 1; }

mode="${KERN_BUILD:-release}"
if [ "$mode" = debug ]; then opt=(-O1 -g); else opt=(-O3 -flto -DNDEBUG); fi

# Conservative feature set: runs on every WebGL2-capable browser (Safari 15+).
features=(-mcpu=mvp -msign-ext -mbulk-memory -mnontrapping-fptoint -mmutable-globals)
target=(--target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot")

component_dirs=(app physics world editor)
sources=()
exclude=()
for dir in "${component_dirs[@]}"; do exclude+=(-not -path "$here/src/$dir/*"); done
while IFS= read -r f; do sources+=("$f"); done < <(find "$here/src" -name '*.cpp' "${exclude[@]}" | sort)
add_component() { while IFS= read -r f; do sources+=("$f"); done < <(find "$here/src/$1" -name '*.cpp' | sort); }
[ "$with_app" = 1 ] && add_component app
[ "$with_physics" = 1 ] && add_component physics
[ "$with_world" = 1 ] && add_component world
[ "$with_editor" = 1 ] && add_component editor

objects=()
if [ "$with_physics" = 1 ]; then
  cflags=("${target[@]}" -std=c17 "${opt[@]}" "${features[@]}" -fvisibility=hidden -ffunction-sections -fdata-sections
          -I"$here/third_party/box3d/include" -I"$here/third_party/box3d/src" -w)
  key="$(printf '%s ' "${cflags[@]}" "$("$CC" --version | head -1)" | sha256sum | cut -c1-16)"
  cache="${KERN_OBJ_CACHE:-${TMPDIR:-/tmp}/kern-obj}/box3d-$key"
  mkdir -p "$cache"
  pids=()
  for c in "$here"/third_party/box3d/src/*.c; do
    o="$cache/$(basename "$c" .c).o"
    objects+=("$o")
    newest_header="$(ls -t "$here"/third_party/box3d/src/*.h "$here"/third_party/box3d/include/box3d/*.h | head -1)"
    if [ ! -f "$o" ] || [ "$c" -nt "$o" ] || [ "$newest_header" -nt "$o" ]; then
      "$CC" "${cflags[@]}" -c "$c" -o "$o" &
      pids+=($!)
      if [ "${#pids[@]}" -ge "${KERN_JOBS:-$(nproc)}" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
    fi
  done
  for p in "${pids[@]}"; do wait "$p"; done
fi

includes=(-I"$here/include")
[ "$with_physics" = 1 ] && includes+=(-I"$here/third_party/box3d/include")

mkdir -p "$(dirname "$out")"
"$CXX" "${target[@]}" \
  -std=c++20 "${opt[@]}" "${features[@]}" \
  -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
  "${includes[@]}" "$@" "${sources[@]}" "${objects[@]}" \
  -mexec-model=reactor \
  -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=1048576 \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=2147483648 \
  -o "$out"
echo "built $out ($(wc -c < "$out") bytes)"
