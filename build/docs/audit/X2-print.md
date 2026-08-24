# X2 — Print path + resource-exhaustion / hang audit

Auditor: agent X2. Read-only on the repository except this file.
Environment: Arch Linux, Qt 6.11.2, poppler-qt6 26.08.0, gcc/C++20,
7119 MB RAM + 36 GB swap (3.5G zram + 32G /dev/sda3), 8 cores,
`vm.overcommit_memory=0` (heuristic). No CUPS printers configured
(`lpstat -a` → "No destinations added").

Build under test: `<repo>/build/mergen`, branch `ata`,
HEAD `472183b M7: PKGBUILD and release workflow`.

Legend: **[M]** = measured on this machine. **[R]** = assessed by reading the
code, not executed.

---

## Summary — findings, most severe first

Every row was measured on this machine unless marked [R]. Section numbers link
to the evidence.

| # | § | Sev | Finding | Measured cost |
|---|---|---|---|---|
| 1 | §2 | **critical** | Poppler returns a **1×1 non-null** `QImage` when it refuses an allocation; every `isNull()` guard in MERGEN misses it. **An A0 page prints as a sheet of solid black.** | threshold exactly 2^31 B; A0 print = 1×1 image at 0.125 ppi, 2 colours on the sheet, 158 ms, exit 0 |
| 2 | §11 | **critical** | `kMinZoom = 0.10` overrides "fit width", so a large MediaBox forces a huge render **on open** | **793-byte file → 2 081 MB RSS / 2 585 MB peak** |
| 3 | §6, §8 | **critical** | Ctrl+I `Document::properties()` walks `fonts()` + every page's annotations on the GUI thread, uncached | **26.3 s** on a 608 KB 1-page file; **260.2 s** on an 8.8 MB file; **90.8 s** again on every repeat |
| 4 | §7a | **critical** | The print loop blocks the event loop entirely — no progress, no cancel, no repaint | **346 s frozen** for 1 000 pages, **0** of ~1 385 expected timer ticks |
| 5 | §10 | high | Print-to-file will **overwrite the document being read**; `outputFileName()` is never checked | 3 874 B 10-page source → 956 628 B 1-page raster; reopens with 1 page |
| 6 | §4a, §4b | high | "Current Page" and "Selection" in the print dialog print the **whole document** | 10 pages / 10.3 MB emitted for a 1-page request |
| 7 | §7b | high | Every page is spooled as a 600 dpi raster | **1.04 GB output from a 346 KB source** — 3 000× inflation, 1.04 MB/page |
| 8 | §12, §12a | high | `computeDiff()` renders every page of both documents up front on the GUI thread — and reports 20 000 changed pages as **identical** | **7.4 s** (1 000 pp), **88.8 s** (20 000 pp), 40 000 refused renders |
| 9 | §9 | high | Unbounded recursion in `flattenOutline` — **already reported by X1 as F-03**; corroborated here with the exact frame cost | **224 B/level** ⇒ SIGSEGV at ≈37 400 levels; 12.3 s freeze at 10 000 |
| 10 | §5a | medium | Rotation is honoured but paper orientation is not, so a rotated print uses half the sheet | 621 ppi unrotated vs **879 ppi** rotated = 70.7% linear / 50% area |
| 11 | §4c | medium | `from` is never bounds-checked (only `to` is) | pages 50–60 of a 10-page doc → **one blank sheet**, 3 075 B, silent |
| 12 | §4d | medium | The dialog's free-text "Pages" range (`pageRanges()`) is never read | field enabled with a real CUPS queue; all-pages job spooled for a "2-3" request |
| 13 | §13 | medium | `contentHash()` hashes the whole file on the GUI thread | **1 913 ms** cold on 500 MB (RSS flat — it does stream) |
| 14 | §16 | medium | `m_words` / `m_links` are never trimmed, unlike the page cache | **+31.3 MB** over 300 drag-selected pages, ≈200 KB/page, never released |
| 15 | §10a | low | A print that produces nothing says nothing (`painter.begin()` failure is a silent `return`) | — |
| 16 | §14a | nit | `paintSearchHits` is linear in the document's total hits, per page, per frame | +0.66 ms/frame at 122 000 hits |
| 17 | §17 | nit | `okToPrint()` is reported in the properties panel and ignored by the printer | [R] |

### Verified sound — no finding

| § | What was checked | Verdict |
|---|---|---|
| §1, §5 | All five assertions of the `MX.md:369` printing paragraph | **All true.** Printer resolution is used (4958 × 7017 = exactly 595 pt × 600/72), the cap bites (printer reports 1200, raster comes out 600), aspect ratio is kept, rotation is followed. The doc's "550 MB per A4 at 1200 dpi" arithmetic checks out at **556.8 MB**. |
| §5b, §7c | Print-loop memory | **Bounded and flat.** 235.4 MB at 10 pages, 235.4 MB at 100, 237.5 MB at 1 000. No leak. |
| §11 | Page-cache bound and zoom ceiling | **Both sound.** `dropPagesOutside` really does track the viewport; A4 at the 1000% maximum ends at 302.4 MB. Off-screen margin pages are never pre-rendered. |
| §14 | The search worker under 122 000 hits | **Sound, and the model the rest should follow.** Worst event-loop gap **5 ms**, +12.6 MB total, progress bar and Cancel both wired correctly. |
| §15 | "`linkAt` runs on every mouse move" | **Premise is wrong.** `mouseMoveEvent` never calls it; only `mousePressEvent` does. Move cost is flat at 0.0020 ms from 400 to 20 000 links. |
| §13 | `contentHash` memory | **Streams correctly.** RSS flat at 41.5 MB while hashing 500 MB, and memoised. |

### The requested table — one 1 000-page A4 document (`p1000.pdf`, 346 KB)

| operation | wall time | event loop alive? | peak RSS | side effect |
|---|---|---|---|---|
| `openPath()` + `show()` | 24 ms | yes | 72.2 MB | — |
| `contentHash()` (portal) | 1 ms | frozen | 41.7 MB | memoised |
| `Document::properties()` (Ctrl+I) | 70 ms | frozen | 47.5 MB | recomputed every time |
| `Document::outline()` (Ctrl+T) | <1 ms | frozen | 47.5 MB | — |
| search, 1 000 hits | ~1 s | **yes, 2 ms worst gap** | 74.5 MB | — |
| `enterCompare()` + `computeDiff()` | **7 375 ms** | **frozen** | 74.9 MB | — |
| zoom fit → 1000% (81 steps) | 131 ms/step | yes | 302.4 MB | — |
| **print all pages** | **346 234 ms** | **frozen, 0 ticks** | 237.5 MB | **1.04 GB file** |

---

## 0. The constitutional claim

The 600 dpi paragraph is **not** in `MZ.md`. It is in `MX.md` §8 (v1.0, sealed),
lines 369–375, and `MZ.md` §9 line 408 imports it by reference:

> "Everything in `MX.md` §8 — the rendering model, zoom and fit behaviour,
> selection, search threading, elevation, **printing** — remains correct and is
> not restated."

`MX.md:369`:

> **Printing.** `QPrintDialog` supplies the page range; MERGEN honours it and
> renders each selected page at the printer's resolution rather than reusing the
> screen cache, then fits it to the paper keeping its proportions. The resolution
> is capped at 600 dpi: a printer reporting 1200 dpi would otherwise mean a
> 550 MB image per A4 page and a spool file to match, and nothing on the paper is
> better for it. Printing follows the rotation currently on screen, so what is
> printed is what is being looked at.

Five checkable assertions. Verdicts are filled in below as they are measured.

---

## 1. The 600 dpi cap — arithmetic

Code: `<repo>/src/mainwindow.cpp:67`

```cpp
constexpr int kPrintDpiCap = 600;
```

`<repo>/src/mainwindow.cpp:1505`

```cpp
const double dpi = qMin<double>(printer.resolution(), kPrintDpiCap);
const double scale = dpi / 72.0;
```

`<repo>/src/document.cpp:352-362` — `renderPage` multiplies back up
(`dpi = 72.0 * scale`) and calls `page->renderToImage(dpi, dpi, -1,-1,-1,-1, rotation)`.
Poppler returns `QImage::Format_ARGB32` — **4 bytes per pixel** (confirmed
below, `fmt=4`).

### [M] Does the cap actually bite?

`QPrinter(QPrinter::HighResolution)` on this machine, with no printer installed:

| property | value |
|---|---|
| `availablePrinterNames()` | `[]` (empty) |
| `defaultPrinterName()` | `''` |
| `resolution()` | **1200** |
| `outputFormat()` | `PdfFormat` (1) |
| `supportedResolutions()` | `72` |
| `QPrinter(ScreenResolution).resolution()` | 96 |

So the exact case the doc names — "a printer reporting 1200 dpi" — is the
**default** case on a machine with no printer, and `qMin(1200, 600)` = 600.
**The cap is applied and it does bite.** Verdict on assertion 3: TRUE, with the
qualification in §2 below.

### [M] Cost per page at the cap, measured

Rendered page 1 of `/tmp/mz/p10.pdf` (A4, 595×842 pt) at 600 dpi through the
same poppler call `printDocument()` uses:

```
rendered 4958x7017 fmt=4 (ARGB32) null=0 sizeInBytes=139161144 (139.2 MB)
in 85 ms; process VmHWM=211.3 MB
```

### [M] The doc's own "550 MB" claim

| page | at 1200 dpi (uncapped) | at 600 dpi (capped) |
|---|---|---|
| A4 595.276 × 841.89 pt | 9921 × 14032 px = **556.8 MB** | 4961 × 7016 px = **139.2 MB** |
| US Letter 612 × 792 pt | 10200 × 13200 px = 538.6 MB | 5100 × 6600 px = 134.6 MB |

**The 550 MB figure in `MX.md` is correct** (556.8 MB, so the doc rounded down
slightly). The cap reduces it 4× to 139.2 MB, exactly as claimed. The arithmetic
in the constitution is sound.

### [M] …but the cap is on DPI, and the AREA is unbounded

`scale` is fixed at 8.333 px/pt; the pixel count is `W_pt · H_pt · scale²`, and
nothing clamps `W_pt · H_pt`. Same 600 dpi cap, larger MediaBox:

| MediaBox | px at the 600 dpi cap | ARGB32 bytes |
|---|---|---|
| A4 595×842 pt | 4 961 × 7 016 | 139 MB |
| **A0 2384×3370 pt** (a real poster/CAD size) | 19 867 × 28 083 | **2.23 GB** |
| 200 in square, 14400×14400 pt (PDF/Acrobat max) | 120 000 × 120 000 | **57.6 GB** |
| MediaBox `[0 0 1e8 1e8]` (malformed, accepted by poppler) | 8.3e8 × 8.3e8 | **2.8 EB** |

That is the gap between the doc and the code: the constitution says the cap
stops a half-gigabyte image, and for A4 it does. It says nothing about page
area, and the code bounds nothing else. A single A0 page — an ordinary
engineering drawing or conference poster — costs **2.23 GB in one allocation**
on a 7 GB machine, sixteen times what the paragraph budgets for.

*(Detail: measurements of what actually happens on those allocations follow in
§3.)*

---

## 2. [M] CRITICAL — poppler does not return a null image when the allocation is refused; it returns a **1×1** one, and every `isNull()` guard in MERGEN misses it

`src/mainwindow.cpp:1518-1521`

```cpp
const QImage image = m_doc->renderPage(page - 1, scale, rotation);
if (image.isNull()) {
    continue;
}
```

Poppler's `gmallocn()` refuses any single allocation of `rowSize × height ≥ INT_MAX`
(2 147 483 647 B). `SplashOutputDev::startPage` catches that and **falls back to a
1×1 bitmap**, so `Poppler::Page::renderToImage()` hands back a perfectly valid,
non-null, 1-pixel image. The only outward sign is the string
`Bogus memory allocation size` on **stderr**, which a GUI reader never sees.

### Measured threshold (probe `/tmp/mz/probe2`, real `mergen::Document::renderPage`, scale = 600/72)

| MediaBox (square) | px at 600 dpi | ARGB32 bytes | result | elapsed | peak RSS |
|---|---|---|---|---|---|
| 595 × 842 (A4) | 4 958 × 7 017 | 139 161 144 | **real image** | 94 ms | 211.8 MB |
| 2700 × 2700 pt | 22 500 × 22 500 | 2 025 000 000 | **real image** | 983 ms | **2460.1 MB** |
| 2760 × 2760 pt | 23 000 × 23 000 | 2 116 000 000 | **real image** | 1060 ms | **2568.6 MB** |
| 2780 × 2780 pt | 23 167 × 23 167 | 2 146 839 556 | **real image** | 1057 ms | **2605.3 MB** |
| 2800 × 2800 pt | 23 333 × 23 333 | 2 177 715 556 | **1×1, not null** | 15 ms | 46.1 MB |
| 3000 × 3000 pt | 25 000 × 25 000 | 2 500 000 000 | **1×1, not null** | 15 ms | 46.1 MB |
| 2384 × 3370 (**A0**) | 19 867 × 28 083 | 2 231 678 244 | **1×1, not null** | 19 ms | 45.8 MB |
| 5000 × 5000 pt | 41 667 × 41 667 | 6.9 GB | **1×1, not null** | 16 ms | 45.8 MB |
| 14400 × 14400 (200 in) | 120 000 × 120 000 | 57.6 GB | **1×1, not null** | — | — |

The boundary sits exactly at 2^31 bytes, between 2 146 839 556 (works) and
2 177 715 556 (refused).

**Consequence in the print path.** `image.isNull()` is false for the 1×1, so
control falls through to:

```cpp
QSize target = image.size();              // 1 x 1
target.scale(paper.size(), Qt::KeepAspectRatio);
painter.drawImage(placed, image);         // one pixel stretched over the sheet
```

An **A0 page — an ordinary poster or CAD plot — prints as one solid block of
colour**, filling the paper, with no warning on screen and no non-zero exit
status. That is the worst kind of failure for a print path: it consumes the
paper and the toner and looks like a printer fault rather than a viewer bug.

The same blind spot exists at:
- `src/pageview.cpp:298-302` (`cachedPage` caches the 1×1 and paints it stretched over the page rect)
- `src/mainwindow.cpp:1101-1105` (`computeDiff` — `left.isNull()` false, then `scanLine()` on a 1×1 vs a 1×1)
- `src/mainwindow.cpp:1421-1424` (`peekPage`)

### [M] End-to-end proof: an A0 page prints as a sheet of solid black

Not a projection — measured through the real Print action to a real PDF
(probe `/tmp/mz/probe7`, document `a0x1.pdf`, one A0 page 2384 × 3370 pt):

```
--- accept ---
Bogus memory allocation size                 <- stderr only; no GUI user sees this
=== returned after 158 ms ticks=0 VmHWM=72.7 MB ===
output size=3979
```

```
$ pdfinfo   a0print.pdf   ->  Pages: 1   Page size: 595 x 842 pts (A4)
$ pdfimages -list a0print.pdf
   page  num  type   width height color comp bpc  enc  ...  x-ppi  y-ppi  size
      1    0 image       1      1  rgb     3   8 jpeg  ...  0.125  0.125  629B
```

**A 1 × 1 pixel image placed at 0.125 ppi** — one pixel stretched across the
whole sheet. Rasterising the produced page and counting colours:

```
distinct colours across the whole printed page: 2  ->  [(0,0,0), (255,255,255)]
corner px (0,0,0)   centre px (0,0,0)   other (0,0,0)
```

The placed rectangle is **solid black**; the white is only the unprinted margin
outside it. So the reader who prints an A0 drawing gets **a sheet of solid black
ink**, in 158 ms, with no error, no dialog, and a zero exit status. On a real
printer that is a cartridge.

**Severity: critical** (silent wrong output on a write/print path).
**Fix direction:** reject the render when `image.size()` is not within rounding
distance of `pageSize(index) * scale` — or, better, pre-compute
`w·h·4` and refuse/downscale *before* calling poppler (see §3).

**Note the gap against the constitution.** `MX.md:371` caps the resolution and
says nothing about area; the code does the same. The cap is on DPI, the area is
unbounded, so a legal A0 page is 16× the budget the paragraph was written to
protect and lands past poppler's own ceiling.

---

## 3. [M] The print loop blocks the event loop completely: no progress, no cancellation, no repaint

*(First measurement; severity is set in §7a once the 1 000-page number is in — **critical**.)*

`src/mainwindow.cpp:1514-1530` is a bare `for` loop on the GUI thread. There is
no `QProgressDialog`, no `processEvents()`, no abort flag. Measured with a
250 ms `QTimer` heartbeat installed before `printAction->trigger()`
(probe `/tmp/mz/probe3`, drives the **real** `MainWindow` Print `QAction`, dialog
auto-accepted; nothing about the print path is replicated):

| document | pages printed | wall time | heartbeat ticks (expected) | peak RSS | output size |
|---|---|---|---|---|---|
| `p10.pdf` A4 ×10 | 10 | **3 874 ms** | **0** (≈15) | 235.4 MB | **10 302 315 B (10.3 MB)** |

Zero timer ticks means the event loop never ran once during the print. The
window cannot repaint, the toolbar cannot be clicked, and `Esc` does nothing.
Larger measurements follow.

Note also the *output* size: **10.3 MB for a ten-page text-only A4 document**,
because every page is embedded as a 600 dpi raster. `MX.md:372` worries about
"a spool file to match" at 1200 dpi; at 600 dpi it is still ~1 MB of raster per
page of plain text.

---

## 4. [M] HIGH — "Current Page" and "Selection" in the print dialog silently print the **whole document**

`src/mainwindow.cpp:1495-1501`

```cpp
int from = printer.fromPage();
int to = printer.toPage();
if (from < 1) { // "All pages": the dialog leaves the range at zero.
    from = 1;
    to = m_doc->pageCount();
}
to = qMin(to, m_doc->pageCount());
```

The comment is wrong about *why* the range is zero. `QPrinter::fromPage()` is 0
for **four** distinct dialog states, not one, and MERGEN never reads
`printer.printRange()` (which distinguishes them) or `printer.pageRanges()`
(the modern channel). Measured by driving the real Print `QAction` against the
real `QPrintDialog`, output to PDF so the emitted pages can be counted
(probe `/tmp/mz/probe7`, document `p10.pdf`, 10 × A4):

| dialog choice | `printRange()` | `fromPage()` | `toPage()` | pages MERGEN emitted | output size | wall time |
|---|---|---|---|---|---|---|
| "Print all" | 0 `AllPages` | 0 | 0 | 10 ✓ | 10 302 315 B | 3 217 ms |
| "Pages from" 3 → 5 | 2 `PageRange` | 3 | 5 | 3 ✓ | 3 091 466 B | 1 009 ms |
| **"Current Page"** | **3 `CurrentPage`** | 0 | 0 | **10 ✗** | **10 302 315 B** | 3 161 ms |
| **"Selection"** | **1 `Selection`** | 0 | 0 | **10 ✗** | **10 302 315 B** | 3 143 ms |
| "Pages from" 10 → 2 (inverted) | 2 | 10 | 10 | 1 (Qt normalised with `qMax`) | 1 034 564 B | 315 ms |
| "Pages from" 8 → 99999 (over-range `to`) | 2 | 8 | 99999 | 3 ✓ (`qMin` clamps) | 3 096 520 B | 931 ms |
| **"Pages from" 50 → 60 on a 10-page doc** | 2 | 50 | 60 | **1 blank sheet ✗** | 3 075 B | 23 ms |

Three defects fall out of that table.

**4a. [M] "Current Page" prints everything.** `printRange()==CurrentPage` and
`fromPage()==0`, so the `from < 1` branch fires and MERGEN prints all ten pages —
10.3 MB of spool for a request for one page. On a 1 000-page document this is a
1 000-page job the reader did not ask for and cannot cancel (§3).
**Severity: high.** *Fix:* switch on `printer.printRange()`;
`CurrentPage` → `from = to = m_view->currentPage() + 1`.

**4b. [M] "Selection" prints everything.** Same shape, `printRange()==Selection`.
MERGEN *has* a text selection (M5), so this is not even an unimplementable case,
but the failure mode should be "print nothing / print the selected page",
never "print everything". **Severity: high.**

**4c. [M] `from` is never bounds-checked, only `to` is.** `to` gets
`qMin(to, pageCount())`; `from` gets nothing. Asking for pages 50–60 of a
ten-page document makes the loop body unreachable, but `painter.begin(&printer)`
has already opened page 1, so **one blank sheet is emitted** with no message.
Verified: 3 075-byte one-page PDF. **Severity: medium** (wasted sheet, silent).
*Fix:* `if (from > m_doc->pageCount() || to < from) { warn and return; }` before
`painter.begin()`.

**4d. [R] The free-text "Pages" field is never read.** The dialog carries a
`pagesRadioButton` + `pagesLineEdit` pair (Qt's "1-5,8,11-13" control). It is
disabled when the output is PDF-to-file, and **enabled** when a real CUPS queue
is selected — measured by adding a temporary CUPS queue `mzdummy`
(`socket://127.0.0.1:19100`, removed afterwards):

```
  radio pagesRadioButton   checked=0 enabled=1     <- with a real queue selected
  radio pagesRadioButton   checked=0 enabled=0     <- with Print to File (PDF)
```

`printer.pageRanges()` is the only place a multi-interval range could arrive,
and `printDocument()` never calls it. Driving that control programmatically left
`fromPage()==0`, and MERGEN spooled a 10 302 464-byte all-pages job to `mzdummy`
for a "2-3" request. (Qt also left `pageRanges()` empty in that programmatic
drive, so I cannot separate "Qt did not commit the field" from "MERGEN ignored
it"; what is certain from the code is that MERGEN has no code path that could
honour a multi-interval range even if one arrived.) **Severity: medium.**

---

## 5. [M] The rotation claim is TRUE — and the 600 dpi cap really does reach poppler in a live print

`src/mainwindow.cpp:1507` takes `m_view->rotation()` and passes it straight into
`Document::renderPage`. Verified end to end by triggering the real "Rotate"
`QAction` *n* times before the real Print action and reading the image geometry
back out of the produced PDF with `pdfimages -list`
(probe `/tmp/mz/probe8`, `p10.pdf` page 1, A4 595×842 pt):

| Ctrl+R presses | on-screen rotation | embedded image in the printed PDF | placed at |
|---|---|---|---|
| 0 | 0° | **4958 × 7017** | 621 ppi |
| 1 | 90° | **7017 × 4958** | 879 ppi |
| 2 | 180° | 4958 × 7017 | 621 ppi |
| 3 | 270° | 7017 × 4958 | 879 ppi |

Assertions 1, 2, 4 and 5 of `MX.md:369` are therefore all **TRUE**:
- printer resolution is used, not the screen cache — 4958 × 7017 is exactly
  595 pt × 600/72 by 842 pt × 600/72, independent of window size or zoom;
- the cap applies — the printer reported 1200 dpi and the raster came out at 600;
- it is fitted to the paper keeping proportions (`Qt::KeepAspectRatio`);
- printing follows the on-screen rotation.

### 5a. [M] MEDIUM — rotation is honoured but the **paper orientation is not**, so a rotated print wastes half the sheet

`printer.pageLayout().orientation()` stays `Portrait` (0) in all four cases
above; `printDocument()` never calls `printer.setPageOrientation()`. A page
rotated 90° on screen produces a landscape 842 × 595 pt image that
`Qt::KeepAspectRatio` then shrinks to fit a *portrait* sheet:
fit factor `min(595/842, 842/595) = 0.7067`, so the content prints at **70.7%
linear / 50% of the area** it would occupy on landscape paper, with the bottom
half of every sheet blank. That is visible in the measured placement density:
the unrotated raster lands at 621 ppi, the rotated one at 879 ppi — i.e. 1.41×
smaller on paper.

`MX.md:371` only promises "fits it to the paper keeping its proportions", which
this technically satisfies, so this is a quality finding rather than a broken
promise. **Severity: medium.** *Fix:* set
`printer.setPageOrientation(image.width() > image.height() ? QPageLayout::Landscape : QPageLayout::Portrait)`
before `painter.begin()`, or per page via `printer.setPageLayout()`.

### 5b. [M] Peak RSS during printing is bounded — a genuine positive

The per-page `QImage` is a loop-local and is released each iteration, so
memory does **not** accumulate over a long job. `VmHWM` was **235 MB** for the
10-page run and **235 MB** for the 100-page run — identical. The bound is
"one page image at the capped DPI plus Qt/poppler overhead", which for A4 is
139 MB + ~96 MB. No leak in the print loop.

---

## 6. [M] CRITICAL — Ctrl+I (`Document::properties()`) freezes the UI for **26 seconds** on a 608 KB, **one-page** document

`src/document.cpp:293` — `const QList<Poppler::FontInfo> fonts = m_doc->fonts();`
walks the whole document's font tables. `src/document.cpp:309-322` then opens
every page and enumerates its annotations. Both run synchronously on the GUI
thread from `MainWindow::showProperties()` (`src/mainwindow.cpp:1438`), which is
wired to Ctrl+I at `src/mainwindow.cpp:426-429`. There is no cache, no worker,
no progress, no cancel.

Measured with probe `/tmp/mz/probe9 props-raw` (calls `Document::properties()`
directly, no widgets, so the number is the pure cost):

| document | size on disk | pages | `properties()` 1st call | 2nd call | peak RSS |
|---|---|---|---|---|---|
| `p10.pdf` (synthetic, 1 shared font) | 3.9 KB | 10 | 17 ms | 0 ms | 44.9 MB |
| `p100.pdf` | 35 KB | 100 | 30 ms | 2 ms | 45.0 MB |
| `p1000.pdf` | 346 KB | 1 000 | **70 ms** | 44 ms | 47.5 MB |
| `p2000.pdf` | 695 KB | 2 000 | **157 ms** | 98 ms | 50.3 MB |
| `annot2000.pdf` (2 000 pp × 5 highlights) | 2.4 MB | 2 000 | **420 ms** | 243 ms | 78.0 MB |
| **`corpus/fonts_5k.pdf`** | **608 KB** | **1** | **26 298 ms** | 429 ms | 78.7 MB |
| `corpus/fonts_40k_spread.pdf` | 8.8 MB | ? | **> 90 s** (still running) | — | — |

Page count is *not* the driver — 2 000 pages costs 157 ms. **Font count is.**
A 608 KB single-page file whose resource dictionary lists ~5 000 fonts makes
`Poppler::Document::fonts()` take **26 seconds**, and MERGEN calls it from a
keystroke with the event loop held. The window is unpaintable and unclickable
for that whole time; on Wayland the compositor marks it unresponsive.

Note also the 2nd-call figures: `properties()` is recomputed on **every** Ctrl+I.
Poppler's own internal caching absorbs most of the repeat (429 ms), but the
annotation walk is redone in full every time (`annot2000`: 243 ms on the repeat).

**Severity: critical** (unbounded, document-controlled, GUI-thread freeze
reachable from a single documented keystroke).
**Fix direction:** move `fonts()` and the annotation walk off the GUI thread the
way search already is (`SearchWorker`), present the cheap rows immediately and
fill in "Fonts" / "Annotations" when they arrive; and cache the result per
document. A cheaper stopgap: cap the annotation walk at the first *N* pages and
label the row "≥ N".

*(The corresponding "loop whose bound comes from file data" is
`src/document.cpp:309` — `for (int i = 0; i < m_doc->numPages(); ++i)` — and
`src/document.cpp:296` over `fonts()`.)*

---

## 7. [M] The headline print numbers — operation vs. time vs. peak RSS

Every row below drives the **real** `MainWindow` Print `QAction` with the real
`QPrintDialog` auto-accepted, output format "Print to File (PDF)"
(probe `/tmp/mz/probe7`). "ticks" is the number of times a 250 ms `QTimer`
installed *before* the trigger managed to fire during the print; anything other
than zero would mean the event loop got a turn. Peak RSS is `VmHWM` read from
`/proc/self/status` and cross-checked by an external 50 ms sampler on
`/proc/<pid>/VmRSS`.

| document | pages | wall time frozen | 250 ms ticks | peak RSS | output file | ms/page | MB/page |
|---|---|---|---|---|---|---|---|
| `p10.pdf` A4 | 10 | **3.87 s** | **0** | 235.4 MB | 10 302 315 B (10.3 MB) | 387 | 1.03 |
| `p100.pdf` A4 | 100 | **30.79 s** | **0** | 235.4 MB | 103 706 862 B (103.7 MB) | 308 | 1.04 |
| `p1000.pdf` A4 | 1 000 | **346.23 s** (5 min 46 s) | **0** | 237.5 MB | 1 044 262 410 B (**1.04 GB**) | 346 | 1.04 |

External sampler on the 1 000-page run: `EXTERNAL_PEAK_RSS_MB=237.7`, i.e. the
in-process `VmHWM` is honest and memory is flat across the job.

*(Run-to-run variance on this machine is roughly ±20%: the same 10-page
all-pages print measured 3 874 ms in one probe and 3 217 ms in another, §4's
table. Nothing below turns on that difference.)*

Three things this settles.

**7a. [M] CRITICAL — a 1 000-page print freezes the whole application for
5 minutes 46 seconds with no progress bar, no cancel and no repaint.**
`src/mainwindow.cpp:1514-1530`. Zero timer ticks across 346 seconds means the
event loop never ran: the window cannot redraw, `Esc` is dead, the toolbar is
dead, the close button is dead. On Wayland the compositor will offer to kill it.
The only exit is to kill the process, and killing it mid-`QPainter` leaves a
truncated spool file or a half-submitted CUPS job.
**Severity: critical.** *Fix direction:* a `QProgressDialog` with a Cancel
button, checked each iteration and driving `printer.abort()`; at minimum a
`QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents)` per page
plus a cancel flag. Rendering off-thread is the better answer but the painter
must stay on one thread, so a per-page render worker feeding the GUI-thread
painter is the shape.

**7b. [M] HIGH — the spool file is ~1.04 MB per page regardless of content.**
1.04 GB for a 1 000-page document whose source PDF is 346 KB — a **3 000×
inflation**, because every page is re-encoded as a 4958 × 7017 JPEG-in-PDF
raster (confirmed with `pdfimages -list`: `image 4958 7017 rgb 3 8 jpeg`).
`MX.md:372` names "a spool file to match" as the thing the 600 dpi cap exists to
prevent; the cap reduces it 4× but 1 MB/page of raster for a text page is still
the same failure in kind. **Severity: high** (a 1 GB job to a real CUPS queue
will hit spool quotas and take minutes to transfer).
*Fix direction:* this is the deeper design point — the whole rasterise-and-blit
approach throws away the fact that the source is already a PDF. Where the
output format is `PdfFormat`, the pages could be copied through; for a real
printer, poppler's own PS/PDF output device would keep text as text.

**7c. [M] Peak RSS is genuinely bounded and does not grow with page count.**
235.4 MB at 10 pages, 235.4 MB at 100, 237.5 MB at 1 000. The per-page `QImage`
is loop-local and freed each iteration. **No leak in the print loop** — the one
part of the resource story that is unambiguously right.

---

## 8. [M] `Document::properties()` — the full curve, and it is far worse than §6 showed

Continuing §6 with the long runs finished:

| document | size | pages | `properties()` 1st | `properties()` 2nd | peak RSS |
|---|---|---|---|---|---|
| `p2000.pdf` | 695 KB | 2 000 | 157 ms | 98 ms | 50.3 MB |
| `annot2000.pdf` | 2.4 MB | 2 000 | 420 ms | 243 ms | 78.0 MB |
| `corpus/fonts_5k.pdf` | 608 KB | **1** | **26.30 s** | 0.43 s | 78.7 MB |
| **`corpus/fonts_40k_spread.pdf`** | 8.8 MB | 2 000 | **260.20 s (4 min 20 s)** | **90.83 s** | 124.6 MB |

An 8.8 MB file freezes MERGEN for **4 minutes 20 seconds** on one Ctrl+I, and
because nothing is cached, **every subsequent Ctrl+I costs another 90 seconds**.
Peak RSS stays low (124.6 MB), so this is purely a CPU/latency denial of
service, not a memory one — which is why an OOM-based defence would not catch it.

Confirms and extends §6. **Severity: critical.**

---

## 9. [M] Outline recursion — corroborating X1's F-03 with an exact frame cost

`src/document.cpp:122-147`, `flattenOutline()` recurses once per outline level
with no depth bound; the depth comes entirely from the file. Reached from
Ctrl+T → `MainWindow::showOutline()` (`src/mainwindow.cpp:1334`) — *not* at open,
so it needs the keystroke.

**This is already reported by agent X1 as F-03 (X1-fuzz.md:175) and I am not
claiming it.** What I add is the exact per-level stack cost, which X1 estimated:

Measured by binary-searching `ulimit -s` against `corpus/outline_deep10k.pdf`
(depth 9 999):

| `ulimit -s` | result |
|---|---|
| 512 KB | SIGSEGV (rc=139, core dumped) |
| 1024 KB | SIGSEGV (rc=139) |
| 2048 KB | SIGSEGV (rc=139) |
| 4096 KB | survives — `Document::outline()` 12 263 ms, 10 000 entries, maxdepth 9 999 |

and read out of `gdb` directly as the difference between two adjacent
`flattenOutline` canonical frame addresses:

```
Stack level 13, frame at 0x7fffffdffc90
Stack level 14, frame at 0x7fffffdffd70     ->  0xE0 = 224 bytes per level
```

`gdb` also attributes the fault unambiguously to MERGEN, not poppler — frames
#12 through the stack bottom are all
`mergen::(anonymous namespace)::flattenOutline(...)`, with poppler's
`OutlineItem::hasKids()` → `Parser::getObj` chain merely being what happened to
be executing when the guard page was hit.

**224 bytes/level ⇒ the default 8 MB stack overflows at ≈ 37 400 levels**
(8 388 608 / 224), which brackets X1's estimate of 35 000–40 000 exactly.
Confirmed at the top end: `corpus/outline_deep200k.pdf` (200 000 levels) run at
the **default** stack died without reaching the print statement, peak RSS
79.0 MB.

Also measured, and worth separating from the crash: at depth 10 000 — well
inside the survivable range — `Document::outline()` takes **12.3–12.5 s** with
the event loop held. The freeze arrives long before the crash does.

---

## 10. [M] HIGH — print-to-file will overwrite the document you are reading, and nothing in MERGEN stops it

`printDocument()` never touches `printer.outputFileName()`. On a machine with no
CUPS queue configured — which is the state of this one, and of any fresh Arch
install without `cups` set up — **"Print to File (PDF)" is the only entry in the
dialog's printer combo**, measured:

```
printer combo items: 0='Print to File (PDF)'          <- no printers installed
printer combo items: 0='mzdummy' 1='' 2='Print to File (PDF)'   <- with a queue
```

So print-to-file is not an exotic corner of the print path here; it is *the*
print path.

Measured with probe `/tmp/mz/probe11`, which clicks the dialog's real
`&Print` button so Qt's own `checkFields()` validation runs exactly as it does
for a human, and reports every guard dialog Qt raises:

| destination | Qt guard raised | answered | open document after | dest after |
|---|---|---|---|---|
| a directory (`/var/tmp/mz/adir`) | `"… is a directory. Please choose a different file name."` (OK only) | OK | unchanged | untouched, dialog stays up ✓ |
| an existing unrelated file | `"… already exists. Do you want to overwrite it?"` (Yes/No) | Yes | unchanged | overwritten with PDF ✓ expected |
| **the currently-open document** | `"/var/tmp/mz/v4.pdf already exists. Do you want to overwrite it?"` (Yes/No) | **Yes** | **3 874 B → 956 628 B** | — |

Reopening the file afterwards:

```
AFTER PRINT: reopening the document -> status=0 pages=1     (it had 10 pages)
```

**The 10-page source PDF is gone**, replaced by a 1-page raster of whatever page
range was printed. Qt's prompt is generic — it names the path and asks
"already exists, overwrite it?" — and gives no hint that the path is the
document open in the window behind it. MERGEN, which *does* know, says nothing.

MERGEN also keeps displaying the old content, because poppler is holding its own
open handle; the `QFileSystemWatcher` armed at `src/mainwindow.cpp:723` will fire
and offer "This file changed on disk. Click to reload." — and clicking it loads
the destroyed file. [R]

**This is exactly the invariant the constitution states for the other write
path** (`MZ.md` §8, the redaction paragraph):

> Redaction writes a new PDF to a path the reader chooses. It never modifies the
> open document, and **the open document is never the destination.**

Redaction enforces it; printing does not. Two write paths, one rule, one of them
applying it. **Severity: high** (irreversible loss of the reader's own file;
reachable as Ctrl+P → choose a filename → Enter → Yes).
*Fix direction:* one comparison before `painter.begin()` —
`if (QFileInfo(printer.outputFileName()).canonicalFilePath() == QFileInfo(m_doc->path()).canonicalFilePath())` →
refuse with the same sentence redaction already uses. The `redact.cpp` guard is
the model to copy.

> **Not a finding — recorded so it is not re-reported.** An earlier run of mine
> showed an unrelated existing file being overwritten after answering "No".
> That was an artefact of my harness calling `QMessageBox::reject()`, which does
> not equal `QMessageBox::No`, so Qt's `if (ret == QMessageBox::No) return false`
> did not fire. Qt's overwrite guard is real and works for a human. The finding
> above is only about *which* file the reader is allowed to aim at, not about
> whether they are asked.

### 10a. [M] LOW — a print that produces nothing says nothing

`src/mainwindow.cpp:1509-1512`:

```cpp
QPainter painter;
if (!painter.begin(&printer)) {
    return;
}
```

A silent `return`. Combined with §4c (out-of-range `from` → zero iterations) and
§2 (1×1 image → blank sheet), the print path has three separate ways to produce
nothing or garbage without a word to the reader. `MainWindow::showError()` and
`PageView::setNotice()` both exist and are used elsewhere. **Severity: low.**
*Fix:* `showError(tr("Could not start the print job."))`.

---

## 11. [M] CRITICAL — `kMinZoom = 0.10` defeats "fit width", so opening an 800-byte PDF allocates 2.6 GB

`src/pageview.h:123` — `static constexpr double kMinZoom = 0.10;`
`src/pageview.cpp:1041` — `return qBound(kMinZoom, factor, kMaxZoom);`

`zoomForFitMode()` computes the factor that would actually fit the widest page
into the viewport and then clamps it to a **floor of 0.10**. For an ordinary
page the floor never binds. For a very large MediaBox it binds hard: a
200 000 pt page in a 1200 px viewport wants `1176/200000 = 0.00588`, and gets
**0.10 — 17× larger linearly, 289× larger in area than the fit it asked for.**
Nothing else in the render path bounds the pixel count (§1), so the floor sets a
minimum allocation proportional to the page area.

Measured with probe `/tmp/mz/probe12` (real `MainWindow`, 1200×900 window,
default FitWidth, RSS from `/proc/self/status`), *on document open, before the
reader touches anything*:

| document | file size | MediaBox | fit factor wanted | zoom used | render | RSS after open | peak RSS |
|---|---|---|---|---|---|---|---|
| `p1000.pdf` | 346 KB | 595 × 842 pt | 1.976 | 1.976 (floor not hit) | 1176 × 1664 | 70.8 MB | 72.2 MB |
| `huge1.pdf` (1 page) | **795 B** | 200 000 × 200 000 pt | 0.00588 | **0.10** | 20 000 × 20 000 | **1 588.8 MB** | **1 969.9 MB** |
| `huge3.pdf` (3 pages) | 795 B | 200 000 × 200 000 pt | 0.00588 | **0.10** | 20 000 × 20 000 | 1 588.8 MB | 1 969.9 MB |
| **`huge230k.pdf`** | **793 B** | 230 000 × 230 000 pt | 0.00511 | **0.10** | 23 000 × 23 000 | **2 081.0 MB** | **2 585.2 MB** |
| `corpus/many_huge_pages.pdf` | 3.3 MB | 200 000 × 200 000 pt ×20 000 pp | 0.00588 | **0.10** | 20 000 × 20 000 | 1 639.2 MB | 2 022.3 MB |

**A 793-byte file makes MERGEN allocate 2.08 GB resident / 2.59 GB peak just by
being opened**, on a 7 GB machine. 230 000 pt is the worst case *that succeeds*:
23 000² × 4 = 2 116 000 000 B, just under poppler's 2^31 ceiling from §2. Above
that the allocation is refused and the page draws as a 1×1 stretched to fill the
page rect — the §2 bug again, now on screen instead of on paper.

Two things are right here and should not be broken while fixing it:
- `PageView::paintEvent` (`src/pageview.cpp:443-447`) only calls `cachedPage(i)`
  for pages whose rect intersects the damage region, so off-screen margin pages
  are never pre-rendered. Measured: 3 huge pages cost the same as 1.
- `dropPagesOutside()` (`src/pageview.cpp:414-421`) really does bound the cache
  to the render window. Measured: A4 zoomed to the 1000% maximum in 81 steps
  ends at **302.4 MB**, growing smoothly and monotonically with zoom, never with
  page count:

| zoom | RSS | | zoom | RSS |
|---|---|---|---|---|
| 1.976 (fit) | 70.8 MB | | 8.0 | 216.4 MB |
| 7.8 | 208.9 MB | | 9.0 | 257.0 MB |
| 7.9 | 212.6 MB | | 10.0 (max, button disables) | **302.4 MB** |

So the page-count bound is sound and the zoom ceiling is sound; **the missing
bound is on the page's own area**, and `kMinZoom` is what stops the fit modes
from compensating for it.

**Severity: critical** (2.6 GB from an 800-byte file, no user action beyond
opening, and on a smaller machine this is an OOM kill rather than a slowdown).
*Fix direction:* make the floor conditional — `qBound(qMin(kMinZoom, factor), factor, kMaxZoom)`
so a fit mode is never forced *above* the factor that fits — and independently
cap the rendered pixel count (e.g. refuse or downscale past ~256 Mpx), which
also fixes §2 and §7 at the same place.

---

## 12. [M] HIGH — `computeDiff()` renders every page of both documents up front, on the GUI thread

`src/mainwindow.cpp:1088-1140`, called unconditionally from `enterCompare()`
(`src/mainwindow.cpp:1073`). No worker, no progress, no cancel.

| pair | pages compared | `enterCompare()+computeDiff()` | peak RSS |
|---|---|---|---|
| `cmpA100` / `cmpB100` (A4) | 100 | **530 ms** | 71.9 MB |
| `cmpA` / `cmpB` (A4) | 1 000 | **7 375 ms** | 74.9 MB |
| `corpus/many_huge_pages.pdf` × 2 | 20 000 | **88 806 ms (1 min 29 s)** | **2 022.3 MB** |

`leaveCompare()` is 7 ms and 64 ms respectively, so the cost is all in the
up-front render. 7.4 s of dead UI for an ordinary 1 000-page comparison;
**89 seconds** for the 3.3 MB corpus file.

**12a. [M] HIGH — and on those 20 000 pages the diff is silently wrong.**
Every one of the 40 000 renders (2 per page) hit poppler's ceiling and came back
as a 1×1 image — `Bogus memory allocation size` appeared on stderr exactly
**40 000 times**. `left.isNull()` is false for a 1×1, so `computeDiff` proceeds
to compare a 1×1 against a 1×1, finds no difference, and **reports the two
documents as identical**. This is §2's blind spot in a third place
(`src/mainwindow.cpp:1104`). Note that at `kDiffScale = 0.35` a page must be
enormous to reach the ceiling — 0.35 × 200 000 = 70 000 px — but the corpus file
is exactly that, and "compare says no changes" is the most dangerous possible
wrong answer from a compare feature.

**Severity: high.** *Fix direction:* same as §3/§7a for the freeze (worker +
progress + cancel), and same as §2 for the correctness (validate the returned
image geometry against the requested one).

---

## 13. [M] MEDIUM — `Document::contentHash()` reads and hashes the whole file on the GUI thread

`src/document.cpp:195-215`, called from the portal path at
`src/mainwindow.cpp:874` and `:901`.

| file | page cache | `contentHash()` 1st | 2nd (memoised) | peak RSS |
|---|---|---|---|---|
| `p1000.pdf` (346 KB) | warm | 1 ms | 0 ms | 41.7 MB |
| `big500.pdf` (500 MB) | **warm** | **606 ms** | 0 ms | 41.6 MB |
| `big500.pdf` (500 MB) | **cold** (`POSIX_FADV_DONTNEED`) | **1 913 ms** | 0 ms | 41.5 MB |

Two things are right: `QCryptographicHash::addData(&file)` **streams** — RSS is
flat at 41.5 MB while hashing 500 MB, so there is no read-it-all-into-memory bug
— and the result is memoised in `m_hash`, so the second portal operation is free.

What is left is a **1.9-second GUI freeze on the first portal operation** of a
large document, on an SSD; on a spinning disk or a network mount it is
proportionally worse and unbounded in principle, since the bound is the file
size. **Severity: medium** (bounded by I/O speed, once per document, no memory
risk). *Fix:* hash on the same worker thread pattern search already uses, or
hash lazily with a "working…" notice.

---

## 14. [M] The search path holds up — no finding, and worth saying so

`src/document.cpp:429-458` (`SearchWorker::run`) + `src/mainwindow.cpp:1641-1700`.
Tested on `hits.pdf` — 2 000 pages × 61 matches = **122 000 hits**, every one
crossing a thread boundary as a queued `hitFound(int, QRectF)` signal.

| document | hits | time to last hit | worst event-loop gap | RSS before → after | repaint with hits loaded |
|---|---|---|---|---|---|
| `p1000.pdf` | 1 000 | ~1 s | **2 ms** | 70.8 → 74.5 MB | 2.71 ms/frame |
| `hits.pdf` | **122 000** | **2 027 ms** | **5 ms** | 73.6 → 86.2 MB | 3.37 ms/frame |

122 000 queued cross-thread signals cost **12.6 MB** (~105 B/hit) and the GUI
thread never lost the event loop for more than **5 ms**. The progress bar and
the Cancel button both exist and are wired (`src/mainwindow.cpp:1668-1670`),
and `cancelSearch()` correctly disconnects before cancelling so a replaced pass
cannot leak hits into the new list (`src/mainwindow.cpp:1674-1686`).

**This is the model the print path (§3, §7a) and `properties()` (§6) should have
been built to.** It is the same application, so the pattern was available.

**14a. [R] NIT — `paintSearchHits` is linear in the document's total hits.**
`src/pageview.cpp:788` — `for (int i = 0; i < m_hits.size(); ++i)` runs the full
122 000-entry list for *each* page painted, filtering by page number inside the
loop. Measured cost is only +0.66 ms/frame at 122 000 hits, so it is a nit today,
but it is O(total hits × visible pages) per repaint and would bite an order of
magnitude up. *Fix:* key `m_hits` by page (`QHash<int, QList<QRectF>>`).

---

## 15. [M] Correction — `linkAt` does **not** run on every mouse move

The audit brief lists "`PageView::linkAt` runs on every mouse move and iterates
all links on the page linearly" as a candidate. **The first half is not true of
this code.** `PageView::mouseMoveEvent` (`src/pageview.cpp:~515`) never calls
`linkAt`; the only call site is `PageView::mousePressEvent`
(`src/pageview.cpp:525`), i.e. once per left-click.

Measured with synthesised `QMouseEvent`s sent to the real viewport
(probe `/tmp/mz/probe14`):

| links on the page | first click (builds `Document::pageLinks`) | steady-state click (`linkAt` scan) | **mouse move, no button** |
|---|---|---|---|
| 400 | 0 ms | 0.0015 ms | 0.0020 ms |
| 5 000 | 5 ms | 0.0126 ms | 0.0020 ms |
| 20 000 | 6 ms | 0.0014 ms | 0.0020 ms |

Move cost is flat at 0.0020 ms regardless of link count — confirming `linkAt` is
not on that path. The linear scan itself is real but costs 12.6 µs at 5 000
links. **No finding.**

---

## 16. [M] MEDIUM — `m_words` and `m_links` are never trimmed, unlike the page cache

`src/pageview.h:256,260`. `m_cache` is bounded every paint by
`dropPagesOutside()` (`src/pageview.cpp:414`) and the comment at
`src/pageview.cpp:438` states the intent: *"Bound the cache to the render window
so memory tracks the viewport, not the page count."* The two sibling caches get
no such treatment — `m_words` and `m_links` are cleared **only** on
`setDocument()` (`src/pageview.cpp:116-117`) and on a rotation
(`src/pageview.cpp:1108-1109`). Every page whose text is touched by a selection
keeps its full word list for the life of the document.

Measured by drag-selecting down `words300.pdf` (300 pages, ~1 500 text boxes per
page), one press-drag-release plus one viewport scroll per step:

| steps | RSS | delta |
|---|---|---|
| 0 | 83.9 MB | +9.2 MB |
| 50 | 96.6 MB | +21.9 MB |
| 150 | 98.4 MB | +23.7 MB |
| 250 | 104.2 MB | +29.5 MB |
| 300 (≈53% of the scroll range) | **106.0 MB** | **+31.3 MB** |

≈ 200 KB per page touched, monotonic, never released. Extrapolating the same
document shape: 2 000 pages ≈ 400 MB, 20 000 pages ≈ 4 GB, all of it retained
until the document is closed or rotated.

**Severity: medium** (needs the reader to drag-select across many pages, but the
growth is unbounded in page count and silently permanent).
*Fix:* call `dropPagesOutside()` on `m_words` and `m_links` too, or give them the
same render-window bound the image cache has.

---

## 17. [R] NIT — the document's own print restriction is reported and then ignored

`src/document.cpp:275-277` puts `print` into the "Restrictions" row when
`m_doc->okToPrint()` is false, and the properties overlay shows it.
`printDocument()` (`src/mainwindow.cpp:1479`) never calls `okToPrint()`, so the
document prints anyway.

Ignoring PDF permission bits is a defensible position for a free viewer — they
are advisory and trivially stripped. The inconsistency is the finding: MERGEN
tells the reader in one panel that printing is denied and then prints. Either
honour it or do not claim it. **Severity: nit.** *Fix:* drop `print` from the
restrictions row, or note in `MX.md`/`MZ.md` that restrictions are reported for
information and never enforced.

---

## 18. [R] Interaction note — `Control::onConnection` × the frozen print loop

Not my finding (another agent owns the control socket); recorded only because it
compounds §7a. `Control::onConnection` (`src/control.cpp`) runs on the GUI thread
and does `client->waitForReadyRead(kReplyMs)` per connection. During a print, the
GUI thread is inside `printDocument()`'s `for` loop and **never returns to the
event loop at all** — measured: 0 timer ticks across 346 seconds (§7a). So while
a large print is running:

- `QLocalServer::newConnection` never fires, so `onConnection` is not reached —
  but the kernel still completes the connection and buffers the client's line;
- every `mergen` launched from a shell during that window finds the socket taken
  (`listenForCommands()` returns false, `src/main.cpp:30`) and sends
  `open <path>` via `Control::send`, whose `waitForReadyRead(kReplyMs)` — 1000 ms,
  `src/control.cpp:65` — then times out and returns an empty string;
- `main.cpp:34` returns 0 regardless, so the reader's second `mergen file.pdf`
  **exits silently after about a second having apparently done nothing**;
- when the print finally ends and the event loop resumes, `onConnection` accepts
  the long-since-closed connection, finds the line still buffered, and opens the
  document — **minutes later and unprompted**, over whatever the reader is
  looking at by then.

That last effect is the one worth flagging to whoever owns the socket: the freeze
turns a synchronous hand-off into a delayed one, and the delay is unbounded in
document size.

---

## Appendix — method, and how to reproduce

All probes are in `/tmp/mz/`, built against the project's own object files so the
code under test is the shipped code, not a re-implementation:

```
OBJS=$(find <repo>/build/CMakeFiles/mergen.dir -name '*.o' ! -name 'main.cpp.o')
g++ -std=c++20 -O2 -fPIC -w -o probeN probeN.cpp $OBJS \
  -I<repo>/src -I<repo>/build \
  -I<repo>/build/mergen_autogen/include \
  -DMERGEN_VERSION='"1.0.0"' -DMERGEN_HELPER_PATH='"/x"' \
  -DMERGEN_RELEASE_DATE='"2026-08-24"' \
  $(pkg-config --cflags --libs Qt6Widgets Qt6PrintSupport Qt6Network poppler-qt6 libqpdf)
```

Run with `QT_QPA_PLATFORM=offscreen` and `XDG_STATE_HOME`/`XDG_CONFIG_HOME`
pointed at `/tmp/mz/xdg` so the reader's real recent-files and portals state is
never touched.

| probe | what it drives |
|---|---|
| `probe1b` | `QPrinter` defaults and the cap arithmetic |
| `probe2` | `mergen::Document::renderPage` at the print scale, by MediaBox |
| `probe7` | **the real Print `QAction`**, real `QPrintDialog` auto-accepted, each range mode |
| `probe8` | the real Rotate `QAction` *n* times, then the real Print action |
| `probe9` | `properties()`, `outline()`, `contentHash()`, `enterCompare()` |
| `probe11` | print-to-file destinations, clicking the dialog's real `&Print` button so Qt's `checkFields()` validation runs |
| `probe12` | `PageView` zoom / fit / memory |
| `probe13` | the search worker under a hit flood |
| `probe14` | link lookup and the `m_words`/`m_links` caches, via synthesised `QMouseEvent`s |

**`printDocument()` is private.** It was driven **through the public Print
`QAction`** (`findChildren<QAction*>()`, matching `text() == "Print"`), with a
`QTimer::singleShot` that manipulates the real `QPrintDialog`'s own widgets
(`printers` combo, `filename` edit, `printAll`/`printRange`/`pagesRadioButton`/
`printCurrentPage`/`printSelection` radios, `from`/`to` spin boxes) and then
accepts it. **No part of the print path was re-implemented.** Where a result
depended on how the dialog was driven rather than on MERGEN's code, it is
labelled as such (§4d) or explicitly withdrawn (the note at the end of §10).

Timing is `QElapsedTimer`; UI liveness is a `QTimer` heartbeat installed before
the trigger; memory is `VmRSS`/`VmHWM` from `/proc/self/status`, cross-checked by
an external 50 ms sampler on `/proc/<pid>/status` (`/tmp/mz/sample.sh`).
`/usr/bin/time` is not installed on this machine.

Test documents: `/tmp/mz/gen.py` builds raw PDFs in the style of the project's
own fixtures (`p10`…`p2000`, `huge1`/`huge3`/`huge230k`, `links400`…`links20000`,
`words300`, `hits`, `annot2000`), plus `/var/tmp/mz/big500.pdf` (500 MB, one
padding stream). The font- and outline-stress files
(`fonts_5k`, `fonts_40k_spread`, `outline_deep10k`, `outline_deep200k`,
`many_huge_pages`) are agent X1's corpus in `docs/audit/corpus/`, reused rather
than regenerated.

**System changes made and reverted.** A temporary CUPS queue `mzdummy`
(`socket://127.0.0.1:19100`, raw) was added with `lpadmin` to establish whether
the dialog's free-text "Pages" field is enabled when a real printer exists (§4d).
Its jobs were cancelled and the queue removed (`lpadmin -x mzdummy`);
`lpstat -a` reports "No destinations added" again. No repository file was
modified — `git diff --stat` is empty and no PDF in the tree is tracked.
