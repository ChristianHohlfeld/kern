#!/usr/bin/env python3
"""The book's cover for an Amazon KDP hardcover (case laminate): back, spine and front in one PDF.

    python3 kern/tools/buch_umschlag.py --pages 452 [--en] [--out kern-buch-umschlag.pdf]

The cover's size follows from the trim size and the page count:
  width  = 2 x trim width + spine + 2 x hinge + 2 x (wrap + overhang)
  height = trim height + 2 x (wrap + overhang)
  spine  = pages x paper + boards
with KDP's case-laminate allowances (wrap 0.591 in, hinge 0.197 in, board overhang 0.118 in) and
white paper (0.002252 in a page) plus 0.19 in for the boards. Check the numbers against the cover
template KDP offers for the final page count before uploading: --spine sets the spine directly.
Text, logo and frame are vector; the screenshot is embedded at its full resolution.
"""
import argparse, base64, pathlib, sys

here = pathlib.Path(__file__).resolve().parent
img_dir = here.parent / "docs" / "buch" / "bilder"

ap = argparse.ArgumentParser()
ap.add_argument("--pages", type=int, required=True)
ap.add_argument("--en", action="store_true")
ap.add_argument("--out", default=None)
ap.add_argument("--trim", default="7x10", help="trim size in inches")
ap.add_argument("--spine", type=float, default=0, help="spine width in inches (default: from the page count)")
ap.add_argument("--image", default="studio.png", help="the front picture (in kern/docs/buch/bilder); no duck on the cover of a book that is sold: the robot is Pollen Robotics'")
a = ap.parse_args()

W, H = (float(x) for x in a.trim.split("x"))
WRAP, HINGE, OVER = 0.591, 0.197, 0.118
M = WRAP + OVER
S = a.spine or a.pages * 0.002252 + 0.19
TW, TH = 2 * W + S + 2 * HINGE + 2 * M, H + 2 * M
out = pathlib.Path(a.out or ("kern-book-cover.pdf" if a.en else "kern-buch-umschlag.pdf")).resolve()

def data(name):
    p = img_dir / name
    if not p.exists(): p = img_dir / "studio.png"
    return "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode()

logo = f'<img src="{data("kern-logo.png")}" alt="Kern">'  # sRGB raster: the same in every viewer
de = not a.en
t = {
    "book": "Das Buch" if de else "The Book",
    "sub": ("Die Engine, Kern Studio, ein Laufroboter und der Lehrer" if de
            else "The engine, Kern Studio, a walking robot and the Lehrer"),
    "tag": ("Verstehen, benutzen, weiterentwickeln" if de else "Understand it, use it, build on it"),
    "blurb": ("""<p><b>Kern</b> ist eine 3D-Engine in C++20, die im Browser läuft: WebAssembly und WebGL2, Physik mit Box3D,
eine eigene Sound-Engine, Logik-Graphen mit JIT und Netzwerkspiel. Dazu <b>Kern Studio</b>, ein Editor im Stil der großen Engines,
und <b>der Lehrer</b>, ein Trainingssystem für neuronale Netze in C++ – nativ und im Browser, bit-identisch.</p>
<p>Mit ihm lernt ein kleiner zweibeiniger Laufroboter, auf den Original-Netzen seines Herstellers zu laufen, zu springen,
einen Ball zu kicken und eine Socke zu holen.</p>
<p>Dieses Buch erklärt das Ganze von Grund auf: wie die Engine aufgebaut ist und warum, wie man sie bedient und eigene Spiele baut,
wie Rendering, Physik und neuronale Netze funktionieren – und wie man eigene Modelle trainiert.</p>
<p><b>Christian Heinrich Hohlfeld</b>, Softwareentwickler seit 2010, hat Kern entworfen und mit KI gebaut.</p>
<p>Die Software ist Open Source und kostenlos: <b>christianhohlfeld.github.io/kern</b></p>""" if de else
"""<p><b>Kern</b> is a 3D engine in C++20 that runs in the browser: WebAssembly and WebGL2, physics with Box3D,
its own sound engine, logic graphs with a JIT and networked play. With it come <b>Kern Studio</b>, an editor in the style of the big engines,
and <b>the Lehrer</b>, a training system for neural networks in C++ – native and in the browser, bit for bit identical.</p>
<p>With it a small two-legged walking robot learns, on its maker's original networks, to walk, to jump,
kick a ball and fetch a sock.</p>
<p>This book explains it all from the ground up: how the engine is built and why, how to use it and build your own games,
how rendering, physics and neural networks work – and how to train your own models.</p>
<p><b>Christian Heinrich Hohlfeld</b>, a software engineer since 2010, designed Kern and built it with AI.</p>
<p>The software is open source and free: <b>christianhohlfeld.github.io/kern</b></p>"""),
}
html = f"""<!doctype html><html><head><meta charset="utf-8"><style>
@page {{ size: {TW}in {TH}in; margin: 0 }}
html, body {{ margin: 0; padding: 0 }}
body {{ width: {TW}in; height: {TH}in; position: relative; overflow: hidden;
  background: radial-gradient(ellipse at 75% 30%, #1b3358 0%, #0d1830 55%, #070d1a 100%);
  font-family: 'DejaVu Sans', sans-serif; color: #e8eef8 }}
.grid {{ position: absolute; inset: 0; opacity: .07;
  background-image: linear-gradient(#9cc4ff 1px, transparent 1px), linear-gradient(90deg, #9cc4ff 1px, transparent 1px);
  background-size: 0.5in 0.5in }}
.panel {{ position: absolute; top: {M}in; height: {H}in }}
.back {{ left: {M}in; width: {W}in }}
.spine {{ left: {M + W + HINGE}in; width: {S}in }}
.front {{ left: {M + W + HINGE + S + HINGE}in; width: {W}in }}
.front .logo img {{ width: 1.15in; height: 1.15in }}
.front .logo {{ position: absolute; left: 0.75in; top: 0.85in }}
.front h1 {{ position: absolute; left: 0.75in; top: 2.05in; margin: 0; font-size: 92pt; letter-spacing: -2pt; font-weight: bold; color: #fff }}
.front .book {{ position: absolute; left: 0.8in; top: 3.45in; font-size: 24pt; color: #9cc4ff }}
.front .sub {{ position: absolute; left: 0.8in; right: 0.6in; top: 4.0in; font-size: 14pt; line-height: 1.35; color: #d5e2f5 }}
.front .shot {{ position: absolute; left: 0.75in; right: 0.55in; top: 5.05in; height: 3.45in; border-radius: 10px; overflow: hidden;
  box-shadow: 0 0 0 1.5px #3d8bff, 0 10px 40px rgba(0,0,0,.6) }}
.front .shot img {{ display: block; width: 100%; height: 100%; object-fit: cover; object-position: 50% 60% }}
.front .author {{ position: absolute; left: 0.8in; bottom: 0.75in; font-size: 17pt; color: #fff }}
.front .tag {{ position: absolute; left: 0.8in; bottom: 0.45in; font-size: 10pt; color: #9cc4ff; letter-spacing: .5pt }}
.spine .inner {{ position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%) rotate(90deg);
  white-space: nowrap; font-size: {min(16, S * 72 * 0.42):.1f}pt; display: flex; align-items: center; gap: 0.25in }}
.spine .inner b {{ color: #fff }} .spine .inner span {{ color: #9cc4ff }}
.spine .mark {{ position: absolute; left: 50%; bottom: 0.45in; transform: translateX(-50%) }}
.spine .mark img {{ width: {min(0.6, S * 0.7):.2f}in; height: {min(0.6, S * 0.7):.2f}in }}
.back .logo img {{ width: 0.55in; height: 0.55in }}
.back .logo {{ position: absolute; left: 0.75in; top: 0.85in }}
.back .title {{ position: absolute; left: 1.45in; top: 0.9in; font-size: 20pt; font-weight: bold }}
.back .title span {{ font-weight: normal; color: #9cc4ff }}
.back .blurb {{ position: absolute; left: 0.75in; right: 0.65in; top: 1.75in; font-size: 11pt; line-height: 1.5; color: #dbe5f4 }}
.back .blurb b {{ color: #fff }}
.back .shots {{ position: absolute; left: 0.75in; right: 0.65in; top: 5.85in; display: flex; gap: 0.18in }}
.back .shots img {{ width: 50%; border-radius: 6px; box-shadow: 0 0 0 1px #3d8bff }}
.back .rights {{ position: absolute; left: 0.75in; bottom: 0.55in; width: 3.6in; font-size: 7.5pt; line-height: 1.4; color: #8ea3c4 }}
.back .barcode {{ position: absolute; right: 0.55in; bottom: 0.45in; width: 2in; height: 1.2in; background: #fff }}
</style></head><body><div class="grid"></div>
<div class="panel back">
  <div class="logo">{logo}</div>
  <div class="title">Kern <span>– {t["book"]}</span></div>
  <div class="blurb">{t["blurb"]}</div>
  <div class="shots"><img src="{data('studio-selected.png')}"><img src="{data('duck-play.png')}"></div>
  <div class="rights">© 2026 Christian Heinrich Hohlfeld · ORCID 0009-0003-6634-9045<br>
  {"Alle Rechte am Buch vorbehalten. Die Software Kern ist Open Source (Apache 2.0). Kein Produkt von Pollen Robotics oder Hugging Face und nicht von ihnen geprüft." if de else "Book: all rights reserved. The Kern software is open source (Apache 2.0). Not a product of Pollen Robotics or Hugging Face, nor reviewed by them."}</div>
  <div class="barcode"></div>
</div>
<div class="panel spine"><div class="inner"><b>Kern</b><span>{t["book"]}</span><span style="color:#d5e2f5">Christian Heinrich Hohlfeld</span></div>
  <div class="mark">{logo}</div></div>
<div class="panel front">
  <div class="logo">{logo}</div>
  <h1>Kern</h1>
  <div class="book">{t["book"]}</div>
  <div class="sub">{t["sub"]}</div>
  <div class="shot"><img src="{data(a.image)}"></div>
  <div class="author">Christian Heinrich Hohlfeld</div>
  <div class="tag">{t["tag"]}</div>
</div></body></html>"""

work = out.with_suffix(".html")
work.write_text(html)
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b = p.chromium.launch()
    page = b.new_page()
    page.goto(work.as_uri())
    page.pdf(path=str(out), width=f"{TW}in", height=f"{TH}in", print_background=True, prefer_css_page_size=True)
    b.close()
work.unlink()
print(f"{out}: {TW:.3f} x {TH:.3f} in ({TW * 25.4:.1f} x {TH * 25.4:.1f} mm), spine {S:.3f} in ({S * 25.4:.1f} mm) for {a.pages} pages")
