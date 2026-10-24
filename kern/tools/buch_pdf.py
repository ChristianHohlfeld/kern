#!/usr/bin/env python3
"""Sets the book (kern/docs/buch, or kern/docs/book) as one print-ready PDF.

    python3 kern/tools/buch_pdf.py [--en] [--out kern-buch.pdf] [--format 170x240]

Needs pandoc, xelatex (with German hyphenation for the German book) and mmdc (Mermaid CLI).
The chapters are read in order (00 imprint, 01-17, appendices A-C), Mermaid diagrams are
rendered to vector PDFs, links between chapters become links inside the book, and the result is
set with KOMA-Script (scrbook): chapters open on the right, running heads, table of contents.
"""
import argparse, pathlib, re, shutil, subprocess, sys, tempfile

here = pathlib.Path(__file__).resolve().parent
docs = here.parent / "docs"

ap = argparse.ArgumentParser()
ap.add_argument("--en", action="store_true", help="the English book (kern/docs/book)")
ap.add_argument("--out", default=None)
ap.add_argument("--format", default="177.8x254", help="trim size in mm, width x height (default: Amazon KDP hardcover 7 x 10 in)")
ap.add_argument("--keep", action="store_true", help="keep the intermediate files (prints where)")
a = ap.parse_args()

src = docs / ("book" if a.en else "buch")
out = pathlib.Path(a.out or ("kern-book.pdf" if a.en else "kern-buch.pdf")).resolve()
w, h = (float(x) for x in a.format.lower().split("x"))
files = sorted(p for p in src.glob("*.md") if re.match(r"^(\d\d|[A-Z])-", p.name))
files = [p for p in files if p.name[0].isdigit()] + [p for p in files if p.name[0].isalpha()]
if not files: sys.exit(f"no chapters in {src}")

work = pathlib.Path(tempfile.mkdtemp(prefix="kern-buch-"))
(work / "fig").mkdir()
ids = {p.name: "kap-" + p.stem.split("-")[0].lower() for p in files}
mermaid_n = 0

def mermaid(code):
    """A Mermaid diagram as a print-resolution PNG (4x), set at its natural size or narrower."""
    global mermaid_n
    mermaid_n += 1
    mmd, png = work / "fig" / f"m{mermaid_n}.mmd", work / "fig" / f"m{mermaid_n}.png"
    mmd.write_text(code)
    cfg = work / "fig" / "mmdc.json"
    cfg.write_text('{"theme":"neutral","themeVariables":{"fontFamily":"DejaVu Sans"}}')
    for attempt in range(3):  # headless Chromium now and then fails to start on a busy machine
        if subprocess.run(["mmdc", "-q", "-i", str(mmd), "-o", str(png), "-s", "4", "-b", "white", "-c", str(cfg)]).returncode == 0: break
    else: sys.exit(f"mmdc failed on diagram {mermaid_n}:\n{code}")
    from PIL import Image
    pw, ph = Image.open(png).size
    text_w, max_h = w - 39.0, 185.0                 # mm: the text block's width, the tallest a diagram may be
    natural = pw / 4 / 96 * 25.4 * 0.85             # mm at 96 dpi, a little smaller (the book's type is smaller than the web's)
    width = min(text_w, natural, max_h * pw / ph)
    return f"![]({png}){{width={width:.1f}mm}}"

parts = []
for i, p in enumerate(files):
    t = p.read_text()
    t = re.sub(r"\n---\n\n© 20\d\d Christian Heinrich Hohlfeld[^\n]*\n?$", "\n", t)  # the per-chapter notice (the imprint has it)
    t = re.sub(r"```mermaid\n(.*?)```", lambda m: mermaid(m.group(1)), t, flags=re.S)
    # Code blocks without a language: plain text (set by the verbatim environment, wrapped).
    # Images: from the chapter's folder; missing ones (screenshots still to come) are left out.
    def img(m):
        alt, path = m.group(1), m.group(2)
        f = (src / path).resolve()
        if not f.exists():
            print("missing image", path, file=sys.stderr)
            return ""
        return f"![{alt}]({f}){{width={'45mm' if f.name.startswith('autor') else '100%'}}}"
    t = re.sub(r"!\[([^\]]*)\]\(((?:\.\./buch/)?bilder/[^)\s]+)\)", img, t)
    # Links to other chapters (with or without an anchor) point into the book.
    def link(m):
        text, target, anchor = m.group(1), m.group(2), m.group(3)
        if target in ids: return f"[{text}](#{anchor[1:] if anchor else ids[target]})"
        return m.group(0)
    t = re.sub(r"\[([^\]]+)\]\(([0-9A-Z][^)#\s]*\.md)(#[^)\s]+)?\)", link, t)
    if p.name.startswith("00"):  # in print: the imprint alone (the title page and the table of contents are the book's)
        t = re.sub(r"\A.*?^## ", lambda m: "```{=latex}\n\\thispagestyle{empty}\\footnotesize\n```\n\n## ", t, count=1, flags=re.S | re.M)
        t = re.sub(r"^## (.+)$", r"## \1 {.unnumbered}", t, flags=re.M)
        t = re.sub(r"^## (Inhalt|Contents)\b.*\Z", "", t, flags=re.S | re.M)
    # The chapter's first heading carries its id; the imprint and the appendices are not numbered.
    first = None if p.name.startswith("00") else re.search(r"^# (.+)$", t, flags=re.M)
    if first:
        attrs = "{#" + ids[p.name] + (" .unnumbered" if p.name.startswith("00") else "") + "}"
        head = first.group(1)
        short = head
        if len(short) > 44:  # the running head has one line
            cut = max(short.rfind(sep, 0, 44) for sep in (",", " – ", " ("))
            short = short[:cut] if cut > 10 else short[:44]
        mark = "" if short == head else "\n\n```{=latex}\n\\chaptermark{" + short.replace("&", "\\&") + "}\n```\n"
        t = t[:first.start()] + f"# {head} {attrs}" + mark + t[first.end():]
    if p.name[0].isalpha() and (i == 0 or files[i - 1].name[0].isdigit()):
        t = "```{=latex}\n\\appendix\n```\n\n" + t
    if p.name.startswith("00"):
        t = t + "\n\n```{=latex}\n\\normalsize\n\\tableofcontents\n\\kernmainmatter\n```\n"
    parts.append(t)

(work / "buch.md").write_text("\n\n".join(parts))
lang = "en" if a.en else "de"
header = work / "header.tex"
logo_tex = work / "kern_logo.tex"
logo_tex.write_text((here / "kern_logo.tex").read_text().replace("KERNLOGO", (docs / "buch" / "bilder" / "kern-logo.png").as_posix()))
header.write_text(r"""
\usepackage{polyglossia}
\setmainlanguage%s
\setmonofont{DejaVu Sans Mono}[Scale=0.82]
\setsansfont{DejaVu Sans}[Scale=0.9]
\usepackage{fvextra}
\DefineVerbatimEnvironment{verbatim}{Verbatim}{breaklines,breakanywhere,fontsize=\footnotesize}
\RecustomVerbatimEnvironment{Highlighting}{Verbatim}{commandchars=\\\{\},breaklines,breakanywhere,fontsize=\footnotesize}
\usepackage[headsepline]{scrlayer-scrpage}
\pagestyle{scrheadings}
\automark[section]{chapter}
\setcounter{tocdepth}{1}
\setcounter{secnumdepth}{-1}  %% the headings carry their own numbers
\renewcommand{\arraystretch}{1.15}
\setlength{\emergencystretch}{3em}
\input{%s}
\let\kernmainmatter\mainmatter
\renewcommand{\mainmatter}{}  %%%% pandoc says it before the body; the book says it after the contents
\frontmatter
""" % ("[variant=british]{english}" if a.en else "[spelling=new]{german}", logo_tex.as_posix()))
title = "Kern – The Book" if a.en else "Kern – Das Buch"
titlepage = work / "titlepage.tex"
sub = ("The engine, Kern Studio, a walking robot and the Lehrer" if a.en
       else "Die Engine, Kern Studio, ein Laufroboter und der Lehrer")
titlepage.write_text(r"""
\thispagestyle{empty}
\begin{center}
\vspace*{18mm}
\kernlogo{34mm}\par
\vspace{14mm}
{\sffamily\bfseries\fontsize{46}{50}\selectfont Kern\par}
\vspace{4mm}
{\sffamily\Large %s\par}
\vspace{10mm}
{\large %s\par}
\vfill
{\Large Christian Heinrich Hohlfeld\par}
\vspace{2mm}
{\small ORCID 0009-0003-6634-9045\par}
\vspace{12mm}
\end{center}
\newpage
""" % ("The Book" if a.en else "Das Buch", sub))
cmd = ["pandoc", str(work / "buch.md"), "-o", str(out),
       "-f", "markdown+pipe_tables+tex_math_dollars+raw_attribute+autolink_bare_uris-yaml_metadata_block-implicit_figures",
       "--pdf-engine=xelatex", "--top-level-division=chapter", "--toc-depth=1",
       "-V", "documentclass=scrbook", "-V", "classoption=openright,twoside,fontsize=10pt,headings=normal",
       "-V", f"geometry=paperwidth={w}mm,paperheight={h}mm,inner=22mm,outer=17mm,top=20mm,bottom=24mm",
       "-V", "fontfamily=fontspec", "-V", "mainfont=DejaVu Serif", "-V", "mainfontoptions=Scale=0.92", "-V", "mathfont=DejaVu Math TeX Gyre", "-V", "colorlinks=true", "-V", "linkcolor=black", "-V", "urlcolor=black",
       "-V", f"title-meta={title}", "-V", "author-meta=Christian Heinrich Hohlfeld", "-B", str(titlepage),
       "-H", str(header), "--resource-path", str(src)]
r = subprocess.run(cmd, cwd=work)
if r.returncode: sys.exit(f"pandoc failed (intermediate files in {work})")
pages = subprocess.run(["pdfinfo", str(out)], capture_output=True, text=True).stdout
pages = re.search(r"Pages:\s+(\d+)", pages)
print(f"{out}: {pages.group(1) if pages else '?'} pages, {mermaid_n} diagrams, {w} x {h} mm")
if a.keep: print("intermediate files:", work)
else: shutil.rmtree(work)
