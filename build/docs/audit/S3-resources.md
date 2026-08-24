# S3 — Memory and resource-growth audit

MERGEN v2.0 (branch `ata`), audited at commit `472183b` ("M7: PKGBUILD and
release workflow" — note: working tree is on `ata`, HEAD commit message is
from the old M-series; milestones Z1–Z10 described in MZ.md appear to already
be implemented in the tree even though the visible git log only shows M1–M7
messages. Not investigated further; irrelevant to this audit).

Method: read pageview.{h,cpp}, mainwindow.{h,cpp}, document.{h,cpp},
iconset.{h,cpp}, main.cpp in full. Generated a synthetic 1000-page PDF
(`/tmp/big_a.pdf`, ~8.3 MB, 100,000 occurrences of "lorem" / 1,000 of
"needle" / one internal link per page, produced by `/tmp/genpdf.py`) and a
near-identical companion (`/tmp/big_b.pdf`) for compare-mode testing. Built
four small harnesses linked against the real built `.o` files
(document.cpp.o, pageview.cpp.o, iconset.cpp.o, etc.), driving the real
public `Document`/`PageView`/`IconSet` APIs under `QT_QPA_PLATFORM=offscreen`:
- `/tmp/probe.cpp` — the main, multi-stage long-session harness (`VmRSS`
  sampled from `/proc/self/status`).
- `/tmp/probe2.cpp` — two fast, targeted live confirmations (stale
  `m_diffBands`; a plain click populating `m_words`/`m_links`).
- `/tmp/probe3.cpp` — `malloc_trim(0)`-instrumented re-run of the scroll/
  zoom/words-links sequence, to separate genuine growth from glibc
  allocator retention.
- `/tmp/probe4.cpp` — a scoped-down (150-page) re-run of the scroll/zoom/
  rotate/night-mode sequence, run under `valgrind --tool=massif` for an
  allocator-retention-immune, fully-symbolized heap profile (raw output
  preserved at `docs/audit/massif.out.probe4`).

The real `mergen` binary itself was also run end-to-end (offscreen, driven
via its own control socket) for the `IconSet` destruction-order check.

This file was appended to as each measurement was taken; it is authoritative
over the final chat summary.

Status: **COMPLETE.** All items in the brief (cache bounding, per-axis
invalidation, compare-mode cost, night mode, `IconSet`, a long-session RSS
table, search-hit capacity) have been measured. See "Findings, most severe
first" for the ranked list, and "Consolidated long-session RSS table" for
the single table spanning a full synthetic session.

---

## Findings from reading the code (to be confirmed/quantified by measurement)

### 1. `dropPagesOutside()` bounds only `m_cache` — CONFIRMED BY READING, measurement pending

`src/pageview.cpp:414-422`:
```cpp
void PageView::dropPagesOutside(int first, int last) {
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key() < first || it.key() > last) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}
```
Only iterates `m_cache`. Called once per paint, from `paintEvent()` at
`src/pageview.cpp:440`. `m_words`, `m_links`, `m_diffBands`, `m_hits` are
never touched by it or by anything else keyed to the viewport/render window.

Refinement after reading the call graph (contradicts the audit brief's framing
that *scrolling* accumulates `m_words`/`m_links`): **passive scrolling alone
does not populate `m_words` or `m_links` at all.** `paintEvent()`'s per-page
loop (`src/pageview.cpp:443-456`) calls only `cachedPage()`,
`paintSelection()`, `paintSearchHits()`, `paintDiffBands()`. `paintSelection()`
(`src/pageview.cpp:690-714`) calls `wordsOf(page)` but returns immediately at
line 691 if there is no active selection at all — it does not run per-page
just because a page is visible. `paintSearchHits`/`paintDiffBands` never touch
`m_words`/`m_links`.

The actual population paths are:
- `wordsOf(page)` (`pageview.cpp:571-577`, private, memoizes into `m_words`) —
  called from `positionAt()` (`pageview.cpp:603`, itself called from
  `mousePressEvent`/`mouseMoveEvent` during a text-selection drag) and from
  `selectedText()` (`pageview.cpp:661`, iterates every page between the
  selection anchor and cursor, so a selection spanning many pages memoizes
  every page in between even if never independently displayed).
- `linksOf(page)` (`pageview.cpp:486-496`, private, memoizes into `m_links`) —
  called from `linkAt()` (`pageview.cpp:498-516`), itself called
  unconditionally at the top of every `mousePressEvent` with the left button
  (`pageview.cpp:524-531`), for whichever single page is under the click —
  regardless of whether a link is actually there (`linksOf` inserts an empty
  `QVector<PageLink>` too).

So the real accumulation trigger is: **any left click, or any text selection,
on a page** — not merely scrolling past it. Over a long reading session where
the reader selects/copies text or clicks around (extremely plausible usage),
`m_words` and `m_links` grow by one entry per distinct page touched this way,
forever, with **zero pruning** — `dropPagesOutside` never runs on them, and
nothing else does either except full clear on `setDocument()` or rotation
(see next finding). Severity and exact bytes/page: pending measurement
(stage 2 of the harness, direct-fill using the same `QHash<int,QVector<Word>>`
/ `QHash<int,QVector<PageLink>>` types and the same `Document::words()` /
`Document::pageLinks()` calls `wordsOf`/`linksOf` make).

### 2. Cache invalidation per axis — read-only pass, table

| Trigger | `m_cache` | `m_words` | `m_links` | `m_diffBands` | `m_hits` | Correct? |
|---|---|---|---|---|---|---|
| `setZoom`/`zoomIn`/`zoomOut`/fit mode (`applyScale`, `pageview.cpp:1091-1117`) | cleared (line 1104) | **kept** | **kept** | not touched | not touched | Matches MZ.md's documented intent for `m_words` ("survive a zoom... dropped on rotation"); `m_links` explicitly follows the same rule in code (`pageview.cpp:1108-1109`, both cleared together only `if (turned)`) — confirms the brief's "verify m_links follows the same rule" is TRUE. |
| Rotation (`applyScale`, `turned` branch) | cleared | cleared (line 1108) | cleared (line 1109) | not touched | not touched | Correct per above. |
| Night-mode toggle (`setNightMode`, `pageview.cpp:403-412`) | cleared (line 409) | not touched | not touched | not touched | not touched | Correct — only the rendered pixels depend on night mode. |
| New document (`setDocument`, `pageview.cpp:112-144`) | cleared (115) | cleared (116) | cleared (117) | **NOT cleared** | cleared (118) + `m_currentHit=-1` (119) | **BUG** — see finding below. |
| Enter compare (`MainWindow::enterCompare`, `mainwindow.cpp:1038-1075`) | n/a (new second `PageView`, starts empty) | n/a | n/a | populated by `computeDiff()` for every page of both docs | not touched | — |
| Leave compare (`MainWindow::leaveCompare`, `mainwindow.cpp:1077-1086`) | second view/doc destroyed entirely | destroyed with second view | destroyed with second view | `m_view->clearDiffBands()` (1081) — only the *primary* view's bands are cleared | not touched | Correct for the primary view; second view's caches go away with the object. |

### 3. `PageView::setDocument()` never clears `m_diffBands` — BUG, file:line confirmed by reading, severity HIGH (correctness), MEDIUM (memory)

`src/pageview.h:277` / `src/pageview.cpp:112-144`. `setDocument()` clears
`m_cache`, `m_words`, `m_links`, `m_hits`/`m_currentHit`, selection — but not
`m_diffBands`. `clearDiffBands()` (`pageview.cpp:382-385`) is called from
exactly one place in the whole tree: `MainWindow::leaveCompare()`
(`mainwindow.cpp:1081`), and only on the *primary* `m_view`.

Compounding bug found by tracing every caller of `MainWindow::openPath()`
(the single documented entry point for opening a document, per its own doc
comment at `mainwindow.h:45-48`): **none of the three UI paths that open a new
primary document while compare mode is active call `leaveCompare()` first**:
- `MainWindow::chooseFile()` (Ctrl+O) — `mainwindow.cpp:599-607` → `openPath()` directly.
- `MainWindow::dropEvent()` (drag-and-drop) — `mainwindow.cpp:1191-1198` → `openPath()` directly.
- `MainWindow::runCommand("open", ...)` (control socket) — `mainwindow.cpp:740-749` → `openPath()` directly.
- `MainWindow::reloadDocument()` (file-watcher reload) — `mainwindow.cpp:609-616` → `openPath()` directly.

`enterCompare()` itself guards against re-entry (`isComparing() ||
!m_doc->isOpen()` at `mainwindow.cpp:1039`), so this is not "compare mode
doubles further" — but it does mean: **open a new primary document while
comparing (Ctrl+O, drag a file, or the control socket), and the second
`Document`+`PageView` (with all its own caches) stays resident and now
compares against a document it was never asked to compare, while the primary
view keeps rendering the OLD comparison's diff bands over the brand-new
document's same page indices** (since `setDocument()` doesn't clear
`m_diffBands`, and nothing calls `leaveCompare()`). This is reachable through
completely ordinary interaction (open a different file while a comparison is
up) and is both a visible correctness bug (wrong bands drawn) and a resource
one (second full document + view never released). Not yet reproduced live;
the code path is unambiguous from reading, severity assessed as HIGH given
how easily it is triggered.

Suggested fix direction: `PageView::setDocument()` should clear
`m_diffBands` unconditionally (symmetric with how it treats every other
per-page cache), **and** `MainWindow::openPath()` should call `leaveCompare()`
before proceeding, the same way `closeDocument()` probably should too (it
currently doesn't touch compare state either — see `mainwindow.cpp:579-592`).

### 4. `m_hits` (search) has no cap — read-only, measurement pending

`PageView::addSearchHit()` (`pageview.cpp:729-736`) unconditionally appends
to `m_hits` (`QList<QPair<int,QRectF>>`) and emits `searchHitsChanged` +
`viewport()->update()` **per hit**. `SearchWorker::run()`
(`document.cpp:433-458`) emits `hitFound` once per match via a queued
cross-thread connection, so a term with 100,000 occurrences means 100,000
individually queued metacalls landing on the GUI thread. No batching, no cap.
Byte cost and wall-clock cost: pending measurement (stage 3).

---

## Measurements

Harness: `/tmp/probe.cpp`, compiled with:
```
g++ -std=c++20 -O2 -fPIC -o probe probe.cpp \
  $(find <repo>/build/CMakeFiles/mergen.dir -name '*.o' ! -name 'main.cpp.o') \
  -I<repo>/src -I<repo>/build -I<repo>/build/mergen_autogen/include \
  -DMERGEN_VERSION='"1.0.0"' -DMERGEN_HELPER_PATH='"/x"' -DMERGEN_RELEASE_DATE='"2026-08-24"' \
  $(pkg-config --cflags --libs Qt6Widgets Qt6PrintSupport Qt6Network poppler-qt6 libqpdf)
```
Run as `QT_QPA_PLATFORM=offscreen ./probe`. Test document: `/tmp/big_a.pdf`,
1000 pages, 595×1300pt each, ~101 words/page (100,000 "lorem" + 1,000
"needle" total), 1 internal link per page. Companion `/tmp/big_b.pdf` for
compare mode. RSS sampled from `/proc/self/status` (`VmRSS`) inside the
harness process itself.

### Stage 1 — open, full scroll (forward + back), zoom, rotate, night mode

Viewport 900×700, fit-width zoom ≈1.47 (pixel image per cached page ≈
875×1911 ≈ 6.7 MB at `Format_RGB32`). Scroll driven by directly setting
`verticalScrollBar()->setValue()` in 600px steps (less than one page's screen
height, so every page is visited) followed by a synchronous
`viewport()->repaint()` — i.e. the real `paintEvent()` → `dropPagesOutside()`
→ `cachedPage()` path runs exactly as it would for a real scroll gesture.

| Stage | VmRSS (MB) | Note |
|---|---:|---|
| Process start (QApplication constructed) | 35.6 | |
| After `Document::openPath` (1000-page, 8.3MB PDF) | 37.9 | poppler parses lazily; cheap |
| After `setDocument()` + first paint | 65.8 | first ~3 pages rendered into `m_cache` |
| Scroll step 250/3158 | 79.7 | |
| Scroll step 500 | 79.7 | |
| Scroll step 1000 | 79.7 | |
| Scroll step 1500 | 81.0 | |
| Scroll step 2000 | 81.0 | |
| Scroll step 2500 | 81.0 | |
| Scroll step 3000 | 81.4 | |
| **After full forward scroll (all 1000 pages, 3158 steps, 40.26s)** | **72.9** | |
| After full second pass back to top | 75.5 | |
| After zoom in ×15 (fit-width ≈1.47 → ≈2.97) | 100.4 | see note below |
| After zoom out ×15 (→ back to ≈1.47) | 98.4 | did not fully return to 75.5MB — see note |
| After `setFitWidth()` | 98.4 | |
| After rotate 90° | 98.4 | |
| After rotate 180° | 98.4 | |
| After rotate back to 0° | 98.4 | |
| After night-mode toggled on/off ×30 | 98.4 | flat — no per-toggle growth |
| After deleting the `PageView` and closing the `Document` | 96.2 | |

**Headline result: RSS does NOT climb with page count.** Scrolling through
all 1000 pages forward, then all 1000 pages back, moved RSS from 65.8MB to a
peak of ~81.4MB and settled at 75.5MB — a ~10-15MB fluctuation, not a
1000-page-shaped climb. This is the central claim in MZ.md §9 / MX.md M2
("memory does not climb with page count") and **it still holds in v2.0** for
the render cache specifically. `dropPagesOutside()` (`pageview.cpp:414-422`)
does what it says.

**Note on the zoom plateau not fully returning (RESOLVED below with
`malloc_trim`):** 65.8→81.4MB across scrolling is credible as "~3 cached
pages at ~6.7MB each plus baseline." Zooming to ≈2.97× makes each cached page
≈27MB, so a couple of cached pages at that zoom is consistent with the
98-100MB reading. Zooming back down to ≈1.47× did not bring RSS back down to
the pre-zoom ~75-80MB — it stayed at 98.4MB. Investigated with a follow-up
`malloc_trim(0)`-instrumented harness (`/tmp/probe3.cpp`) — see "Follow-up:
malloc_trim" below. Short answer: **this is glibc allocator retention (freed
heap arena not yet returned to the OS), not a live Qt-side leak** — a repeat
of the same scroll→zoom-in→zoom-out sequence in the `malloc_trim` harness
showed RSS return to *below* its pre-zoom level even *without* trimming,
confirming the 98.4MB reading here was an allocator-history artifact (glibc's
dynamic mmap threshold adapting differently across the two runs' slightly
different operation counts) rather than anything reproducible or Qt-side.
Does not contradict the headline plateau finding above.

Full raw log for this stage: `/tmp/probe_out.log` (lines 1-30), preserved at
time of writing.

### Stage 2 — `m_words` + `m_links` growth (the actual unbounded-accumulation path)

Direct-fill harness stage using the exact types and exact per-page calls
`PageView::wordsOf()`/`linksOf()` use (`QHash<int,QVector<Word>>::insert`,
`QHash<int,QVector<PageLink>>::insert`, fed from the public
`Document::words()`/`Document::pageLinks()`), for all 1000 pages of
`big_a.pdf` (1,106 words/page — 6-word title line + 100 filler lines of 11
words each — and 1 link/page).

| Pages filled | VmRSS (MB) | Δ over previous checkpoint |
|---:|---:|---:|
| 0 (doc just opened) | 96.25 | — |
| 250 | 96.67 | +0.43 MB |
| 500 | 109.6 | +12.9 MB |
| 750 | 139.7 | +30.1 MB |
| 1000 | 169.9 | +30.1 MB |

- Total for 1000 pages: **+73.6 MB** (98,560 KB → 173,952 KB).
- `Document::words()`+`Document::pageLinks()` wall time for 1000 pages:
  **5,478 ms** (5.48 ms/page).
- Totals: **1,106,000 `Word` entries**, **1,000 `PageLink` entries** (1/page,
  by construction of the test PDF).
- `sizeof(Word)` = **64 bytes**, `sizeof(PageLink)` = **40 bytes**,
  `sizeof(QRectF)` = 32 bytes (measured via the harness, real compiled
  layout).
- **≈69.8 bytes per `Word`** measured end-to-end (75,392 KB / 1,106,000),
  i.e. `sizeof(Word)`=64B plus a per-word heap allocation for the `QString`
  text amortized against `QVector`/`QHash` container overhead. `PageLink`'s
  contribution is negligible at 1/page (1,000 × 40B ≈ 39KB of payload).
- **≈75.4 KB added per page** touched this way (words dominate; links are
  noise at this document's 1 link/page).
- After `words.clear(); links.clear();`: RSS **unchanged** (173,952 KB, byte
  for byte) — expected; see the `malloc_trim` section for what actually comes
  back once forced.
- The non-linear staircase (250-page step nearly free, then two ~30MB steps)
  is consistent with glibc `malloc` growing the heap via `brk()` in bursts
  rather than per-allocation; it does not change the total.

**Implication:** since `dropPagesOutside()` never touches `m_words`/`m_links`
(Finding 1 above) and nothing else prunes them short of a full document close
or a rotation, a reading session in which the reader selects/copies text (or
clicks any link) on **every page of a 1,000-page document** would retain
**≈75 MB** for the life of that document, on top of whatever the bounded
render cache is holding. For a document with denser text than this synthetic
one (1,106 words/page is already fairly dense; MZ.md nowhere claims a
page-content ceiling) the number scales with words actually on the page, not
with anything MERGEN controls.

### Stage 3 — 100,000 search hits (`m_hits`)

Fed via the real, public `PageView::addSearchHit()` (same call `onSearchHit`
makes), sourced from `Document::search()` for "lorem" across all 1000 pages
of `big_a.pdf` (100 hits/page by construction).

- Wall time to add all 100,000 hits: **5,647 ms** (56.5 μs/hit).
- `view->searchHitCount()` after: **100,000** (matches the fed count exactly
  — no cap observed).
- RSS in this run (same process as stages 1-2, right after stage 2's
  `words`/`links` `clear()`): **flat at 174,020 KB across every
  checkpoint**, before, during (25k/50k/75k/100k hits) and after
  `clearSearchHits()`. That reading was masked by stage 2's leftover
  allocator slack (see `malloc_trim` section) rather than evidence `m_hits`
  costs nothing — resolved below with a clean re-run.
- **No cap exists in the code** (confirmed by reading — `addSearchHit()`
  unconditionally appends) and none was observed in practice up to 100,000
  hits; it neither crashed nor throttled.
- Responsiveness (ingest): in the real app each hit arrives as a separate
  queued cross-thread signal (`SearchWorker::hitFound`, `document.cpp:433-
  458`) — 100,000 individually queued metacalls land on the GUI thread
  rather than one batch. Not directly timed here (the harness calls
  `addSearchHit` in-process, without the queued-signal overhead), so the
  5.6s figure above is a **lower bound** on real search-to-fully-listed
  latency, not the full cost.

**Clean re-measurement (`/tmp/probe5.cpp`), fresh process, `malloc_trim`-bracketed:**

| Checkpoint | RSS (MB) | RSS after `malloc_trim(0)` (MB) |
|---|---:|---:|
| After open + first paint | 66.1 | 65.3 |
| After all 100,000 hits added | 72.7 | 72.3 |

**≈7.0MB for 100,000 hits (72.3−65.3MB, trim-anchored), ≈73 bytes/hit** —
higher than the `QPair<int,QRectF>`-payload-only estimate (~40B) because
`QList`'s incremental-append growth over-allocates capacity as it grows and
does not shrink to fit. Each checkpoint barely moved under its own `trim`
(baseline 66.1→65.3MB, post-hits 72.7→72.3MB — a few hundred KB either
time, not the multi-MB swing seen elsewhere in this report when slack is
being reclaimed), meaning the ~7MB delta between the two checkpoints **is**
genuinely live, referenced memory, not masked allocator slack. Two orders
of magnitude smaller than the `m_words`/`m_links` finding — a real cost,
not a practical concern by itself.

**Steady-state paint cost** (the brief's "UI responsiveness" question,
separate from ingest cost): `paintSearchHits()` (`pageview.cpp:783-806`)
scans the *entire* `m_hits` list on every call, once per visible page, every
repaint — worth timing directly rather than asserting it away. Scrolled to
a page with visible hits, 200 consecutive `repaint()` calls:

| | Total (200 repaints) | Per repaint |
|---|---:|---:|
| With 100,000 hits loaded | 799 ms | 4.00 ms |
| With 0 hits (`clearSearchHits()`) | 648 ms | 3.24 ms |

**≈0.76ms added per repaint by scanning 100,000 hits** — real and directly
attributable (same viewport/zoom/scroll position, only the hit list
differs), but small next to a 16.7ms/frame (60Hz) budget and dwarfed by the
page-render cost itself (a few ms per newly-cached page, and MZ.md §9's own
Z7 measurement: a *trivial* page already costs 2.0/9.7/23.4ms to render at
100%/250%/400% zoom — see Finding 3's compare-mode timing for the same
render cost showing up at scale). Not a responsiveness problem at 100,000
hits; would only become one at a hit count high enough to make the O(hits)
per-page scan itself comparable to render cost, which measurement puts
well beyond 100,000.

### Stage 4 — Compare mode / `computeDiff()` (load-time cost)

Reproduces `MainWindow::computeDiff()` exactly (same 0.35 render scale,
6-row bands, 24-channel slack — `mainwindow.cpp:1088-1153`), driven from two
real `Document`s (`big_a.pdf`, `big_b.pdf`) and two real `PageView`s (mirrors
`enterCompare()`), storing bands via the real public
`PageView::setDiffBands()`.

Context for why this is worth measuring at all rather than assuming
"rendering is cheap": MZ.md §9's own Z7 amendment records that the project
already measured full-resolution rendering of a *trivial* page (nothing on
it but five rectangles) at **2.0ms/9.7ms/23.4ms at 100%/250%/400% zoom** —
already past a 16.7ms (60Hz) frame budget at 400% on essentially blank
content, which is the project's own stated reason zoom is not tweened
(§9, "zoom is not animated"). `computeDiff()` renders at a much cheaper
fixed 0.35 scale, but does so **2,000 times** (both documents × 1000 pages)
synchronously, which is the other side of the same cost — many cheap
renders rather than few expensive ones. Both are why a correctly-bounded
cache (Finding 6) matters as much as it does here: rendering is measurably
not free at any of the scales MERGEN actually uses.

- **`computeDiff()`-equivalent wall time for a 1000-page pair: 9,116 ms
  (9.1 seconds).** This runs synchronously on the calling thread with no
  `processEvents()` inside the loop — exactly matching the real
  `MainWindow::computeDiff()`, which has none either (confirmed by reading;
  MZ.md's own "Threading" note in §9 says rendering is synchronous on the
  main thread "if either is measurably slow on real documents, move it to a
  `QThreadPool` — after measuring, not before"). **This is that
  measurement, and the answer is yes: 9.1 seconds of a fully blocked UI with
  no progress indicator** (unlike search, which has a progress bar/cancel
  button — compare has neither, confirmed by reading `enterCompare()`/
  `computeDiff()`, no `QProgressBar` or cancellation is wired to it).
- RSS during the whole stage: **flat, 174,020 KB → 174,280 KB (+260 KB)**
  across opening both documents, both views, and computing+storing diff
  bands for all 1000 pages of both — confirms the per-page `left`/`right`
  `QImage`s inside the loop really are transient (scope-local, one page's
  worth at a time; not accumulated across the loop as raw pixels — only the
  small `QVector<QPair<double,double>>` band lists persist, one per page,
  in both views' `m_diffBands`).
- `pagesWithDiff`: 1000/1000 (by construction — `big_b.pdf`'s title line
  differs from `big_a.pdf`'s on every page).
- Teardown (`clearDiffBands()` + delete both compare objects, mirroring
  `leaveCompare()`): RSS unchanged immediately after (174,280 KB, same
  allocator-retention pattern as everywhere else in this report — see
  `malloc_trim` section for what's actually reclaimable).

**Verdict on item 3 of the brief:** memory is not the problem with compare
mode at scale — it stays flat. **Time is.** 9.1 seconds with the UI
unresponsive and no way to cancel, for a fairly modest 1000×1000-page
comparison at a deliberately cheap 0.35 render scale, is a real, measured UX
problem for large documents. Confirmed by MZ.md's own "measure, then thread
it" standard: this is the measurement.

### Stage 5 — repeated open/close (5 rounds, does it plateau?)

Each round: open `big_a.pdf`, create a `PageView`, `setDocument()`, scroll
through a subset of pages, `repaint()`, then delete the view and close the
document.

| Round | After open+scroll (MB) | After close (MB) |
|---|---:|---:|
| 0 | 170.2 | 170.2 |
| 1 | 170.2 | 170.2 |
| 2 | 170.2 | 170.2 |
| 3 | 170.2 | 170.2 |
| 4 | 170.2 | 170.2 |

**RSS is bit-for-bit identical (174,280 KB) across all 5 rounds and both
checkpoints within each round.** No per-cycle growth of any kind was
observed — strong evidence against a leak in the document/view open-close
lifecycle itself (independent of the `m_words`/`m_links`/`m_diffBands`
findings above, which are about content touched *while a document is open*,
not about the open/close cycle).

### Stage 6 — `IconSet` static cache

9,000 `IconSet::icon()` calls (500 repetitions × 9 glyphs × 2 pixel sizes)
collapsing to 18 unique `(glyph, size)` cache entries.

- RSS: 174,280 KB → 176,120 KB (**+1.84 MB** for 18 cached `QIcon`s, each
  carrying 3 `QPixmap` states — Normal/Disabled/Active).
- After `IconSet::clearCache()` (simulating a palette change) and
  repopulating the same 18 entries: RSS **unchanged** (176,120 KB both
  before and after) — no growth from repeated clear/repopulate cycles.
- Confirms the cache is bounded by construction: MERGEN only ever asks for 9
  fixed glyphs (`iconset.h:21-29`) at whatever toolbar/button pixel sizes the
  UI uses (2 in the real app: toolbar icon size and the search-cancel
  button's `QIcon::Close` size) — the key space is small and fixed, not
  something that can grow with document size, page count, or session length.
  Not a concern in practice.
- "Two windows" concern from the brief: `cache()` is a process-wide function
  static (`iconset.cpp:61-64`), so two `MainWindow`s (were MERGEN ever to
  allow that — it doesn't, §5 bans a multi-document workspace, but the
  toolbar/icon code has no window affinity) would **share** one cache, which
  is more efficient, not a leak risk.

Full raw log for stages 2-6: `/tmp/probe_out.log` (lines 31-84), preserved at
time of writing.

### Follow-up: `malloc_trim` — separating "still referenced" from "allocator slack"

Several results above show RSS plateauing instead of dropping after a
`clear()`/zoom-back-down/teardown that should, logically, free memory. To
check whether that is a real Qt-side retention or ordinary glibc `malloc`
behaviour (freed heap arena kept resident for reuse rather than `munmap`'d
back to the OS), a second harness (`/tmp/probe3.cpp`) repeated the same
operations and called glibc's `malloc_trim(0)` at each checkpoint, comparing
RSS immediately before and after the forced trim.

| Checkpoint | RSS before trim (MB) | RSS after trim (MB) | Reclaimed |
|---|---:|---:|---:|
| Process start | 36.6 | 36.8 | ~0 |
| After open + first paint | 67.6 | 66.7 | 0.8 MB |
| After full forward scroll (1000 pages) | 83.7 | 75.2 | 8.5 MB |
| After zoom in ×15 (→≈2.97×) | 101.2 | 88.0 | 13.2 MB |
| After zoom out ×15 (→≈1.47×) | 70.5 | 68.5 | 2.0 MB |
| After deleting view + closing doc | 66.0 | 57.4 | 8.5 MB |
| After filling `m_words`/`m_links` for 300 pages | 93.3 | 93.2 | ~0.1 MB (still referenced — correctly *not* reclaimable) |
| After `words.clear(); links.clear();` | 93.2 | 72.5 | **20.7 MB** |

**Two distinct, both-legitimate behaviours, now told apart:**
1. **While `m_words`/`m_links` content is still alive (referenced), `trim`
   reclaims almost nothing** (93.3→93.2MB) — this confirms the Stage 2
   finding is **real, live, referenced memory**, not an allocator artifact.
   Good: it means the earlier measurement can be trusted.
2. **Ordinary render-cache churn (scroll, zoom) and post-`clear()` state
   leaves genuinely reclaimable slack behind** (8.5MB after a full scroll,
   13.2MB after a zoom excursion, 20.7MB after clearing 300 pages of
   words/links) **that a plain `clear()`/`delete` does not return to the
   OS**, because glibc `malloc` keeps freed heap-arena space resident for
   reuse rather than `munmap`-ing it back to the kernel. MERGEN never calls
   `malloc_trim()` itself (nor is there a reason it should — this is
   completely ordinary allocator behaviour, not a MERGEN bug), so **the RSS
   a reader sees in `top`/`htop` will sit some tens of MB above the
   strictly-live working set and will not fall back to baseline just because
   a cache was cleared.** This explains the Stage 1 "zoom didn't return"
   observation without needing to invoke a Qt-side leak: it is the same
   phenomenon as every line in this table.
3. Critically, **this slack does not compound run over run** — probe.cpp's
   stage 5 (5 rounds of open/scroll/close in the *same* process, no
   trimming) showed byte-for-byte identical RSS across every round. The
   slack finds a ceiling and further operations reuse it rather than adding
   to it.

**Conclusion for the report:** "memory does not climb with page count"
holds in the strict sense that matters (bounded, plateaus, does not grow
per additional page or per additional scroll/zoom repetition). The RSS a
user actually observes will sit somewhat above the theoretical minimum due
to ordinary allocator retention (tens of MB, bounded, not cumulative) — this
is normal for any long-running C++ application using glibc `malloc` and is
not something to fix.

### Live confirmation of Findings 3 and 1 (small, fast follow-up harness `/tmp/probe2.cpp`)

Two targeted checks against the real public `PageView`/`Document` API
(not a reproduction — the actual compiled methods):

**Check 1 — does `setDocument()` really leave stale `m_diffBands`?**
```
CHECK1  hasDiffBands_after_setDiffBands              1
CHECK1  hasDiffBands_after_setDocument_to_new_doc    1
CHECK1  BUG CONFIRMED: stale diff bands from the old comparison are still
        marked on the newly-opened, unrelated document.
```
Set diff bands on a `PageView`, then called `setDocument()` with a
*different* `Document` (mirroring what `MainWindow::openPath()` does today,
uncorrected, when called while comparing) — `hasDiffBands()` is still `true`
afterward, against a document that was never part of any comparison. This
reproduces Finding 3 live, not just by reading.

**Check 2 — does one plain left click populate `m_words`/`m_links` with no visible selection made?**
```
CHECK2  selection_page_after_single_click  0
CHECK2  CONFIRMED: a single left click (no drag) already memoized that
        page's words (and, per source, its links) with no user text
        selection made.
```
A single synthetic `QMouseEvent` press+release at the same point (i.e. a
plain click, not a drag — no user-visible selection results) was sent to a
freshly-opened 1000-page document's viewport via `QApplication::sendEvent`.
`PageView::selectionBounds()` — which reads `m_words` via a raw
`constFind()` that does **not** lazily populate — successfully returned page
0, proving `wordsOf(0)` already ran as a side effect of the click itself.
Confirms the call-graph read in Finding 1: **ordinary clicking, not just
text-drag-selecting, silently grows `m_words`/`m_links` on every distinct
page clicked, for the life of the document.**

### Night mode — is the un-inverted original kept anywhere?

Read `src/pageview.cpp:291-305` (`cachedPage()`) and `41-58`
(`invertLightness()`):
```cpp
QImage page = m_doc->renderPage(index, m_zoom, m_rotation);
if (m_night && !page.isNull()) {
    page = invertLightness(page);
}
it = m_cache.insert(index, page);
```
`invertLightness(const QImage &in)` does `QImage out =
in.convertToFormat(QImage::Format_RGB32)` (a shallow/shared copy when `in` is
already that format) and then mutates `out` via `out.scanLine(y)` — a
non-const call, which forces `QImage`'s copy-on-write to detach and allocate
a fresh pixel buffer at that point. So for one page's render: the freshly
decoded buffer and the inverted buffer briefly co-exist (a genuine, but
**page-scoped and transient**, ~2× peak for that one page), and then
`page = invertLightness(page)` reassigns the local, dropping the original
decoded buffer's only reference — it is freed before `cachedPage()` returns.
**Only the final (inverted) image is ever inserted into `m_cache`.** No
persistent double-buffering, and the cache never holds both a night and a
non-night version of the same page — confirmed correct by reading, and
consistent with MZ.md §9's Z5 amendment ("the cache is cleared when the mode
changes instead ... rather than holding two copies of every visible page").

`setNightMode()` (`pageview.cpp:403-412`) clears `m_cache` wholesale on
toggle (both directions), so the next paint re-renders (and re-inverts, if
now in night mode) from scratch — no stale mixed-mode pages possible.

**Measured:** Stage 1 above toggled night mode on/off **30 times** in a row,
each followed by a real repaint (so each toggle actually did the
clear-and-re-render work, not a no-op): RSS before
(`11_after_rotate_back_to_0`) and after
(`12_after_night_toggle_x30`) are **identical: 100,732 KB**. Zero net growth
across 30 repeated toggles. Severity: none — this is a correctly-designed,
measurement-confirmed non-issue.

*(measurement phase substantially complete; see final severity-ranked
findings list and consolidated long-session RSS table below)*

---

## Consolidated long-session RSS table

One continuous process (`/tmp/probe.cpp`), all stages run back-to-back with
no restart in between — this doubles as the "open a large document, scroll
all the way through, zoom in and out repeatedly, run a search, enter/leave
compare, toggle night mode, open/close several documents" scenario the brief
asked for. Condensed from the full 85-line raw log (`/tmp/probe_out.log`,
preserved) — repetitive checkpoints (individual scroll steps, the 5 identical
open/close rounds) are collapsed with their range noted; nothing is omitted
that shows a different number.

| # | Stage | VmRSS (MB) | Δ from previous shown row |
|---:|---|---:|---:|
| 1 | Process start | 35.6 | — |
| 2 | `Document::openPath` (1000-page, 8.3MB PDF) | 37.9 | +2.3 |
| 3 | `PageView::setDocument()` + first paint | 65.8 | +27.9 |
| 4 | Mid-scroll (steps 250-3000 of 3158) | 79.7 → 81.4 | +13.9 → +15.6 |
| 5 | **End of full forward scroll — all 1000 pages visited** | 72.9 | −8.5 |
| 6 | End of full backward scroll — 2000 page-visits total | 75.5 | +2.6 |
| 7 | Zoom in ×15 (fit-width ≈1.47× → ≈2.97×) | 100.4 | +24.9 |
| 8 | Zoom out ×15 (→ ≈1.47× again) | 98.4 | −2.0 (see `malloc_trim` note: allocator retention, resolved as benign) |
| 9 | Fit-width / rotate 90° / 180° / back to 0° | 98.4 | 0 |
| 10 | Night mode toggled on/off ×30 | 98.4 | 0 |
| 11 | View deleted, document closed | 96.2 | −2.2 |
| 12 | New document opened (stage 2 begins) | 96.2 | 0 |
| 13 | **`m_words`/`m_links` populated for all 1000 pages** | 169.9 | **+73.7** |
| 14 | `words.clear(); links.clear();` | 169.9 | 0 (≈20MB of this is reclaimable via `malloc_trim` — see above; not returned by plain `clear()`) |
| 15 | 100,000 search hits added (`m_hits`) | 169.9 | 0 (see caveat: likely masked by (14)'s leftover slack) |
| 16 | Search hits cleared | 169.9 | 0 |
| 17 | Compare mode: 2nd `Document` + `PageView` opened | 170.2 | +0.3 |
| 18 | **`computeDiff()`-equivalent over the full 1000-page pair (9.1s wall)** | 170.2 | 0 |
| 19 | `leaveCompare()`-equivalent teardown | 170.2 | 0 |
| 20 | 5× (open document, scroll, close) rounds | 170.2, every round | 0 across all 5 |
| 21 | 9,000 `IconSet::icon()` lookups (18 unique keys) | 172.0 | +1.8 |
| 22 | `IconSet::clearCache()` + repopulate | 172.0 | 0 |
| 23 | End of session | 172.0 | — |

**Reading this table:** it does not climb monotonically. It rises in three
identifiable, bounded steps — first paint (+28MB, one-time Qt/poppler/font
startup + initial cache), the zoom excursion (+25MB, mostly reclaimable
allocator slack, confirmed by the separate `malloc_trim` run), and the
`m_words`/`m_links` population (+74MB, genuinely live, confirmed by
`malloc_trim` — this is Finding 1 below) — and is **completely flat across
every repeated operation**: two full scroll passes, 15+15 zoom steps, 4
rotations, 30 night-mode toggles, 100,000 search hits, a full compare-mode
diff computation, and 5 rounds of document open/close. Nothing here is a
monotonic per-repetition climb; the one real unbounded-with-page-count
growth path is Finding 1, and it is bounded by *pages touched via click or
selection*, not by pages merely scrolled past.

---

## Findings, most severe first

### 1. `m_words` / `m_links` grow without bound for the life of the open document, pruned by nothing — HIGH
**Where:** `src/pageview.h:256-260` (member declarations); populated by
`wordsOf()` (`pageview.cpp:571-577`) and `linksOf()`
(`pageview.cpp:486-496`), reachable from any left-click
(`mousePressEvent`, `pageview.cpp:518-543`, via `linkAt()` unconditionally
and `positionAt()` when the click isn't on a link) or any multi-page text
selection (`selectedText()`, `pageview.cpp:660-664`). `dropPagesOutside()`
(`pageview.cpp:414-422`), the only pruning mechanism in the class, touches
only `m_cache`.
**Measured:** filling both hashes for all 1000 pages of a 1,106-word/page
document costs **+73.6MB** (98,560KB→173,952KB), confirmed via
`malloc_trim` to be genuinely live/referenced (93.3MB→93.2MB while alive,
i.e. ~0% is reclaimable slack). 1,106,000 `Word` entries + 1,000 `PageLink`
entries; **≈69.8 bytes/`Word`** end-to-end, **≈75.4KB added per page**
touched. `sizeof(Word)=64B`, `sizeof(PageLink)=40B` (measured, real compiled
layout). Fill cost: 5.48ms/page. Trigger confirmed live with a synthetic
single left-click (no drag): `wordsOf()` ran for that page with no visible
selection ever made (`/tmp/probe2.cpp` CHECK 2).
**Mitigating factor:** resets to zero on `setDocument()` (new document, or
reopening the same one) and on rotation (`applyScale()`,
`pageview.cpp:1104-1111`) — the growth is bounded by (pages touched ×
words/page) *within one document's open session*, not cumulative across
documents opened in sequence over a long MERGEN session.
**Fix direction:** extend `dropPagesOutside()` (or a variant keyed to a
wider but still bounded window, since these are cheaper to recompute than
`m_cache`'s images) to also evict `m_words`/`m_links` entries for pages far
outside the current viewport; or accept and document the tradeoff, since
poppler's `textList()`/`links()` calls are cheap enough (5.5ms/page) to
recompute on demand rather than needing to memoize forever.

### 2. `PageView::setDocument()` never clears `m_diffBands`; nothing calls `leaveCompare()` before opening a new primary document — HIGH
**Where:** `pageview.cpp:112-144` (`setDocument()`, clears `m_cache`,
`m_words`, `m_links`, `m_hits` — not `m_diffBands`); the only place
`clearDiffBands()` is ever called is `mainwindow.cpp:1081`
(`leaveCompare()`), and only for the primary view. All four routes that open
a new primary document — `chooseFile()` (`mainwindow.cpp:599-607`),
`dropEvent()` (`mainwindow.cpp:1191-1198`), `runCommand("open", …)`
(`mainwindow.cpp:740-749`), `reloadDocument()` (`mainwindow.cpp:609-616`) —
call `openPath()` directly with no `isComparing()`/`leaveCompare()` guard.
**Measured (live):** `/tmp/probe2.cpp` CHECK 1 — set diff bands on a
`PageView`, then call `setDocument()` with an unrelated second `Document`:
`hasDiffBands()` is still `true` afterward.
**Effect:** open a different file (Ctrl+O, drag-and-drop, or the control
socket's `open` command) while comparing, and the primary view keeps
painting the *old* comparison's diff bands over the new document's same page
indices, while the second `Document`+`PageView` (full cache set) stays
resident, now orphaned from any real comparison. Reachable through entirely
ordinary interaction.
**Fix direction:** `PageView::setDocument()` should clear `m_diffBands`
unconditionally, symmetric with everything else it already clears;
`MainWindow::openPath()` (or `closeDocument()`) should call `leaveCompare()`
first when `isComparing()`.

### 3. `computeDiff()` is synchronous, unthreaded, unprogressed and uncancellable — HIGH (responsiveness, not memory)
**Where:** `mainwindow.cpp:1088-1153`, called from `enterCompare()`
(`mainwindow.cpp:1073`) with no worker thread, no `QProgressBar`, no cancel
path — unlike search, which has all three (`SearchWorker`,
`document.cpp:429-458`, plus the search bar's progress/cancel UI,
`mainwindow.cpp:1597-1618`).
**Measured:** **9,116 ms (9.1 seconds)** wall clock, main-thread-blocking,
for a 1000-page-pair comparison (2000 page renders at the fixed 0.35 diff
scale + banded pixel comparison) — reproduced faithfully from the real
algorithm, storing bands via the real `PageView::setDiffBands()`. Memory
during this stage is flat (+260KB for 1000 pages both directions) — this is
purely a latency finding.
**Context:** MZ.md §9's own "Threading" paragraph states rendering is
synchronous "if either is measurably slow on real documents, move it to a
`QThreadPool` — after measuring, not before." This is that measurement, and
1000 pages is not an exotic document size.
**Fix direction:** move `computeDiff()`'s render+compare loop to a worker
thread (the same `QThreadPool`/`SearchWorker` pattern already used for
search), with incremental band delivery and a cancel path.

### 4. `m_hits` has no cap; 100,000 hits means 100,000 individually-queued cross-thread signals — MEDIUM
**Where:** `PageView::addSearchHit()` (`pageview.cpp:729-736`, unconditional
append + per-hit `searchHitsChanged` emit + per-hit `viewport()->update()`);
`SearchWorker::run()` (`document.cpp:433-458`, emits `hitFound` once per
match over a queued cross-thread connection with no batching).
**Measured:** 100,000 hits fed successfully with no crash and no observed
cap (`view->searchHitCount()` reported exactly 100,000). In-process feed
time 5.65s (56.5μs/hit) — a **lower bound** on real latency, since it
excludes the queued cross-thread signal-delivery overhead each of the
100,000 individual hits would add in the real app. Memory, fresh-process
and `malloc_trim`-confirmed live (not allocator slack): **≈7.0MB for
100,000 hits, ≈73 bytes/hit** — real, but an order of magnitude below
Finding 1. Steady-state paint cost (`paintSearchHits()` scans all of
`m_hits` on every repaint of every visible page): **≈0.76ms added per
repaint** with 100,000 hits loaded vs. none (4.00ms vs. 3.24ms/repaint,
200-repaint average) — measurably real but small next to a 16.7ms/60Hz
frame budget and dwarfed by per-page render cost itself. Not a
responsiveness problem at this hit count.
**Fix direction:** batch hit delivery on the worker thread (flush every N
hits or every few ms) instead of one queued signal per hit; consider a soft
display cap ("100,000+ hits, showing first N") for pathological search
terms.

### 5. `IconSet`'s static cache outlives `QApplication` by C++ object-lifetime rules — LOW / NIT
**Where:** `src/main.cpp:12,25` (`QApplication app` then `MainWindow window`,
both automatic storage, destroyed in reverse order when `main()` returns —
*before* any static-storage destructor runs); `src/iconset.cpp:61-64`
(`cache()`, a function-local static `QHash<CacheKey, QIcon>`, first
constructed during `MainWindow::buildToolBar()`, i.e. after `app` already
exists). By the standard, automatic objects in `main()` finish destructing
before static-duration objects (including function-local statics) are
touched at all — so `cache()`'s `QIcon`/`QPixmap` contents are destroyed
strictly after `QApplication` is gone.
**Measured:** built and ran the real `mergen` binary end-to-end
(`QT_QPA_PLATFORM=offscreen`, which unconditionally populates the icon
cache during toolbar construction), driven through a full open→`goto`→
`quit` lifecycle via the real control socket to force a clean
`QApplication::exec()` return (not a `SIGTERM`, which would skip static
destructors and prove nothing). **Result: exit code 0, empty stderr — no
crash, no "QPixmap: Must construct a QGuiApplication before a QPixmap"
warning.** Not tested against a real Wayland platform plugin (none available
in this sandbox; MERGEN is Wayland-only per MZ.md §3) — the raster/
`offscreen` backend's `QPixmap` cleanup may simply not depend on a live
platform integration the way a real compositor-backed one might.
**Fix direction:** none needed unless a real-platform crash is ever reported;
if wanted, an explicit `IconSet::clearCache()` at the top of
`MainWindow::~MainWindow()` would make the ordering deliberate rather than
relying on (correct, but nonobvious) implicit lifetime rules.

### 6. `m_cache` (the render cache) — CONFIRMED SOUND, no fix needed
**Where:** `dropPagesOutside()` (`pageview.cpp:414-422`), called every paint
(`pageview.cpp:440`).
**Measured:** RSS during a full 1000-page forward scroll plus a full
1000-page backward scroll (2000 page-visits, 3158+3158 repaint operations)
stayed in a **65.8-83.7MB band** throughout — not the multi-gigabyte figure
a hypothetical unbounded cache would reach (3-13GB+ depending on zoom, for
1000 retained ~6.7-28MB page images). Cross-validated with `malloc_trim`:
the true live floor after a full scroll is ≈75MB, ≈8.5MB of the raw 83.7MB
reading being ordinary reclaimable allocator slack, not retained pages.
**MX.md M2's and MZ.md §9's "memory does not climb with page count" claim
still holds for the render cache in v2.0.**

### 7. Cache-invalidation rules per axis — CONFIRMED CORRECT
Zoom keeps `m_words`/`m_links`, clears `m_cache` only; rotation clears all
three (`applyScale()`, `pageview.cpp:1091-1117`) — `m_links` explicitly
follows the same rule as `m_words` (both cleared together, only inside the
`if (turned)` branch, `pageview.cpp:1108-1109`), which is exactly what the
brief asked to verify, and it checks out. Night mode clears only `m_cache`
and does so correctly — no persistent double-buffering of inverted vs.
plain pages was found (only a page-scoped, transient ~2× during a single
page's render, released before `cachedPage()` returns; confirmed both by
reading `invertLightness()`/`cachedPage()`, `pageview.cpp:41-58,291-305`,
and by measurement: RSS unchanged, byte-for-byte, across 30 consecutive
night-mode toggles with real repaints).

### 8. Repeated document open/close — CONFIRMED SOUND
5 rounds of (open 1000-page document, create view, scroll a subset, close)
in the same process produced **byte-identical RSS** at every checkpoint of
every round (174,280KB, all 10 readings). No per-cycle drift of any kind.

### Independent confirmation of Finding 6 via `valgrind --tool=massif` (real heap profiler, not RSS)

Every measurement above uses `/proc/self/status` `VmRSS`, which (as the
`malloc_trim` section showed) is confounded by glibc allocator retention —
freed memory that stays resident without being genuinely referenced. To
confirm Finding 6 (`m_cache` is properly bounded) with a methodology immune
to that confound, a scoped-down harness (`/tmp/probe4.cpp`: open the
1000-page document, scroll through the first 150 pages, zoom in/out ×5/×5,
rotate once, toggle night mode twice) was run under
`valgrind --tool=massif --time-unit=ms` (native run 5.3s, under massif
≈161s of instrumented program time, 78 snapshots taken).

*Note on where the raw data lives:* this task's brief is read-only w.r.t.
the MERGEN source tree, and every other artifact this audit produced
(`genpdf.py`, the test PDFs, all five `probeN.cpp` harnesses and their raw
`.log` outputs) was deliberately kept under `/tmp`, outside the repo, for
exactly that reason. This one raw profiler dump
(`docs/audit/massif.out.probe4`, 42KB) is the single exception: it was
copied into `docs/audit/` — a gitignored, non-source directory this audit
task is otherwise using purely for its own written report — so the
call-stack detail behind the peak-snapshot breakdown below is checkable
without re-running `valgrind`. Recorded here explicitly rather than left
for someone to notice a stray binary file next to the report.

Massif tracks **useful-heap bytes** directly from the allocator, not
resident pages — no allocator-retention ambiguity.

| Program time | useful-heap (MB) | Phase |
|---:|---:|---|
| 0 - 120ms | 0 → 0.35 | Startup: `QApplication`, font DB, `Document::openPath` |
| 120.6 - 124.7ms | 3.6 → 29.8 | First pages rendered into `m_cache`, scroll begins |
| **125ms - 160ms (the entire 150-page scroll + 5×zoom-in + 5×zoom-out + rotate + 2×night-mode-toggle phase, ~35ms of program time)** | **oscillates 16.6 - 30.3, peak 30.29 at snapshot 30** | **Never exceeds ~30MB despite 150 pages visited and 12 zoom/rotate/night-mode operations** |
| 161.2ms onward (teardown: `delete view; doc->close();`) | 30.3 → 0.35 → 0.04 | Drops to near-zero within the same millisecond bucket |

**Peak snapshot (30, 138.5ms, 30.29MB useful heap) call-stack breakdown**
(from `ms_print`, real symbolized stacks): **62.06% (19.48MB) is
`SplashBitmap` allocations reached through the exact stack**
`mergen::PageView::cachedPage(int) → mergen::Document::renderPage(...) →
Poppler::Page::renderToImage(...) → ... → SplashOutputDev::startPage(...) →
SplashBitmap::SplashBitmap(...)` — **this is `m_cache`'s contents, named by
the profiler as the single largest live consumer, and it is a small,
bounded multiple of one page's rendered size**, not 150 pages' worth (150
retained pages at this test's zoom levels would be gigabytes, not 30MB).
Most of the remainder (8.03%, 2.52MB) is `QOffscreenBackingStore::resize()`
— the `offscreen` QPA platform's own window-compositing buffer, an artifact
of the test harness's platform choice, not anything `PageView` allocates.

**This independently confirms Finding 6 with a methodology that cannot be
explained away by allocator retention: the live, useful heap genuinely
plateaus around 30MB through 150 pages of scrolling plus a dozen zoom/
rotate/night-mode operations, dominated by exactly the code path
(`PageView::cachedPage()`) the design intends to be the bounded one, and
collapses to near-zero on document close.**

### 9. `IconSet` static cache size — CONFIRMED SOUND
9,000 lookups (500 reps × 9 glyphs × 2 sizes) collapse to 18 unique
`(glyph,size)` entries, costing 1.84MB; flat across `clearCache()` +
repopulate. Bounded by construction — MERGEN uses a fixed set of 9 glyphs
(`iconset.h:21-29`) at (in the real app) two pixel sizes. Not a function of
document size, page count, or session length. "Two windows" would share one
process-wide cache (more efficient, not a leak) — moot in practice, since §5
bans a multi-document workspace and MERGEN never constructs a second
`MainWindow`.

### 10. `m_portals` (`MainWindow`) — CONFIRMED SOUND, read-only assessment (no harness — mechanism is simple and human-rate-limited)
**Where:** `src/mainwindow.h:185-194` — `struct PortalEnd { QString hash,
path; int page; }`, `struct Portal { PortalEnd a, b; }`,
`QVector<Portal> m_portals`.

**Growth path:** `markPortal()` (`mainwindow.cpp:870-895`) appends exactly
**one** `Portal` per **two explicit `Ctrl+M` presses at two different
locations** — a deliberate, human-rate-limited action per MZ.md §5's own
reasoning for why portals are exempt from the "remember last page" ban
("the reader creates it deliberately... nothing about a reader's position is
ever written without them asking for it"). This is categorically different
from the scroll/click-driven growth in Finding 1: it cannot accumulate
faster than the reader can physically make portals.

**Reload safety:** `loadPortals()` (`mainwindow.cpp:804-847`) is the only
place `m_portals` is populated from disk, is called **exactly once** — from
the `MainWindow` constructor (`mainwindow.cpp:242`) — and unconditionally
clears `m_portals` first (line 805) before re-parsing the TOML file, so
there is no double-load/re-entrant-growth risk even if it were ever called
again. `savePortals()` (849-868) rewrites the entire file atomically
(`QSaveFile`) on every new portal — matches MZ.md §8's "written atomically"
claim, and doesn't let the on-disk file grow by appending either.

**Per-entry cost (arithmetic, not measured — bounded tightly enough that a
harness would not change the conclusion):** each `Portal` is two
`PortalEnd`s, each holding a SHA-256 content hash (64 hex chars) and an
absolute path as `QString`s plus an `int`. At roughly 250-400 bytes per
`PortalEnd` (heap-allocated UTF-16 string data plus `QArrayData` overhead
for typical hash/path lengths), that's **on the order of 0.5-1KB per
portal**. A reader would need several thousand deliberately-made portals —
each requiring two separate `Ctrl+M` presses at two different locations —
to reach even 1-2MB. Not a concern.

