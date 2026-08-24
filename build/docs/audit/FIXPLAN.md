# Fix plan — grouped by root cause

**Final.** All eleven agents reported. 52 findings, 21 root causes.

52 findings do not mean 52 fixes. Several collapse into one defect each, and
fixing the cause closes every symptom at once — whereas patching symptoms
individually leaves the cause in place to produce new ones. Ordered by severity
then by how much each buys.

Confidence is marked: **[R]** reproduced by me, **[A]** reported by an agent and
recorded but not re-reproduced here.

---

## A — `isOpen()` does not mean "usable"   [R]
Covers V9, V32, V32a. **One line closes a crash reachable from ten callers.**

`Document::isOpen()` is `m_doc != nullptr`, which is true for a document poppler
has loaded but not decrypted, because `adopt()` deliberately keeps the handle so
`unlock()` can retry. Reproduced in three lines with no socket and no dialog:
`openPath` → `NeedsPassword` → `isOpen()` true → `pageCount()` → SIGSEGV.

Exposed callers, all of which begin with that check: `runCommand`'s
`goto`/`search`/`quit`, `redactSelection`, `showProperties`, `showOutline`,
`markPortal`, `followPortal`, `chooseComparison`, `computeDiff`.

**Fix:** track the locked state explicitly; `isOpen()` false while locked,
`unlock()` flips it on success. Do NOT add guards caller by caller.
**Test:** the three-line repro, plus a socket `goto` during the password prompt
driven from a *separate OS process* (same-process clients give a false negative —
S4's method note).

## A2 — `isNull()` is not a failure check   [R]
Covers V43. **Three lines, four call sites, and it stops silently wrong output.**

Poppler refuses allocations above 2^31 bytes by returning a **1x1 image that is
not null** (with "Bogus memory allocation size" on stderr). Every `isNull()`
guard in MERGEN passes on it. Reproduced: 7.2 GB requested -> 1x1, `isNull()`
false.

Sites: `mainwindow.cpp:1519` (print), `pageview.cpp:298` (page cache),
`mainwindow.cpp:1104`, `mainwindow.cpp:1421`.

Silent consequences: print scales one pixel to fill the sheet (a solid black
page); the cache stores 1x1 as the page; **compare reports two documents
identical** because both rendered to 1x1.

**Fix:** compare the returned size against the requested size; a mismatch is a
failure. Ranks with A because it is nearly as cheap and it stops the application
producing confidently wrong output.

## B — `deleteLater` inside nested event loops   [A]
Covers V22, V23, and part of the re-entrancy family. H3's unifying finding.

A `DeferredDelete` posted inside a nested loop is collected **by that loop**, not
deferred past it. MERGEN has seven nested loops; `m_opening` guards one function
against one re-entry.

**Fix:** structural, not three point patches. Either stop deleting through
nested loops (own the objects explicitly and delete at a known-safe point), or
mark re-entrant regions and defer collection past them. Whichever is chosen, the
password prompt and the `pkexec` loop are the two that matter.
**Test:** H3's two chains — a peer hanging up during a nested loop, and Ctrl+I
while a portal-triggered polkit prompt is up.

## C — redaction models text layout wrongly   [R]
Covers V19, V20, V30, and H1's TJ/origin findings. This is why redaction
fails open.

Three separate errors compound:
1. `inside()` tests only a show operator's **origin**, never its painted extent,
   so a line starting outside and running in survives whole.
2. The text matrix never advances across glyphs — X3 measured 400pt of text
   outside the bar being deleted.
3. `'` and `"` are dropped whole, discarding the **line advance** that following
   lines depend on; the next line slides up under the black bar and stays
   extractable [R].
4. The rectangle is computed in poppler display space (CropBox-relative) and
   consumed as MediaBox-relative user space — on any cropped page the wrong
   region is destroyed and the secret survives [R].

**Fix:** this is not patchable in place. The filter needs to model the text
object properly: track the full text state (`Tf` size, `Tc`, `Tw`, `Tz`, `Ts`,
`TL`), advance the matrix per glyph using font widths, test the painted **extent**
against the area, and decompose `'`/`"` into their advance plus their show so
only the show is suppressed. Convert display space to user space explicitly,
adding the CropBox origin.
**Risk:** the largest single piece of work here, and the most dangerous to get
half-right. Consider whether shipping redaction at all is wise versus removing
it until it can be done properly — a feature that fails open is worse than an
absent one.

## D — redaction's verification proves the wrong thing   [R]
Covers V7, V13, V29. Independent of C, and cheaper.

`stillThere()` searches page text for the words the reader selected. So it is
vacuous when every token is under 3 characters [R]; it never sees metadata,
where the secret survives in `/Info`, outline titles and annotation `/Contents`
[R]; and it cannot see anything else the bar now covers.

**Fix:** verification must be incapable of passing vacuously — if nothing
checkable was extracted, that is a refusal. Search metadata and annotations too,
not only page text. Compare the *rendered* region before and after as a
geometric check rather than relying on text matching alone.
**Test:** redact a two-character token; redact a name that also sits in
`/Info /Title`; redact on a real-world PDF (V29: it currently declines on the
valgrind manual, the speex manual, and the project's own `test.pdf`).

## E — privileged bytes are never wiped   [R for the asymmetry, A for the measurements]
Covers V10, V26, V27, V31.

The password is wiped three times with a comment citing MX.md §5; the document
it protects is only `.clear()`ed, which frees without overwriting. Three resident
copies (Document, SearchWorker, poppler), all surviving `close()`. Recovered
from a coredump three times, and the core is user-readable — so one `auth_admin`
becomes a permanent unauthenticated copy on disk. A SHA-256 **of the privileged
content** is written to `portals.toml`.

**Fix:** overwrite before releasing, in every holder. Disable core dumps
(`PR_SET_DUMPABLE`) while privileged bytes are resident. Do not hash privileged
content into a state file — hash the path, or refuse portals on elevated
documents. Audit the other resting places X3 named: `recent.toml`, the
clipboard, print-to-file (which has no `data()` guard at all).

## F — path identity compared as a string   [R]
Covers V6. Redaction destroyed a source file through a symlink.

`absoluteFilePath()` normalises `..` but does not resolve symlinks;
`canonicalFilePath()` does — and neither catches a hard link.
**Fix:** compare device + inode via `stat`. Cheap, and it closes the whole class.

## G — no bound on what is read from outside   [R]
Covers V21 and S1's truncation finding.

`mergen-open` checks `st.st_size` once at fstat time, then copies to EOF with no
running total — 720,901 bytes emitted from a 5-byte file [R]. H2 measured 500 MB
in producing 1059 MB RSS.
**Fix:** enforce the cap inside the copy loop, and bound the socket read.

## H — state not reset when the document changes   [A]
Covers V17, V25, V34, V36.

`setDocument()` never clears `m_diffBands`; no open path calls `leaveCompare()`;
`stopScrollAnimations()` is called only from `applyScale`, so a stale animation
drags the new document; presentation silently degrades to FitWidth.
**Fix:** one reset path that every document change goes through. The four
symptoms are four things it forgot to do.

## I — main-thread work with no bound   [R for the socket, A for computeDiff]
Covers V14, V18.

One idle socket client freezes the GUI for ~0.7-1.0s [R], linearly per client.
`computeDiff()` blocks 9.1s on a 1000-page pair with no progress or cancel.
**Fix:** socket reads must not block the GUI thread — read asynchronously.
`computeDiff` moves to a `QThreadPool`, which is exactly what MZ.md §9's
"measure, then thread it" rule prescribes now that it has been measured.

## J — diff bands ignore rotation   [A]
Covers V33. `computeDiff` measures unrotated; `paintDiffBands` paints
rotation-oblivious. `paintSearchHits` does it correctly and is **the pattern to
mirror**. Also: the two compare panes hold independent rotation state.

## K — the audit scripts do not audit   [R]
Covers V4, V5, V12.

`audit-colours.sh` catches 5 of 17 planted violations and misses the ordinary
declaration form `QColor x(255,0,0)`. `audit-deps.sh`'s unanchored `libm` matches
`libmagic`/`libmount`. Both pass **vacuously outside a git repo** — exit 0 after
printing a fatal error. `audit-attribution.sh` never checks the committer field
or author email, and excludes `build/docs/*` wholesale.
**Fix:** anchor every alternative; `set -e` plus an explicit `git rev-parse`
assertion; check committer and email; widen the colour patterns and enforce the
`Qt::black` exception properly. Then re-run F1's planted-violation battery as a
regression test — a gate that has never been tested against a violation is not
a gate.

## L — packaging tells the truth   [R]
Covers V2, V3.

`CMakeLists.txt` still says `VERSION 1.0.0`, so a v2.0.0 package ships a binary
reporting 1.0.0. The PKGBUILD justifies `SKIP` checksums with "integrity comes
from the signed tag" — and the tag is lightweight and unsigned.
**Fix:** bump the project version and derive one from the other so they cannot
drift; either sign the tag and add `validpgpkeys`, or delete the claim.

## Q — the print path has no guards at all   [A]
Covers V46, V47, V48.

A 1000-page print freezes the app for **346 s** with zero repaints, no progress
and no cancel. "Current Page" and "Selection" print the whole document because
`printRange()` is never read. `from` is never bounds-checked (only `to` is), so
an out-of-range request emits one blank sheet silently. And print-to-file will
happily **overwrite the document you are reading** — the only combo entry on a
printer-less machine — while MZ.md §8 states that exact invariant for redaction.

**Fix:** honour `printRange()`; bounds-check both ends; refuse a destination that
is the open document (root cause F's device+inode check applies here too); move
the render loop off the GUI thread with progress and cancel. X2 notes the search
worker already does all of this correctly and is the model to copy.

## T — document-controlled geometry is never validated   [A]
Covers V50, V51, and part of V44.

A `/MediaBox` from the file reaches `qRound()` at `pageview.cpp:227` unchecked,
with signed overflows downstream at `:231` and `:1087`. Debug aborts on
`Q_ASSERT`; the **shipped Release build compiles that assert out** and silently
converts out of range. `flattenOutline` recurses with no depth bound and no
visited-set (cycles are caught by poppler, not by MERGEN).

**Fix:** validate page geometry at the boundary — clamp or reject implausible
MediaBoxes when the document is opened, rather than defending at every use site.
Bound the outline recursion depth.

**Note for whoever verifies this:** GCC's default `-fsanitize=undefined` does NOT
include `float-cast-overflow`, so a standard UBSan build cannot see the primary
conversion. My own sanitizer pass over all ten suites had that blind spot.

## R — a large MediaBox allocates gigabytes   [A]
Covers V44. `kMinZoom = 0.10` (`pageview.h:123`) clamps fit-width, so a 200,000pt
page renders at 289x the intended area — X2 measured a **793-byte file taking
2,081 MB on open**, before any user action.
**Fix:** let fit modes go below `kMinZoom`, or cap the rendered pixel count
rather than the zoom factor.

## S — properties() is a whole-document scan on the GUI thread   [A]
Covers V45. `fonts()` scans everything; **260 s** on an 8.8 MB file, **90.8 s
again** on repeat because nothing is cached. Driven by font count, not page
count. Triggered by a single keystroke.
**Fix:** cache the result per document; compute off the GUI thread; or drop the
font row.

## N — the state-file write path is unguarded   [R]
Covers V37, V40, and part of V41.

`QSaveFile::commit()`'s return value is never checked, so a failed save reports
success ("Portal made") across read-only directories, dangling symlinks and
genuine ENOSPC [A]. Worse, **no signal is handled anywhere in the source**: under
a file-size limit, `openPath` → `saveRecent` terminates the process with SIGXFSZ,
exit 153, core dumped [R] — and `recent.toml` is written on every successful
open, so this sits on the most common path in the application. The core dump is
itself V26's disclosure vector.

**Fix:** check `commit()` and report honestly; handle SIGXFSZ and SIGPIPE;
`fsync` the containing directory after the link (S2 showed the publish is not
crash-durable without it).

## O — the portal store is fragile and unbounded   [A]
Covers V38, V39, V41.

The `ends` counter increments only on the literal key `page`, so a reordered or
missing line silently destroys **both ends** of a portal, permanently on the next
save. `followPortal` locates the far end only by stored path, never re-resolving
by the hash sitting right beside it — so MZ.md §8's "survives a rename" holds
one way and fails the other. No cap on count or field length.

**Fix:** parse a portal as a whole record rather than counting keys; resolve the
far end by hash with the path as a hint; cap the store the way `recent.toml` is
capped.

## P — MZ.md §8 describes a mechanism the code does not use   [A]
"Temp file, fsync, rename" is wrong: `QSaveFile` uses `O_TMPFILE` → `fdatasync`
→ `linkat`. No named temp file, no `rename()`. Arguably a stronger idiom, but the
document should say what the code does. Correct the sentence.

## M — smaller, independent   [mixed]
V1 (the `QByteArray` temporary dangling into `QPDFWriter` — a named local fixes
it), V15 (invoke `/usr/bin/pkexec` by absolute path), V16 (prune `m_words` and
`m_links` in `dropPagesOutside`), V28 (the socket answers `ok` to opens that
never happened), V35 (selection line breaks at rotation), V11 (`readElevated`
accepting a truncated read), plus the clazy hygiene items.

---

## Do not touch — proven correct
`rotateRect`/`unrotateRect` (290,400 cases, 0 mismatches). `paintSearchHits`
(pixel-exact, all rotations). `linkAt` (27/27). The Esc chain (every
combination, three deep). Keyboard routing (no gaps, nothing swallows typing).
The render cache (memory does not climb with page count — three measurement
methods). `deeds` in `showCommands`. `m_focusBefore` and its `!now` guard.
`mergen-open`'s descriptor validation. `document.cpp:172`'s `Goto`-only filter.
`addContentTokenFilter` as the API choice. Split content streams. Encrypted and
linearized handling. X3 lists 16 load-bearing defences in full.

## Sequencing
A first — one line, biggest crash, unblocks confident testing of everything else.
Then A2 — comparable cost, and it stops the application producing confidently
wrong output (black pages, "identical" documents).
Then F, G, L, M, P (small, independent, low risk). Then N — it is on the most
common path and it crashes. Then E, D, R, S. Then H, I, J, K, O, Q.
C last and separately, with a decision first about whether redaction ships at
all in this state.
