#!/usr/bin/env bash
# Builds a Kern WebAssembly module: the engine plus the game sources given.
#
#   kern/build.sh <out.wasm> [game sources...]
#
# Toolchain: wasi-sdk (clang + wasm-ld + wasi-libc + libc++). Set WASI_SDK to
# its install directory; the CI workflow downloads release 25 from GitHub.
# The module is a WASI "reactor": the host calls _initialize once, then the
# exported kern_* / game functions. No Emscripten runtime is involved.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:?usage: kern/build.sh <out.wasm> [game sources...]}"
shift || true
WASI_SDK="${WASI_SDK:-/opt/wasi-sdk}"
CXX="$WASI_SDK/bin/clang++"
[ -x "$CXX" ] || { echo "wasi-sdk clang++ not found at $CXX (set WASI_SDK)" >&2; exit 1; }

mode="${KERN_BUILD:-release}"
if [ "$mode" = debug ]; then opt=(-O1 -g); else opt=(-O3 -flto -DNDEBUG); fi

# Conservative feature set: runs on every WebGL2-capable browser (Safari 15+).
features=(-mcpu=mvp -msign-ext -mbulk-memory -mnontrapping-fptoint -mmutable-globals)

sources=()
while IFS= read -r f; do sources+=("$f"); done < <(find "$here/src" -name '*.cpp' | sort)

mkdir -p "$(dirname "$out")"
"$CXX" --target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot" \
  -std=c++20 "${opt[@]}" "${features[@]}" \
  -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
  -I"$here/include" "$@" "${sources[@]}" \
  -mexec-model=reactor \
  -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=1048576 \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=2147483648 \
  -o "$out"
echo "built $out ($(wc -c < "$out") bytes)"
