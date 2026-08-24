# S4 — UI correctness audit: coordinate transforms, keyboard routing, state consistency

Scope, per assignment: MERGEN (C++20/Qt6 PDF viewer). Read-only audit — no
source files modified. Four independent state axes checked in combination:
zoom (10%-1000%), rotation (0/90/180/270), night mode, compare mode. Features
in scope: text selection, search-hit highlighting, internal-link hit-testing,
hold-to-peek, compare diff bands, outline jump, Esc chain, keyboard routing,
action enable/disable, focus.

Reference: `<repo>/build/docs/MZ.md` §6 (Interface), §7
(Keybindings), §9 (Architecture) — read in full before starting.

Existing acceptance tests read for coverage (not repeated here):
`<scratch>/z{1,2,3,4,5,6,7,9,10}check.cpp`.

Method: static/algebraic reasoning first, then a compiled harness
(`g++ -std=c++20`, linking MERGEN's own .o files, `QT_QPA_PLATFORM=offscreen`)
driving real `MainWindow`/`PageView`/`Document` objects, asserting geometry
and, where relevant, pixel output via `QWidget::grab()`.

Findings are appended below AS THEY ARE CONFIRMED, most-severe-so-far first is
NOT maintained during writing (append-only log); the final report re-sorts.
Each finding is tagged REPRODUCED (built and ran a concrete failing/passing
assertion) or READ-ONLY ASSESSMENT (code-reasoning only, not exercised).

---

## Status: COMPLETE. Summary table above; full evidence log below.

---

## Summary (most severe first)

| # | Severity | Tag | Finding | File:line |
|---|---|---|---|---|
| 1 | **CRITICAL** | REPRODUCED (crash, full backtrace, 3x reproduced) | Control-socket `goto` command received while the password prompt is up crashes the process (SIGSEGV in poppler `Catalog::getNumPages()`, via `Document::isOpen()` reporting true for a locked document) — modality blocks UI input but not the socket | `mainwindow.cpp:736-797` (runCommand "goto"), `document.h:102` (isOpen), `document.cpp:115-117` (pageCount) |
| 2 | HIGH | REPRODUCED (pixel-exact, 4/4 rotations) | Compare diff bands painted in the wrong place/shape whenever the page is rotated 90/180/270 — bands computed against an unrotated render but painted with no rotation transform | `pageview.cpp:387-401` (paintDiffBands), `mainwindow.cpp:1088-1153` (computeDiff) |
| 3 | HIGH | REPRODUCED (2 variants) | Opening any new document (Ctrl+O, drag-drop, recent menu, control socket) while comparing leaves compare mode dangling: two unrelated documents on screen, still reported as "comparing," stale diff bands | `mainwindow.cpp:516-573` (openPath), `579-592` (closeDocument), `1038-1086` (enterCompare/leaveCompare) |
| 3a | (addendum to #2/#3) | REPRODUCED | The two compare panes hold fully independent rotation state, so rotating either alone desyncs the (already-wrong) diff highlights between panes | `mainwindow.cpp:1056-1071` |
| 4 | MEDIUM | REPRODUCED (2 methods) | `selectedText()` (hence `Ctrl+C` clipboard copy) gets line breaks wrong at every non-zero rotation — three different failure shapes at 90/180/270; does NOT affect redaction targeting | `pageview.cpp:645-681` |
| 5 | MEDIUM | REPRODUCED | Opening a new document while presenting silently drops presentation's "one page fitted" layout back to a scrolling FitWidth column, while still fullscreen/toolbar-hidden | `pageview.cpp:112-144` (setDocument), `336-345` (setPresenting) |
| 6 | LOW/nit | REPRODUCED | `closeDocument()` doesn't call `closeSearch()` — a load error while the search bar is open leaves it visible/focused over the empty state until Esc or the next successful open | `mainwindow.cpp:579-592` |
| 7 | LOW/nit | READ-ONLY ASSESSMENT | Hold-to-peek's render-scale target is computed from the unrotated page height, so a peek at a quarter-turn rotation isn't actually sized to ~45% of window height (undersized for portrait) | `mainwindow.cpp:1409-1431` (peekPage) |

**Positive results (checked, no bug found — recorded because absence-of-bug is itself the deliverable for these items):**

| Item | Tag | File:line |
|---|---|---|
| `Document::rotateRect`/`unrotateRect` are exact inverses (290,400 cases) — load-bearing for redaction | REPRODUCED | `document.cpp:392-427` |
| End-to-end selection geometry (real mouse drag -> `selectionBounds()`) correct at every rotation/zoom tried | REPRODUCED | `pageview.cpp` (positionAt/toPageSpace/selectionBounds) |
| `PageView::linkAt` hit-testing correct at every rotation/zoom tried; the early `return nullptr` in the page loop is safe given the layout invariant | REPRODUCED (27/27) | `pageview.cpp:486-516` |
| `paintSearchHits` pixel-exact at all 4 rotations — the positive control the diff-bands fix direction leans on | REPRODUCED | `pageview.cpp:783-806` |
| Esc chain (overlay -> search -> presentation, innermost first) correct in every combination tried, including the untested 3-deep stack; one real Escape key event to the focused search field closes it exactly once | REPRODUCED | `mainwindow.cpp:357-375` |
| Keyboard routing: no window shortcut swallows typing, F/Shift+F reach the right handler in each focus state, re-entrant overlay triggers (Ctrl+T over Ctrl+K) swap cleanly, focus never stranded across error/presentation/compare transitions | REPRODUCED | `mainwindow.cpp` (buildToolBar shortcuts), `overlay.cpp` (present/dismiss) |
| Night mode is geometrically orthogonal to every rotation/zoom/selection/link/diff-band finding above | READ-ONLY ASSESSMENT | `pageview.cpp:291-305`, `424-459` |

---

## Finding log

### [REPRODUCED — NEGATIVE RESULT, proved correct — LOAD-BEARING for redaction] rotateRect / unrotateRect are exact algebraic inverses

**File:** `src/document.cpp:392-427` (`Document::unrotateRect`, `Document::rotateRect`)

By hand algebra, for unrotatedSize=(w,h) and rect=(x,y,rw,rh):

- Rotate90: `rotateRect` gives `(h-y-rh, x, rh, rw)`. Feeding that into
  `unrotateRect`'s Rotate90 branch `(rect.y(), h-rect.x()-rect.width(),
  rect.height(), rect.width())` gives `(x, h-(h-y-rh)-rh, rw, rh) = (x, y, rw,
  rh)`. Exact.
- Rotate180: `rotateRect` gives `(w-x-rw, h-y-rh, rw, rh)`. `unrotateRect`'s
  Rotate180 branch is the same formula applied again (180° is an involution):
  `(w-(w-x-rw)-rw, h-(h-y-rh)-rh, rw, rh) = (x, y, rw, rh)`. Exact.
- Rotate270: symmetric to Rotate90. Exact.

All three non-zero rotations are pure add/subtract of doubles (no
multiplication, no trig), so there is no floating-point mechanism that would
break the round trip for finite, reasonably-scaled inputs — no catastrophic
cancellation is possible since every term is a direct difference of two
already-computed quantities, not an accumulated sum.

**Verified empirically** with a compiled harness (`/tmp/rt_probe.cpp`, built
via `g++ -std=c++20 -O2 -fPIC` against the project's real
`build/CMakeFiles/mergen.dir/src/document.cpp.o` and the other .o files per
the link line given in the task — not a reimplementation) that called the
real linked `Document::rotateRect`/`unrotateRect` for:
- all 4 rotations,
- 3 page sizes: (595,842) A4-ish, (300,900) non-square, (1,1) degenerate-small,
- an 11-value x-coordinate grid and 11-value y-coordinate grid (negative,
  zero, sub-pixel, page-edge coordinates, page-sized, larger-than-page, and
  ~1e7 "huge") crossed with a 10-value width grid and 10-value height grid
  (including negative extents and zero extents),
- both compositions: `unrotate(rotate(r)) == r` AND `rotate(unrotate(r)) == r`
  (left- and right-inverse, both directions).

That is 3 x 4 x 11 x 11 x 10 x 10 x 2 = **290,400 cases**. Command:
`QT_QPA_PLATFORM=offscreen /tmp/rt_probe`.

Result: **0 mismatches out of 290,400 cases**, exact bitwise equality
(`QRectF::operator==`, not fuzzy-compare) in every case including the
degenerate zero-size, negative-size, and huge-coordinate ones.

**Conclusion: this is not a bug.** The single caller of `unrotateRect`
(`PageView::selectionBounds`, pageview.cpp:370) passes
`m_doc->pageSize(from.page)` — confirmed to be the *unrotated* page size
(`Document::pageSize`, documented as "before rotation is applied",
document.cpp:344) — which is the argument `unrotateRect` requires to be the
inverse of whatever rotation produced the rect it's given. No swapped-argument
or mismatched-size bug found at that call site either. This closes out the
single highest-value item in scope: **redaction geometry is not
mis-targeted by this transform pair**, given correct call-site usage (which is
what's actually deployed).

Still to verify (separate finding, in progress): whether the *word boxes fed
into* `selectionBounds` end-to-end (a real click-drag on screen, at a given
zoom+rotation, through to the unrotated rect handed to `Redact::run`) lands on
the visually-selected word. That is a stronger, end-to-end claim than the
static round-trip proven here.

---

### [REPRODUCED — HIGH SEVERITY] Compare diff bands land in the wrong place whenever the page is rotated 90/180/270

**Files:**
- `src/mainwindow.cpp:1088-1153` (`MainWindow::computeDiff`) — always renders
  both documents at `Poppler::Page::Rotate0` and expresses each differing band
  as a fraction of that UNROTATED render's height.
- `src/pageview.cpp:387-401` (`PageView::paintDiffBands`) — paints each band
  as `top = box.top() + band.first * box.height()` where `box` is
  `m_layout.at(page)` at the view's **current** rotation, with **no call to
  `Document::rotateRect` and no rotation-dependent logic at all.**

**One-sentence description:** diff-band fractions are computed against the
unrotated page but painted directly against the current on-screen box with no
coordinate transform, so at 90°/270° the band is drawn as a horizontal strip
spanning the wrong axis entirely, and at 180° it is not vertically flipped, so
it appears on the opposite side of the page from the content that actually
differs.

**Trigger:** open a document, enter compare against a second revision that
differs on some page (e.g. `outline.pdf` vs `outline-v2.pdf`, which differ on
page 2 per the task brief), then rotate (`Ctrl+R`) while still comparing.
Reproduced directly by setting `PageView::setDiffBands()` (the same public API
`computeDiff()` calls) and rotating, without even needing full compare mode.

**Contrast with the positive control:** `PageView::paintSearchHits`
(pageview.cpp:783-806) does this correctly — it calls
`Document::rotateRect(m_hits.at(i).second, size, m_rotation)` before mapping
to screen, exactly matching MZ.md's stated design ("search hits are stored
UNROTATED and transformed at paint time"). `paintDiffBands` is the one caller
of the pattern that's missing that step. This makes the bug very likely a
straightforward omission rather than a considered tradeoff — nothing in
MZ.md §9's Compare section addresses rotation, so this isn't a documented
deliberate simplification.

**REPRODUCED.** Harness: `/tmp/diffband_probe.cpp`, built against the real
`pageview.cpp.o`/`mainwindow.cpp.o`. Method: for each rotation, grab the
viewport with `setDiffBands(0, {{0.10, 0.30}})` cleared vs set, and take the
exact pixel bounding box of the diff between the two grabs — this recovers
*precisely* what `paintDiffBands` painted, in viewport pixels, with no
guessing. Compare that to two independently-computed hypotheses: (a) the
"naive" formula literally in the source (rotation-oblivious), and (b) the
geometrically-correct rect obtained by running the SAME unrotated band rect
through `Document::rotateRect` (the function proven exact above) before
mapping to screen — i.e. what `paintSearchHits`'s pattern would produce if
applied here. Both hypotheses clipped to the viewport rect before comparing,
since `grab()` only captures the visible area (needed for the Rotate180 case,
where the predicted band is partly scrolled above y=0 after the
anchor-restore that rotating triggers).

Command: `QT_QPA_PLATFORM=offscreen /tmp/diffband_probe`. Result (test.pdf
page 0, 595x842pt, zoom fixed at 1.0 so it's not a confound):

| Rotation | Actual painted rect | Matches naive (buggy) formula | Matches geometrically-correct rect |
|---|---|---|---|
| 0   | x=195 y=96  w=595 h=169 | **YES** (exact) | YES (they coincide at 0°) |
| 90  | x=72  y=72  w=842 h=119 | **YES** (exact) | no (correct would be x=661 y=12 w=169 h=595 — a tall narrow strip, not a wide short one) |
| 180 | x=195 y=0   w=595 h=91  | **YES** (exact, after viewport-clipping the naive rect which is predicted to start at y=-78) | no (correct would be x=195 y=427 w=595 h=169) |
| 270 | x=72  y=72  w=842 h=119 | **YES** (exact) | no (correct would be x=156 y=12 w=169 h=595) |

At every rotation the actual output is bit-for-bit what the rotation-oblivious
formula predicts, and diverges from the geometrically correct one exactly as
predicted — at 90°/270° the band should be a **tall vertical** strip
(169 wide x 595 tall) but is actually painted as a **wide horizontal** strip
(842 wide x 119 tall dictated purely by the transposed on-screen box); at 180°
it should sit in the bottom half of the page (y=427) but is actually painted
in the top half (y=0-91, i.e. still anchored near y=96 like the unrotated
case, just scrolled).

**Practical impact:** in compare mode with the page rotated, the highlighted
"this changed" region does not correspond to where the change actually is.
For a 90°/270° rotation the mark degrades into an unrelated horizontal stripe;
for 180° the mark appears in the mirror-opposite half of the page from the
real change. This is a visual-correctness bug in a shipped, documented v2.0
feature (Z9/Compare) — not a crash, but actively misleading, which for a
"tells you what changed" tool is close to as bad as not marking anything.
Severity: **HIGH** (silently wrong output in normal use of a headline
feature; not security-critical the way redaction would be, but confidently
wrong, discoverable by any reader who rotates while comparing).

**Suggested fix direction:** in `PageView::paintDiffBands`, build the
unrotated band rect (`QRectF(0, band.first*unrotatedHeight, unrotatedWidth,
(band.second-band.first)*unrotatedHeight)` using `m_doc->pageSize(page)`),
run it through `Document::rotateRect(..., m_rotation)`, then map to screen via
the same `fromPageSpace`-style transform `paintSearchHits` already uses,
instead of taking fractions of the current `box` directly. Mirrors the
existing, already-correct `paintSearchHits` pattern almost exactly.

---

### [REPRODUCED — HIGH SEVERITY] Opening any new document while comparing leaves compare mode dangling: two unrelated documents shown side by side, still labelled as "comparing," with stale diff bands

**Files:**
- `src/mainwindow.cpp:516-573` (`MainWindow::openPath`) — no reference to
  `m_compareDoc`, `m_compareView`, or `isComparing()` anywhere in the
  function.
- `src/mainwindow.cpp:579-592` (`MainWindow::closeDocument`) — dismisses the
  overlay and resets the primary document/view, but likewise never touches
  compare state.
- `src/mainwindow.cpp:1038-1075` / `1077-1086` (`enterCompare` /
  `leaveCompare`) — `leaveCompare()` is the only place that tears down
  `m_compareView`/`m_compareDoc` and clears `m_view`'s diff bands; nothing
  calls it from `openPath` or `closeDocument`.

**One-sentence description:** every ordinary way of opening a document —
`Ctrl+O`, drag-and-drop, the recent-files menu, the control-socket `open`
command — is fully enabled while comparing and, when used, replaces only the
primary document/view while leaving the second (comparison) `PageView`,
`Document`, and the primary view's stale diff-band highlights all in place,
so the reader ends up looking at two completely unrelated documents side by
side, still reported as "comparing," with diff marks computed for a document
pair that no longer includes what's on screen.

**Trigger (both variants reproduced):**
1. Open document A, `enterCompare(B)` (A vs B differ on some page), then
   `openPath(C)` where C is a third, unrelated document — e.g. the toolbar
   Open button, a drag-dropped file, a recent-files entry, or `mergen-ctl open
   C` over the control socket. All route through the same `openPath()`.
2. Open document A, `enterCompare(B)`, then `openPath()` on a path that fails
   to load (nonexistent file, bad permissions, corrupt PDF) — `showError()` ->
   `closeDocument()` closes only the primary document.

**REPRODUCED.** Harness: `/tmp/compare_leak_probe.cpp`, driving real
`MainWindow` objects (`outline.pdf` vs `outline-v2.pdf` for the compare pair,
which genuinely differ, confirmed via `hasDiffBands()` before the probe
action in each case). Command: `QT_QPA_PLATFORM=offscreen
/tmp/compare_leak_probe`.

**Variant 1 result** (open an unrelated third document, `test.pdf`, while
comparing `outline.pdf` vs `outline-v2.pdf`):
```
isComparing() after opening an unrelated doc: true
PageView count after: 2
primary view still carries diff bands (computed for a DIFFERENT document pair): true
```
The primary `PageView` now shows `test.pdf`; the second `PageView` still
shows `outline-v2.pdf` (the old comparison target, now orphaned — unrelated to
`test.pdf`); `isComparing()` still reports `true`; and `test.pdf`'s pages
carry diff-band highlights that were computed by comparing `outline.pdf`
against `outline-v2.pdf`, i.e. highlights that mean nothing about `test.pdf`
at all and will land on whatever page indices happen to overlap.

**Variant 2 result** (load error on the primary document while comparing):
```
isComparing() after a load ERROR on the primary doc: true
PageView count after: 2
```
The primary view is closed to the empty/error state (per
`closeDocument()`), but the second `PageView` is still on screen showing the
old comparison document in full, next to an empty pane, while
`isComparing()` still reports `true` — a "comparison" between a document and
nothing.

**Why this matters:** MZ.md §5's ruling #4 is explicit that "[compare] is a
mode, not a workspace, and it exits back to one document" — the architecture
promises exactly one clean way out of compare (`leaveCompare()`, `Ctrl+D`,
back to the left-hand document), specifically to avoid the multi-document
workspace state §5 otherwise bans. This gap reopens exactly that: a
reachable state with two independent, unrelated documents on screen and no
single action that is "the" comparison. Combined with the diff-band rotation
bug above, a reader in this state sees highlighted regions that correspond to
neither document actually visible. Severity: **HIGH** — reached through
completely ordinary actions (every document-opening path is enabled during
compare; nothing warns or blocks), produces a persistently confusing/wrong UI
state with no crash to signal something went wrong, and directly contradicts
a documented architectural invariant (§5 ruling #4).

**Suggested fix direction:** `MainWindow::openPath()` should call
`leaveCompare()` at its start whenever `isComparing()` (or refuse the open
outright until compare is exited — either honours "exits back to one
document"). `closeDocument()` should do the same, so a load error while
comparing doesn't strand the second view. Both are one-line additions given
`leaveCompare()` already exists and already does the right cleanup
(deletes `m_compareView`, resets `m_compareDoc`, clears `m_view`'s diff
bands).

---

### [REPRODUCED — POSITIVE RESULT] End-to-end selection geometry is correct at every rotation/zoom tested — this is the redaction-relevant claim, and it holds

Before the next finding (which is about `selectedText()`'s formatting, not
its geometry), the geometric claim that actually matters for redaction safety
was tested end-to-end and holds. Harness: `/tmp/selection_probe.cpp`, driving
a real `MainWindow`, synthesizing real `QMouseEvent`s at `v->viewport()`
(press/move/release — not calling any private method directly) at computed
screen points, then reading back the public `PageView::selectedText()` /
`selectionBounds()`.

Method: took the real word box for "haystack" on `test.pdf` page 0 at
Rotate0 as ground truth (`Document::words`, unrotated). For 8
rotation x zoom combinations (0/90/180/270 deg at 50-250% zoom), computed
where that word's centre appears on screen right now via
`Document::rotateRect` (the proven-exact function) plus a faithful
replication of `PageView`'s own layout math, synthesized a real mouse
press+release there, and checked `selectionBounds()` against the *unrotated*
ground-truth rect (tolerance 1.5pt).

Result: **8/8 single-word combinations passed** — correct word selected,
correct page, rect within tolerance of the true unrotated box, at every
rotation and zoom tried, including cases where the computed click point
ended up scrolled outside the visible viewport (e.g. y=-206) — Qt's
`QAbstractScrollArea` does not clip synthesized events to the visible area, so
this exercised the coordinate math correctly even though such a point isn't
literally clickable by a real user at that scroll position; it doesn't
weaken the conclusion since the geometry is what's under test.
**3/4 multi-word drag-select combinations also passed** on bounds (0/180/270
degrees); the 90-degree case is the subject of the next finding — its
*bounds* still came out correct (union rect matched, page correct), only its
*text string* was wrong. So: **selection/redaction targeting itself is sound
at every rotation and zoom tested; no evidence of a geometry bug here.**

---

### [REPRODUCED — MEDIUM SEVERITY] `selectedText()` inserts wrong line breaks (or omits real ones) whenever the page is rotated — corrupts clipboard copy, but NOT redaction targeting

**File:** `src/pageview.cpp:645-681` (`PageView::selectedText`)

**One-sentence description:** the same-line/new-line heuristic
(`word.box.top() >= previousWord.box.center().y()`, pageview.cpp:672) assumes
un-rotated top-to-bottom, left-to-right text flow and is applied unchanged
regardless of `m_rotation`, so at every rotation other than 0 degrees the
line breaks in the returned string are wrong — verified wrong in three
different ways at three different rotations, not just "slightly off."

**Root cause (from the code, stated precisely, without assuming poppler
internals beyond what was directly measured):** the heuristic decides "this
word starts a new line" purely by comparing its `box.top()` to the *previous*
word's `box.center().y()`, in the CURRENT rotation's coordinate frame. This
is correct only when consecutive same-line words share close-to-equal `top()`
and a genuine new line has strictly larger `top()` than the line before it —
true for Rotate0, and (verified false, see below) not reliably true once the
page is rotated, because the geometric top-to-bottom axis and the logical
reading-order axis part ways under rotation.

**REPRODUCED**, two ways:

1. `/tmp/selection_probe.cpp`: a real mouse drag from "searchable" to
   "needle" (three words that are on ONE line in the source PDF) at
   `90deg@100%` produced `selectedText() == "searchable\nhaystack\nneedle"`
   (one word per line) instead of `"searchable haystack needle"`. The SAME
   drag at 0/180/270 degrees correctly produced the single space-joined
   line. (`selectionBounds()` was still numerically correct in the 90-degree
   case — this is a text-formatting bug, not a geometry bug; see the previous
   finding.)

2. `/tmp/wordbox_diag2.cpp`: replayed the exact heuristic over *every* word
   on `test.pdf` page 0 (two real lines: "MERGEN test page 1" and
   "searchable haystack needle page 1"), rendering `|` for a heuristic
   newline and `_` for a heuristic space, at each rotation:

   | Rotation | selectedText() line structure (`|` = newline, `_` = space) | Correct? |
   |---|---|---|
   | 0   | `MERGEN_test_page_1` \| `searchable_haystack_needle_page_1` | **Yes** — exactly the 2 real lines |
   | 90  | `MERGEN` \| `test` \| `page` \| `1` `_` `searchable` \| `haystack` \| `needle` \| `page` \| `1` | No — splits ONE line into 4 fragments, AND joins the two DIFFERENT lines with a space instead of a break |
   | 180 | `MERGEN_test_page_1_searchable_haystack_needle_page_1` | No — the real line break between the two lines is lost entirely; everything comes back as one line |
   | 270 | `searchable_haystack_needle_page_1` \| `MERGEN_test_page_1` | Partially — 2 lines are produced (no over/under-split), but in reverse order from the document's actual line order |

   Command: `QT_QPA_PLATFORM=offscreen /tmp/wordbox_diag2`. So **every
   non-zero rotation gets the line structure wrong**, in three qualitatively
   different ways — this is broader than just "90 degrees," as first
   suspected from the single-selection test.

**Blast radius — confirmed by grepping every caller of `selectedText()`**
(`grep -rn selectedText src/`, 3 call sites total):
- `PageView::copySelection()` (pageview.cpp:684) calls it **raw** and puts
  the result straight on the clipboard via `QGuiApplication::clipboard()`.
  **This is the one real bug**: `Ctrl+C` on a rotated, multi-word,
  same-original-line selection copies text with spurious newlines (or a
  missing line break) instead of the expected text.
- `MainWindow::redactSelection()` (mainwindow.cpp:942) calls
  `.simplified()` on the result before using it, which collapses every
  whitespace run (including any newline this bug inserts) to a single
  space — **redaction's "does the target text still exist" verification
  check is unaffected**, since it never depends on the exact whitespace, and
  the region actually removed is driven by `selectionBounds()` (proven
  correct above) plus `Redact::run`'s own position-tracking, not by this
  string.
- `MainWindow::showCommands()` (mainwindow.cpp:1285) only checks
  `.trimmed().isEmpty()` to decide whether to offer "Redact selection" in the
  command overlay — internal newline-vs-space differences don't change that
  emptiness check.

**Severity: MEDIUM.** Real, reproducible, user-facing correctness bug
(copy-to-clipboard produces mangled text whenever the document is rotated and
the reader copies more than one word), reachable through completely ordinary
use (rotate, select, Ctrl+C). Contained: it does not affect what redaction
removes, does not affect which words get selected, does not crash, and is
invisible unless the reader actually pastes the copied text somewhere and
looks closely (a copy that silently line-wraps every word still reads
correctly to a human glancing at it, which is likely why this has not been
noticed).

**Suggested fix direction:** make the newline decision rotation-aware —
either (a) compare along the axis that is "reading order" for the current
rotation rather than always literal screen-`top()` (e.g. transform both
boxes back to unrotated space via `Document::unrotateRect` before comparing,
since reading order is invariant there and Rotate0's logic is already
correct), or (b) track line breaks using poppler's own line/paragraph
boundaries if `Poppler::TextBox` exposes them, rather than inferring line
breaks from raw geometry at all.

---

### [REPRODUCED — MEDIUM SEVERITY] Opening a new document while presenting silently breaks presentation's "one page fitted" promise (falls back to a scrolling FitWidth column)

**Files:**
- `src/pageview.cpp:112-144` (`PageView::setDocument`) — unconditionally sets
  `m_zoomMode = ZoomMode::FitWidth` (line 123) and never reads or touches
  `m_presenting`.
- `src/pageview.cpp:336-345` (`PageView::setPresenting`) — only sets
  `ZoomMode::FitPage` (via `setFitPage()`) at the moment presentation is
  *entered* (`if (on) setFitPage();`); nothing re-asserts FitPage afterward.
- `src/mainwindow.cpp:516-573` (`MainWindow::openPath`) — no presentation-mode
  awareness, same gap pattern as the compare-mode finding above.

**One-sentence description:** `PageView::setDocument()` always resets the
zoom mode to `FitWidth`, so opening any new document (`Ctrl+O`, drag-drop,
recent-files menu, or the control-socket `open` command — all fully enabled
and reachable by keyboard even though the toolbar is hidden while presenting)
while already presenting leaves the window fullscreen with the toolbar
withdrawn and black surround (i.e. still reporting `isPresenting() == true`),
but silently turns the "one page at a time fitted to the screen" layout MZ.md
§6 promises into an ordinary scrolling multi-page FitWidth column.

**REPRODUCED.** Harness: `/tmp/presentation_probe.cpp`, real `MainWindow`.
Command: `QT_QPA_PLATFORM=offscreen /tmp/presentation_probe`.

```
--- presentation mode uses FitPage on entry ---
  PASS  presenting
  PASS  zoom mode is FitPage while presenting (one page fitted)

--- opening a NEW document while presenting ---
  isPresenting() after opening a new doc: true
  zoomMode() after: FitWidth
  toolbar visible: false   window fullscreen: true
  PASS  still reports presenting (fullscreen/black-surround/no-toolbar all still active)
  FAIL  zoom mode is STILL FitPage after opening a new doc while presenting
```

So the window keeps every OTHER outward sign of presenting (fullscreen,
black surround from `surroundColour()`'s `m_presenting` check, no toolbar),
while the actual page layout reverts to the ordinary reading column. This is
the presentation-mode analogue of the compare-mode finding above: a whole
class of "replace the primary document" actions is left fully enabled in a
mode whose entire point is a controlled, singular presentation state, and
none of them account for that mode.

**Severity: MEDIUM.** Self-evident once triggered (the reader immediately
sees a scrolling column instead of a single fitted page, so it's not a
silent-corruption class of bug like the diff-band one above), but it is a
clear regression of a directly-documented behaviour ("shows one page at a
time fitted to the screen", MZ.md §6) reachable through a mechanism MERGEN
specifically built for this kind of scripted workflow (§9's control socket:
"`mergen file.pdf` from a keybinding raises the window already open" —
raising a window that happens to be mid-presentation is not a contrived
scenario).

**Suggested fix direction:** either have `setDocument()` call `setFitPage()`
instead of unconditionally setting `FitWidth` when `m_presenting` is true, or
have `MainWindow::openPath()` (mirroring the compare-mode fix) re-assert
`setFitPage()` after `setDocument()` whenever `m_view->isPresenting()`.

---

### [REPRODUCED — POSITIVE RESULT] The Esc chain (overlay -> search -> presentation) is correct in every combination tried, including the untested 3-deep stack

**File:** `src/mainwindow.cpp:357-375` (the single window-wide `Esc` QAction
in `buildToolBar`).

MZ.md §7: "Esc already closed the search bar in v1.0 and now closes whatever
transient surface is frontmost, innermost first" -> overlay, then search bar,
then presentation. Existing tests cover overlay-while-presenting
(`z7check.cpp`) and search-alone (`z2check.cpp`) separately, but not together.
Tested the case neither covers: **all three active at once**, and the
**search+presentation-without-overlay** ordering.

**REPRODUCED**, harness `/tmp/presentation_probe.cpp` (second half),
command `QT_QPA_PLATFORM=offscreen /tmp/presentation_probe`:

- With overlay + search bar + presentation ALL open simultaneously: Esc #1
  closed only the overlay (search bar and presentation untouched); Esc #2
  closed only the search bar (presentation untouched); Esc #3 finally left
  presentation. Exactly the documented innermost-first order, and confirmed
  that **one Esc never closes two things** even under the full 3-deep stack.
- With only search bar + presentation (no overlay): Esc closed the search
  bar first and explicitly left presentation running, confirming search
  outranks presentation on its own, not just as an artifact of the overlay
  case.
**No bug found here.** This is a clean, evidence-backed confirmation of a
documented behaviour holding under the exact combinations the task asked to
check ("Verify the ordering is right in every combination, that one Esc never
closes two things").

**Correction/follow-up:** an earlier draft of this finding claimed the above
"incidentally confirms" that the search field's own `Qt::Key_Escape` handling
in `MainWindow::eventFilter` (mainwindow.cpp:1748-1753) never competes with
the window-wide `Esc` `QAction`. That claim was not actually earned — every
test above triggered the window-wide action directly via `QAction::trigger()`
(a programmatic call, not a real key event), which never exercises Qt's
shortcut-vs-focused-widget dispatch at all, so it could not have shown
whether the two handlers compete. Closed properly with a dedicated test
sending one real `QKeyEvent(Key_Escape)` to the focused, visible search
field via `QApplication::sendEvent()` (the same event-routing method
confirmed elsewhere in this file to exercise Qt's real shortcut-priority
path — see the keyboard-routing finding below). Harness
`/tmp/esc_direct_probe.cpp`, command `QT_QPA_PLATFORM=offscreen
/tmp/esc_direct_probe`: the search bar closed after exactly one Escape key
event, and focus landed on `PageView` afterward, not stranded. **This does
confirm no double-close and no dead key** — the window-wide `QAction` and the
field's own handler don't both leave a visible mark (whether that's because
one wins outright, or because both fire and `closeSearch()` is simply
idempotent, is not distinguishable from the outside and doesn't need to be:
either way the observable behaviour is correct).

---

### [REPRODUCED — POSITIVE RESULT] Link hit-testing (`PageView::linkAt`) is correct at every rotation/zoom tried, and the early `return nullptr` in the page loop is safe

**File:** `src/pageview.cpp:498-516` (`PageView::linkAt`), `486-496`
(`linksOf`).

`linksOf` fetches `m_doc->pageLinks(page, m_rotation)` — already
rotation-aware at the source, unlike the diff-band case — and is correctly
dropped/refetched on rotation (cleared in `applyScale` alongside `m_words`
whenever `turned` is true; pageview.cpp:1105-1111).

On the loop's early `return nullptr`: page rects in `m_layout` are built by
`relayout()` as a strictly-increasing-`y` single column (`y += h + kPageGap`
every iteration, all starting at `x=0` then only ever moved horizontally by
`moveLeft`), so no two pages' boxes can ever overlap vertically, and a given
viewport point can be contained in at most one page's box. Giving up after
the first (and only possible) containing page is therefore equivalent to
checking every page — not a bug, just a loop that doesn't need to keep going
given that invariant.

**REPRODUCED.** Harness `/tmp/link_probe.cpp`, real `MainWindow`,
`outline.pdf` (one internal link on page 0 -> page index 2, confirmed via
`Document::pageLinks` as ground truth, matching `z3check.cpp`'s existing
extraction test). For 9 rotation x zoom combinations (0/90/180/270 deg at
30-250% zoom), for each: (1) clicked the computed on-screen centre of the
link's rotated area — hit every time, correct target page; (2) clicked a
point on page 0 far from the link — missed cleanly every time (not just
"missed," but specifically confirmed NOT to fall through to some other
page's link); (3) clicked the centre of page 1, which genuinely has no links
— missed every time, i.e. page 1 never incorrectly reports page 0's link
through the early-return path. **27/27 checks passed.** Command:
`QT_QPA_PLATFORM=offscreen /tmp/link_probe`.

**No bug found.** Link hit-testing is solid across the rotation x zoom
combinations tested.

---

### [REPRODUCED — POSITIVE RESULT] Keyboard routing: no window shortcut swallows typing, no key is caught by nothing, re-entrant overlay triggers are handled cleanly, focus is never stranded

Harness: `/tmp/keyboard_focus_probe.cpp`, real `MainWindow`, real
`QKeyEvent`s sent via `QApplication::sendEvent()` (which routes through
`QApplication::notify()`'s actual shortcut-vs-focused-widget priority
mechanism — the same path real input takes — rather than calling a handler
directly, so this tests the real dispatch, not a simulation of it). Command:
`QT_QPA_PLATFORM=offscreen /tmp/keyboard_focus_probe`. **Every check passed.**
Confirms, with evidence rather than by reading the code's own comments:

1. **Every registered window-wide shortcut was enumerated**
   (`grep -n setShortcut src/mainwindow.cpp`) — all 19 are either `Ctrl`/
   `Ctrl+Shift`-modified, the bare non-printable `Esc`/`F5`, or a
   `QKeySequence::StandardKey` role (all Ctrl-based on Linux). **There is no
   bare unmodified single-letter window shortcut anywhere in the app**,
   confirming `F`/`Shift+F` (handled inside `PageView::keyPressEvent`,
   pageview.cpp:982-988, not as a `QAction`) really is the only case that
   needed the special treatment MZ.md's comment describes, and it is the
   only one that got it.
2. Typing plain letters into the search field inserts them literally
   (`"abc"` in, `"abc"` out).
3. Plain `'f'` sent to the **search field** types the literal character and
   does **not** trigger fit-width (`zoomMode()`/`zoom()` unchanged) — the
   worry the code comment raises is empirically not a live bug given the
   current design (F is not a window shortcut at all).
4. Plain `'f'` / `Shift+F` sent to **PageView itself** (focused) DOES trigger
   `setFitWidth()` / `setFitPage()` respectively, confirming the
   inverse-direction requirement — the key that must reach a handler,
   reaches it.
5. `Ctrl+N` sent to the **focused search field** DOES toggle night mode (a
   window shortcut correctly firing despite a text field having focus — this
   is desired, not a bug: there's no legitimate reason a reader should be
   unable to toggle night mode while typing a search term), and the search
   field's text is left untouched (no stray `'n'` leaked through).
6. **Re-entrant overlay presentation** (`Ctrl+T` fired while `Ctrl+K`'s
   command overlay is already open and its field focused) swaps content
   cleanly: the overlay stays presented, the new content (outline list, 3
   entries) replaces the old, the old command `QLineEdit` is completely gone
   from the tree afterward (not leaked/stacked), and the final `Esc` returns
   focus to what had it **before the very first `present()`** — not to
   either intermediate overlay content — matching `Overlay::present()`'s
   `if (!isVisible()) m_focusBefore = ...` guard (overlay.cpp:140-142), which
   deliberately does not overwrite `m_focusBefore` on a re-entrant call.
7. **Focus is never null** in any of the states tried: fresh window with no
   document, after opening a document, after a load error while the search
   bar was open, on entering and leaving presentation, on entering and
   leaving compare. It always lands on `PageView` or a live `QLineEdit`.

**No bug found in keyboard routing or focus return.** This is a
well-evidenced clean bill of health for audit items 6 and 8's core questions.

**One LOW/nit found along the way:** `MainWindow::closeDocument()`
(mainwindow.cpp:579-592) does not call `closeSearch()`. Reproduced: opening
the search bar, then triggering a load error (`openPath("does-not-exist.pdf")`),
leaves the search bar visible and focused, floating over the
empty/error page-view state, until the reader presses `Esc` or successfully
opens another document (whose `openPath()` success path does call
`closeSearch()`, mainwindow.cpp:567). Functionally inert (`startSearch()`
early-returns with no document), not a correctness bug, but a visible
inconsistency with the "search bar belongs to reading a document" framing —
worth a one-line `closeSearch()` added to `closeDocument()` if picked up
incidentally.

---

### [REPRODUCED — addendum to the diff-bands finding above] The two compare panes hold fully independent rotation state, so rotating either one alone desyncs the diff highlights between panes

**File:** `src/mainwindow.cpp:1056-1071` (`enterCompare`'s scroll-lock
binding) — only `verticalScrollBar()->valueChanged` is bound between
`m_view` and `m_compareView`; nothing binds rotation (or zoom).

This matches MZ.md §9's own description of compare ("two page columns, **one
scroll position**") — not a spec violation by itself. But combined with the
diff-band rotation bug above, it has a compounding effect worth recording
alongside it rather than as a separate root cause.

**REPRODUCED.** Harness `/tmp/compare_rotation_probe.cpp`. Command:
`QT_QPA_PLATFORM=offscreen /tmp/compare_rotation_probe`. Entered compare
(`outline.pdf` vs `outline-v2.pdf`), then called `rotateClockwise()` on only
the primary `PageView`:
```
after rotating the PRIMARY view: primary=1 secondary=0
PASS  rotation state is INDEPENDENT between the two compare panes
PASS  both panes still carry diff bands (same band data, now painted against DIFFERENT geometries)
```
Since both panes are handed the identical band-fraction data by
`computeDiff()` (mainwindow.cpp:1142-1143: `m_view->setDiffBands(...)` and
`m_compareView->setDiffBands(...)` with the same `bands`), and
`paintDiffBands` is rotation-oblivious per the finding above, rotating just
one pane now makes that pane's highlight wrong in the way already
documented while the other pane's stays correct — so the two panes'
highlighted regions visibly disagree with each other, not just with the
truth, which is a more obviously-broken presentation than either pane alone.
No new root cause — filed as a severity-reinforcing addendum, not counted
separately.

---

### [READ-ONLY ASSESSMENT] Night mode is geometrically orthogonal to every other finding — checked, not just assumed

Night mode (`PageView::setNightMode`) only affects `cachedPage()`
(pageview.cpp:291-305): `invertLightness()` runs on the decoded `QImage`
before it's cached, and nothing else. `paintEvent` (pageview.cpp:424-459)
draws that image first, then calls `paintSelection`, `paintSearchHits`, and
`paintDiffBands` — all three compute their geometry from `m_layout`/`origin`/
`m_rotation`/`m_zoom` only, with no reference to `m_night` anywhere in any of
them (confirmed by reading all three functions in full, pageview.cpp:387-401,
690-714, 783-806). So every rotation/zoom finding above (and the one
rotation/zoom non-finding, link hit-testing and selection geometry both
holding up) applies identically whether night mode is on or off — this was
checked by reading rather than by re-running every harness a second time
with night mode toggled, since the code path proves there is no coupling to
check. Z5/Z6's existing acceptance tests (`z5check.cpp`, `z6check.cpp`)
already cover night mode's own correctness (hue-preserving inversion,
involution, annotation legibility) directly and are not repeated here.

---

### [REPRODUCED — POSITIVE RESULT, pixel-verified] `paintSearchHits` is exactly correct at all 4 rotations — the diff-bands fix direction is evidence-backed, not just read

The diff-bands finding above cites `paintSearchHits` as "the positive
control" and its suggested fix is "mirror the `paintSearchHits` pattern" —
that claim had only been verified by reading the source, not by pixel
measurement. Closed the gap with the exact same method as the diff-band
probe (grab with hit cleared vs set, pixel-diff bounding box = exactly what
was painted), reusing a known real search hit ("haystack" on `test.pdf`
page 0, via `Document::search` — the same unrotated-rect contract
`SearchWorker`/`addSearchHit` use).

Harness: `/tmp/searchhit_paint_probe.cpp`. Command:
`QT_QPA_PLATFORM=offscreen /tmp/searchhit_paint_probe`. One wrinkle: a lone
hit is also the *current* hit, so `paintSearchHits` adds a 2px outline via
`.adjusted(-1,-1,1,1)` on top of the fill (pageview.cpp:798-803) — the
expected rect is widened by that same margin before comparing, tolerance 3px.

Result: **exact match at all 4 rotations**, not merely within tolerance:

| Rotation | Actual painted rect | Expected (rotateRect + outline margin) |
|---|---|---|
| 0   | x=327 y=183 w=52 h=16 | x=327 y=183 w=52 h=16 |
| 90  | x=727 y=144 w=16 h=52 | x=727 y=144 w=16 h=52 |
| 180 | x=606 y=493 w=52 h=16 | x=606 y=493 w=52 h=16 |
| 270 | x=243 y=423 w=16 h=52 | x=243 y=423 w=16 h=52 |

Note the shape correctly rotates (52x16 at 0/180, transposed to 16x52 at
90/270) — this is the visual signature the diff-band bug's actual output
never showed. **The positive control is now itself proven, not assumed**, so
the diff-bands fix direction (mirror this exact pattern) is evidence-backed.

---

### [REPRODUCED — CRITICAL SEVERITY — CRASH] The password prompt's nested modal event loop stays live to the control socket: a `goto` command received while password-locked crashes the process (SIGSEGV)

**Files:**
- `src/mainwindow.cpp:736-797` (`MainWindow::runCommand`), specifically the
  `"goto"` branch: `if (page < 1 || page > m_doc->pageCount())`.
- `src/document.cpp:115-117` (`Document::pageCount`): `return m_doc ?
  m_doc->numPages() : 0;` — calls straight into poppler with no lock check.
- `src/document.h:102` (`Document::isOpen`): `return m_doc != nullptr;` —
  true for a **locked** document too, since `adopt()` (document.cpp:63-76)
  sets `m_doc = std::move(doc)` unconditionally, including on the
  `NeedsPassword` path, before the caller ever prompts for a password.
- `src/mainwindow.cpp:653-672` (`MainWindow::promptForPassword`) — blocks the
  main thread inside `QInputDialog::getText()`'s nested `QDialog::exec()`
  event loop for as long as it takes the reader to answer (or forever, if
  they never do).
- `src/control.cpp:65-90` (`Control::onConnection`) — driven by
  `QLocalServer::newConnection`, a plain Qt signal on a `QSocketNotifier`,
  which keeps firing inside *any* nested event loop, including a modal
  dialog's — this is not gated by widget modality in any way, because
  modality is specifically about blocking mouse/keyboard delivery to other
  widgets, and a `QLocalServer` connection is neither.

**One-sentence description:** `Document::isOpen()` reports `true` for a
password-locked document (it only checks the poppler handle is non-null, not
whether it's still locked), so `MainWindow::runCommand`'s `"goto"` handler's
own `isOpen()` guard is a no-op during the password prompt, and its very next
line calls `Document::pageCount()` — which calls straight into poppler's
`Catalog::getNumPages()` on the still-locked document and **segfaults inside
`pthread_mutex_lock`** — and the control socket that delivers this command is
fully live and listening throughout the entire password prompt, because a
modal `QDialog::exec()` only blocks *other UI input*, not socket activity on
the very same (still-running, nested) event loop.

**Exact reproduction (both variants below crash 100% of the time, confirmed
over 3 separate runs):**

1. Cold start, zero prior state — the minimal case: launch MERGEN on a
   password-protected PDF (`locked.pdf`, created for this test via `qpdf
   --encrypt secret123 secret123 256 -- test.pdf locked.pdf`), and, before
   answering the password prompt, send `goto 1` over the control socket
   (exactly what `mergen-ctl goto 1`, or any keybinding-driven script,
   would send). **Crashes the whole process.**
2. Same, but with a normal document (`test.pdf`, 3 pages) already open and
   fully on screen when a *second* `openPath("locked.pdf")` is issued (e.g.
   the reader opens a different, password-protected file from the recent-files
   menu or a drag-drop while already reading something) — same crash, same
   cause, and additionally demonstrates that for the whole duration of the
   prompt, `MainWindow::m_doc`'s wrapped poppler handle has *already* been
   silently replaced with the new, locked document (`Document::adopt()` does
   this unconditionally, before any password is asked for) while `PageView`
   is still fully displaying the *old* document's rendered pages and layout —
   a real document/view mismatch window exists even independent of the crash.

**REPRODUCED**, harnesses `/tmp/modal_race_probe2.cpp` (variant 1, cold
start) and `/tmp/modal_race_probe3.cpp` (variant 2, prior document open),
each opening the locked PDF and auto-dismissing the password dialog via a
1.8s internal timer (Escape) so the process exits cleanly if nothing else
disturbs it. The race itself is driven by a **genuinely separate OS
process** (confirmed necessary — see method note below) connecting to the
real control socket and sending one line, exactly as `mergen-ctl` or a
second `mergen` instance would:

```
$ QT_QPA_PLATFORM=offscreen /tmp/modal_race_probe2 &          # blocks on the password prompt
$ python3 -c 'import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); \
              s.connect("/run/user/1000/mergen-1000.sock"); s.send(b"goto 1\n"); print(s.recv(1024))'
client error (crash likely): [reply never arrives, connection drops]
Segmentation fault (core dumped)
$ echo $?
139
```

Full backtrace (`coredumpctl gdb <pid>`, `bt full`), confirmed on two
independent runs of variant 2:

```
#0  pthread_mutex_lock () from libc.so.6
#1  Catalog::getNumPages() from libpoppler.so.163
#2  mergen::MainWindow::runCommand(QString const&, QString const&)
#3  [lambda in MainWindow::listenForCommands()]
#4  mergen::Control::onConnection()
#5  (Qt signal dispatch) from libQt6Core.so.6
#6  (QLocalSocket internals) from libQt6Network.so.6
#7  (Qt signal dispatch) from libQt6Core.so.6
#8  QSocketNotifier::event(QEvent*)
#9  QApplicationPrivate::notify_helper(QObject*, QEvent*)
#10 QCoreApplication::notifyInternal2(QObject*, QEvent*)
#11 (Qt event dispatch) from libQt6Core.so.6
#12-14 g_main_context_iteration / glib internals
#15 QEventDispatcherGlib::processEvents(...)
#16 QEventLoop::exec(...)
#17 QDialog::exec()
#18 QInputDialog::getText(...)
#19 mergen::MainWindow::promptForPassword(QString const&)
#20 mergen::MainWindow::openPath(QString const&)
#21 main()
```

This is an exact, unambiguous trace of the whole mechanism: `main()` calls
`openPath()`, which blocks in `promptForPassword()`'s `QInputDialog::exec()`
(frames 17-20); that nested loop is still pumping the process's one event
queue, so a real `QSocketNotifier` event for the incoming control-socket
connection is delivered through it (frames 8-16) straight into
`Control::onConnection()` (frame 4) and the `runCommand` lambda (frame 3),
landing in `MainWindow::runCommand` (frame 2) — at the `"goto"` branch's
`m_doc->pageCount()` call, straight into poppler's `Catalog::getNumPages()`
(frame 1), which dies inside a mutex lock (frame 0) — almost certainly
because the `Catalog` isn't (fully) constructed for a document that's still
password-locked; poppler is simply never expected to be asked this while
locked, and nothing on the MERGEN side stops it from being asked.

**Method note (worth recording since it cost real time):** the same test
constructed with the racing client living in the *same process* as the
server (either via `Control::send()`'s own `QLocalSocket`, or via
`QProcess::waitForFinished()` spawning the client from inside the blocked
process) reliably produced **empty replies with no crash**, not because the
bug wasn't real but because a single-thread, single-process client+server
pair sharing one event loop for both the blocking client-side `waitFor*`
calls and the server-side connection handling is a fundamentally different
(and non-representative) scenario from two independent OS processes talking
over the socket, which is what `mergen-ctl`/a second `mergen` instance
actually is. Verified the socket mechanism itself was never the problem with
a plain two-Python-process Unix-socket smoke test first. The lesson: **this
class of race must be tested with a genuinely separate process on the
client side**, or it silently fails to reproduce and looks like a false
alarm.

**Severity: CRITICAL.** A crash (denial of service) of the whole
application, 100% reproducible, requiring nothing more than: (1) a
password-protected PDF, and (2) one line sent to a socket that MZ.md §9
explicitly documents as designed to "accept local connections only" from
"the reader" — but says nothing about restricting *what* the reader can
trigger while a modal prompt is up, and the code's own guards
(`Document::isOpen()`, `m_opening`) were evidently written with only the
*other UI* in mind ("leaves the toolbar live" — mainwindow.cpp:517-518's own
comment), not the socket. Any local process with access to
`$XDG_RUNTIME_DIR/mergen-$UID.sock` (any process running as the same user,
by construction — the socket has no finer-grained authentication) can crash
a running MERGEN instance on demand the moment it shows a password prompt,
which makes this both a correctness bug and a trivial local DoS. This is the
single most severe finding in this audit, above every HIGH finding recorded
elsewhere in this file.

**Suggested fix direction:** the root cause is that `Document::isOpen()`
conflates "has a poppler handle" with "is usable," and every caller
(`runCommand`, `openSearch`, etc.) relies on `isOpen()` to mean the latter.
Either (a) make `Document::isOpen()` return `false` while
`m_doc->isLocked()` is true (simplest; matches what every caller already
assumes it means), or (b) add a distinct `Document::isUsable()` /
`isLocked()` accessor and audit every `isOpen()` call site in
`MainWindow::runCommand` to use it instead. Either way, `pageCount()` (and
similarly-shaped accessors) should also defensively return 0 while locked,
as a second line of defense, since the real fix is stopping the reachable
call rather than hardening a function that should never legitimately be
asked. Separately, `MainWindow::openPath()`'s comment already flags the
nested-loop risk for *itself* (`m_opening` guards re-entering `openPath`) —
the same instinct needs to extend to every other socket-reachable method,
not just `openPath`.

---

**Scoping note (confirmed, not assumed):** raced `search haystack` instead of
`goto` against the same cold-start setup — **does not crash** (`client got:
'ok'`, clean exit). This is not because `runCommand`'s `"search"` branch is
any safer at the `isOpen()` level (it has the exact same no-op guard), but
because `startSearch()` hands the work to `SearchWorker`, which creates its
*own*, independent poppler handle on a worker thread and explicitly checks
`doc->isLocked()` before doing anything (`document.cpp`, `SearchWorker::run`)
— a defensive check that happens to exist for an unrelated reason (thread
safety: "poppler documents are not safe to share between threads") and
incidentally saves this one path. `"goto"` has no equivalent guard because it
operates directly on the shared, main-thread `m_doc`. This confirms the bug
is specifically "nothing stops a direct `pageCount()`/similar call on the
shared handle while locked," not "the control socket is broadly unsafe
during a prompt" — precise enough to know that fixing `Document::isOpen()` (or
guarding `pageCount()` itself) closes the actual hole, rather than needing to
firewall the whole socket during every modal dialog.

---

### [READ-ONLY ASSESSMENT — LOW/nit] Hold-to-peek's target size is computed against the unrotated page height, so a rotated peek isn't actually ~45% of window height

**File:** `src/mainwindow.cpp:1409-1431` (`MainWindow::peekPage`).

```cpp
const QSizeF size = m_doc->pageSize(page);              // UNROTATED size
const double target = height() * 0.45;
const QImage image = m_doc->renderPage(page, target / size.height(), m_view->rotation());
```

`size` is the unrotated page size (`Document::pageSize`, documented as
"before rotation is applied"), so `target / size.height()` is a scale factor
chosen to make the *unrotated* height come out to `target` px. But
`renderPage` is then asked to render at `m_view->rotation()`. At a quarter
turn (90/270), the rendered image's actual height is the unrotated *width*
times that same scale, i.e. `target * (unrotatedWidth / unrotatedHeight)` —
not `target`. For a portrait page (the common case) that's smaller than
intended (undersized peek); for a landscape page it would render taller than
intended, though `Overlay::relayout()`'s own `kMaxHeightFraction` (0.78 of
the viewport) bounds how far that can go rather than letting it overflow.

Not independently reproduced with a harness — read-only, and the
`Document::rotateRect`/`renderPage` pieces involved are already proven
correct elsewhere in this file, so this is purely "the size target math
doesn't account for rotation," not a coordinate-correctness bug: the peek
still shows the *right page*, right-side-up, just not sized to the intended
fraction of the window when rotated a quarter turn. **Severity: LOW/nit** —
cosmetic sizing only, no wrong content, no crash. Fix direction: use
`m_view->rotation()`-transposed dimensions (swap width/height for a quarter
turn) when computing `target / size.height()`, mirroring the same
`quarterTurn` pattern already used in `PageView::relayout()` and
`zoomForFitMode()`.

---
