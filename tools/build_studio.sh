#!/usr/bin/env bash
# Builds Kern Studio as one self-contained HTML file.
#
#   tools/build_studio.sh [--skip-wasm] [out.html]     (default: generated/studio.html)
#
# Steps:
#   1. kern/build.sh --studio generated/kern_studio.wasm   (C++ engine, Box3D, game runtime, studio)
#   2. inline kern/web/studio/studio.css, the module sources kern/web/kern.mjs,
#      kern/web/studio/game.mjs and kern/web/studio/studio.mjs and the wasm
#      (base64) into the markup of kern/web/studio/index.html.
#
# The result runs from file:// or any static host without network access.
# The page runs the three module sources as one inline module; kern.mjs and
# game.mjs are also embedded as text, from which packaged games are written.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skip_wasm=0
if [ "${1:-}" = "--skip-wasm" ]; then skip_wasm=1; shift; fi
out="${1:-$root/generated/studio.html}"
wasm="$root/generated/kern_studio.wasm"
mkdir -p "$(dirname "$out")" "$root/generated"
if [ "$skip_wasm" = 0 ]; then "$root/kern/build.sh" --studio ${KERN_COMMERCIAL:+--commercial} "$wasm"; fi
[ -s "$wasm" ] || { echo "missing $wasm (run without --skip-wasm)" >&2; exit 1; }

python3 - "$root" "$wasm" "$out" <<'PY'
import base64, re, sys, pathlib
root, wasm, out = map(pathlib.Path, sys.argv[1:4])
web = root / "kern/web"
html = (web / "studio/index.html").read_text()
css = (web / "studio/studio.css").read_text()
sources = {
    "kern": (web / "kern.mjs").read_text(),
    "game": (web / "studio/game.mjs").read_text(),
    "logic": (web / "studio/logic.mjs").read_text(),
    "studio": (web / "studio/studio.mjs").read_text(),
}
for name, text in sources.items():
    if re.search(r"</script", text, re.I):
        sys.exit(f"{name} source contains '</script'; cannot inline it")
b64 = base64.b64encode(wasm.read_bytes()).decode()

html = html.replace('<link rel="stylesheet" href="./studio.css">', "<style>\n" + css + "\n</style>")
# Module sources stay embedded as text: packaged games are written from them.
blocks = "".join(f'<script type="text/plain" id="kern-src-{n}">{t}</script>\n' for n, t in sources.items() if n in ("kern", "game"))
blocks += f'<script type="text/plain" id="kern-wasm">{b64}</script>\n'

# The page runs one inline module: the sources concatenated with their import
# lines and export keywords removed (no blob: imports, so strict content
# security policies that forbid them still work).
def strip(text):
    text = re.sub(r'^import \{[^}]*\} from "[^"]+";\n', "", text, flags=re.M)
    return re.sub(r"^export (?=(const|let|function|async function|class) )", "", text, flags=re.M)
bundle = "\n".join(f"// ---- {n} ----\n" + strip(sources[n]) for n in ("kern", "game", "logic", "studio"))
boot = """<script type="module">
""" + bundle + """
// ---- single-file boot ----
{
  const text = id => document.getElementById(id).textContent;
  window.KERN_SOURCES = {kern: text("kern-src-kern"), game: text("kern-src-game")};
  startStudio({base64: text("kern-wasm").trim()}).catch(error => {
    const n = document.getElementById("engineNotice");
    n.style.display = "block";
    n.textContent = "Kern Studio failed to start: " + error.message;
    console.error(error);
  });
}
</script>"""
start = html.index('<script type="module">')
end = html.index("</script>", start) + len("</script>")
html = html[:start] + blocks + boot + html[end:]
out.write_text(html)
print(f"{out}: {out.stat().st_size:,} bytes (wasm {wasm.stat().st_size:,} bytes)")
PY
