# Confirmed findings — independently reproduced

Only findings I have personally reproduced go in this file. Agent reports are
in the sibling files; this is the subset that survived verification.

---

## V1 — heap-use-after-free in redaction  [HIGH]
`src/redact.cpp:224`. Found by my own ASan pass, before the agents reported.

    QPDFWriter writer(pdf, destination.toLocal8Bit().constData());
    writer.write();   // :227 — qpdf dereferences the freed pointer

`toLocal8Bit()` yields a temporary QByteArray destroyed at the `;`; QPDFWriter
stores the pointer and uses it in `write()`. Line 202 is the same idiom but
safe, because `processFile()` consumes it synchronously.
Fix: name the QByteArray in a local that outlives the writer.

## V2 — the shipped v2.0.0 binary reports version 1.0.0  [HIGH]
Reported by F1, reproduced here.

    CMakeLists.txt:8   VERSION 1.0.0
    packaging/PKGBUILD pkgver=2.0.0
    strings -e l build/mergen  ->  1.0.0

`MERGEN_VERSION` comes from the CMake project version, which was never bumped
during Z1–Z10 — only the PKGBUILD was. The About dialog and any version string
in a 2.0.0 package would say 1.0.0.
Fix: bump `project(... VERSION ...)`; ideally derive one from the other so they
cannot drift again.

## V3 — PKGBUILD claims integrity from a signed tag that is not signed  [HIGH]
Reported by F1, reproduced here.

    packaging/PKGBUILD:18  "Integrity comes from the signed tag"
    git cat-file -t v2.0.0 ->  commit     (lightweight; a signed tag is 'tag')
    validpgpkeys / ?signed occurrences: 0

`sha256sums=('SKIP')` is justified in the comment by a signature that does not
exist, so the source has no integrity check beyond GitHub's TLS. MZ.md §10's own
recipe (`git tag v2.0.0`) cannot produce a signed tag.
Fix: either sign the tag (`git tag -s`) and add `?signed` + `validpgpkeys`, or
correct the comment to stop claiming a guarantee that is not there.

## V4 — audit-deps.sh allowlist is unanchored  [MEDIUM]
`scripts/audit-deps.sh:11`. Reported by F1, reproduced here.

The bare token `libm` in the alternation matches any substring:

    libmagic.so.1  -> allowed
    libmount.so.1  -> allowed

So the dependency audit would wave through genuinely new libraries whose names
happen to contain "libm". Same class of error for any short token.
Fix: anchor each alternative, e.g. `^libm\.so`.

## V5 — audit scripts pass vacuously outside a git repository  [MEDIUM]
`scripts/audit-attribution.sh`, `audit-colours.sh`. Reported by F1, reproduced:
run from a non-repo directory, they print `fatal: not a git repository`, then
"clean", and **exit 0**.

An environmental failure is therefore indistinguishable from a clean audit — the
worst property a gate can have. In CI this survives only because a *different*
step sets `safe.directory`.
Fix: `set -e`, guard the `cd`, and assert `git rev-parse --git-dir` up front.

---

## Still to verify
F1 also reports that audit-colours.sh misses 12 of 17 planted colour violations
(notably the ordinary declaration form `QColor x(255,0,0)`, because the regex
requires `(` immediately after `QColor`), that audit-attribution.sh never checks
the committer field or author email and excludes `build/docs/*` wholesale, and
that the audits do not gate the release (separate workflows, no `needs:`).
Those are consistent with the code as written and I expect them to hold, but
they are recorded as reported-not-yet-reproduced until I check each.

---

## V6 — a symlink defeats the "never writes over the source" guard  [CRITICAL, data loss]
`src/redact.cpp:195`. Reported by H1 (as F18), reproduced here.

    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath())

`absoluteFilePath()` normalises `..` and makes a path absolute. It does **not**
resolve symlinks; `canonicalFilePath()` does. Reproduced:

    original.pdf              (the real file)
    alias.pdf -> original.pdf (a symlink, chosen as the destination)

    absolute  source: .../v/original.pdf
    absolute  dest  : .../v/alias.pdf      <- differ, guard passes
    canonical dest  : .../v/original.pdf   <- identical, guard should have caught it

Result: `Redact::run` returned `ok=1`, and the SOURCE was rewritten in place —
`original.pdf` went 1471 -> 1546 bytes and lost the redacted line. H1 reports a
worse outcome on a larger file (10 pages, 100 KB, reduced to 2,837 bytes with
every page blank) while the reader was told only "Nothing removable was found in
that area." Severity varies with file size and structure; the root cause is the
same.

Breaks `redact.h`'s own stated contract, MZ.md §5 ("Editing a PDF in place" is
banned) and §9. A hard link would defeat it too, and `canonicalFilePath()` will
not catch that — compare device+inode via `QFileInfo::...` or `stat` for a
complete fix.

## V7 — verification skips any token shorter than 3 characters  [CRITICAL, fails open]
`src/redact.cpp:184`. Reported by H1 (as F1), confirmed by reading:

    if (word.size() >= 3 && text.contains(word)) {

The whole verification is a loop over the selected words; a selection made
entirely of short tokens matches nothing, so `stillThere()` returns false and
the result is reported as verified-clean without a single check having run.

Short secrets are the *common* redaction case: an age, a ward number, a room, a
code, initials. Combined with the origin-only geometry test (H1's F1/F14, which
I have not yet reproduced myself), H1 demonstrates `ok=TRUE` with the selected
text fully intact and `pdftotext` still printing it.

Fix: verification must not be able to pass vacuously. If nothing checkable was
extracted, that is a refusal, not a success.

## V8 — cover rectangle coordinates are locale-dependent  [HIGH]
`src/redact.cpp:219-221`. Reported by H1 (as F4), confirmed by reading:

    "q 0 0 0 rg " + std::to_string(area.x()) + " " + ...

`std::to_string` for floating point honours `LC_NUMERIC`, and Qt calls
`setlocale(LC_ALL, "")` at startup. Under a comma-decimal locale (de_DE, fr_FR,
tr_TR — note the maintainer's own locale) this emits `72,000000` into the
content stream, which is not a valid PDF number. The black mark then fails to
draw, contradicting MZ.md §9.

Could not reproduce here: no comma-decimal locale is generated on this machine
(`locale -a` has none). The mechanism is not in doubt.

Fix: format with `QByteArray::number` / `std::to_chars`, which are
locale-independent, and never use `std::to_string` for content-stream numbers.

---

**Verified from H1 so far: 3 of its 20 findings.** The remaining 17 — including
the CropBox/user-space mismatch (F8), the dropped `'`/`"` line-advance that
slides the next line under the black box (F10), and metadata retention (F19) —
are recorded in `H1-redact.md` as reported-not-yet-reproduced by me.

## V9 — SIGSEGV: a socket command during the password dialog  [CRITICAL, crash]
Found independently by S1 (as Finding 0) and H2 (as F-6). **Reproduced by me
from scratch**, exit 139, core dumped.

    mergen                                    # no document
    open <password-protected.pdf>             # over the control socket
      -> MainWindow::promptForPassword() runs QInputDialog::exec()
      -> that nested event loop still services the listening socket
    goto 1                                    # second command, ~1.5s later
      -> Control::onConnection() REENTERS while the first call is on the stack
      -> runCommand touches m_doc->pageCount()

`Document::isOpen()` is `m_doc != nullptr`, which is **true for a locked
document** — `adopt()` (src/document.cpp:69) deliberately keeps the handle so
`unlock()` can retry without re-reading. So `pageCount()` reaches
`Catalog::getNumPages()` on a document poppler has not decrypted, and null-derefs.

The search worker guards this exact state one file away; `runCommand` does not.

Note the interaction: `m_opening` guards `openPath` and only `openPath`. What
protects the *UI* routes during the password loop is dialog modality — and a
socket message is not a UI event, which is precisely why it gets through.

## V10 — privileged plaintext reaches disk via the coredump  [CRITICAL, data leak]
Reported by H2 (as F-11), chain executed end to end by that agent; **not yet
re-reproduced by me.**

An unreadable file is read through `pkexec`, its bytes live in
`Document::m_data`, the password prompt then triggers V9's crash, and
`systemd-coredump` writes the process image — including that plaintext — to
`/var/lib/systemd/coredump/`. H2 planted a marker and recovered it from the core
twice.

Directly contradicts MZ.md §8 ("Nothing else is persisted"). The elevation
helper exists so the reader does not have to run the whole viewer as root; a
coredump containing the file's plaintext undoes much of that.

Fix: fixing V9 removes the trigger, but the exposure is broader — consider
`PR_SET_DUMPABLE` off while privileged bytes are held, and wiping `m_data`
promptly.

## V11 — readElevated accepts a truncated privileged read as success  [HIGH]
Reported by H2 (as F-1); **not yet re-reproduced by me.**

`loop.exec()` can return without `finished()` — any `quit`, Ctrl+Q or window
close exits *all* nested loops — and `QProcess::exitCode()` returns 0 /
`NormalExit` for a process still running. So the success guard does not fire and
a partial byte stream is handed to `openData()` as a completed privileged read.
H2 confirmed against the real binary: quit at 1.5s of a 6s transfer.


## V12 — audit-colours.sh catches 5 of 17 planted violations  [HIGH]
`scripts/audit-colours.sh`. Reported by F1, reproduced exactly — 5 caught, 12
missed, matching F1's count precisely.

Caught: `QColor(255,0,0)` and `QColor(0xFF0000)` in *temporary* form, `"#ff0000"`,
bare `Qt::red`, `QColor(Qt::red)`.

Missed, and the first one is the point:

    QColor a(255, 0, 0);        <- the ORDINARY DECLARATION FORM
    QColor b(0xFF0000);
    QColor::fromRgb(0xFF0000)   setRgb(255,0,0)   qRgb(255,0,0)
    QColor{255,0,0}   QColor("red")   "#f00"   fromHsv(...)
    int r=255,g=0,b=0; QColor m(r,g,b);
    Qt::darkRed                 <- the whole dark* family bar darkGray
    Qt::black                   <- never checked anywhere

The regex is `QColor\s*\(`, which requires the paren immediately after the type
name. `QColor a(255,0,0)` has an identifier in between, so the most natural way
to hardcode a colour is exactly the way the gate cannot see.

`Qt::black` is legitimately used once (src/pageview.cpp:311, the presentation
surround, argued in MZ.md §6) but the script's comment claims that is the only
permitted use and enforces nothing.

## V13 — the redacted secret survives in document metadata  [HIGH]
Reported by H1 (as F19), reproduced here.

A page whose secret also appears in `/Info /Title`, `/Author` and `/Keywords`:
after a successful redaction (`ok=1`), the page text is gone but `pdfinfo` on the
output still prints

    Title:    Report for Wardnumber
    Author:   Wardnumber
    Keywords: Wardnumber confidential

`stillThere()` reads only `Poppler::Page::text()`, so metadata is invisible to
the verification. Any reader's document-properties dialog shows the name the
user believed they had removed. Outline titles and annotation `/Contents` are
the same class.

## V14 — one idle socket client freezes the GUI thread  [HIGH]
Reported by S1 (as Finding 1), reproduced with measurements.

`Control::onConnection` runs on the main thread and blocks in
`waitForReadyRead(1000)` per connection:

    0 idle clients -> legitimate command answered in 0.001s
    1 idle client  -> legitimate command answered in 0.692s

S1 measured the full curve and found it linear at roughly 1s per silent client
(1 -> 0.956s, 3 -> 2.968s, 5 -> 4.977s), and isolated the cause by showing that
five *active* clients cost 0.001s total. So it is silence that costs, not
concurrency.

Any local process can therefore hold the UI unresponsive indefinitely by
connecting and saying nothing.


## V15 — pkexec is resolved through $PATH; a shim silently substitutes content  [HIGH]
`src/mainwindow.cpp:678`. Reported by H2, reproduced here.

    pkexec.setProgram(QStringLiteral("pkexec"));   // bare name -> $PATH lookup

Reproduced without root, using a mode-000 file to force the elevation path and a
fake `pkexec` earlier on `$PATH`:

- MERGEN invoked the shim instead of the real pkexec — **no polkit prompt at all**
- it accepted the shim's stdout as the privileged read
- it displayed that content while the window title still read `secret.pdf`

So the substitution is complete and invisible: the reader is shown one document
and told it is another. The requested file was genuinely unreadable throughout,
which is what proves the content came from the shim.

An attacker who can prepend to `$PATH` can already do a great deal, so this is
defence-in-depth rather than a boundary. But it is cheap to close and the
current arrangement makes the privileged path exactly as trustworthy as `$PATH`.

Fix: invoke `/usr/bin/pkexec` by absolute path.

## V16 — per-page caches added in v2.0 are never pruned  [HIGH]
Reported by S3 with measurements; recorded as reported (not re-measured by me).

`dropPagesOutside()` bounds `m_cache` only. `m_words` and `m_links` are cleared
on document close or rotation and never otherwise. Measured: **+73.6 MB after
touching all 1000 pages** of a synthetic document (~75 KB/page, ~1.1M word
entries).

S3 corrected the brief's framing here, which is worth noting: the trigger is a
single left click on a page, not passive scrolling — `paintSelection()`
early-returns when there is no active selection, so scrolling alone does not
populate `m_words`.

## V17 — stale diff bands survive onto an unrelated document  [HIGH]
Reported by S3, live-reproduced by that agent; recorded as reported.

`PageView::setDocument()` never clears `m_diffBands`, and none of the four ways
to open a new primary document (Ctrl+O, drag-drop, the control socket, the
file-watcher reload) calls `leaveCompare()` first. Open a different file while
comparing and the old comparison's marks persist over the new, unrelated
document — while the second `Document` and `PageView` stay orphaned.

## V18 — computeDiff blocks the UI for 9.1s on a 1000-page pair  [HIGH]
Reported by S3 with measurement; recorded as reported.

Synchronous, main thread, no progress, no cancel. Memory stays flat, so this is
purely responsiveness. It is exactly the situation MZ.md §9's own rule covers:
move it off the main thread *after measuring* — and it has now been measured.

---

## Confirmed SOUND — things that hold up

Worth recording so they are not "fixed" into breakage later.

- **The render cache genuinely bounds memory.** S3 scrolled all 1000 pages
  forward and back with RSS held in a 65.8–83.7 MB band, cross-validated three
  ways (raw RSS, malloc_trim-anchored RSS, and a massif profile whose peak is
  dominated by `cachedPage()` → poppler's `SplashBitmap`, dropping to near zero
  on close). v1.0's "memory does not climb with page count" still holds in v2.0.
- **Cache invalidation per axis is correct**, including `m_links` following the
  same rotation rule as `m_words`.
- **Night mode does not double-buffer**; repeated open/close is byte-identical
  across five rounds.
- **`rotateRect`/`unrotateRect` are exact algebraic inverses** (S4) — load-bearing,
  since redaction computes its target region through `unrotateRect`.
- **The elevation helper's own validation is sound** (H2): it validates the
  descriptor and never the name, `open()` is called exactly once, and non-regular
  files, directories, devices, FIFOs, relative and empty paths are all refused.
  15 protections confirmed to hold.
- **H1 refuted four suspicions**: matrix composition, operand arithmetic,
  multi-stream coalescing, and encrypted/linearized/objstm handling are correct.

## V19 — dropping `'`/`"` discards their line advance; the next line slides under the box  [CRITICAL]
`src/redact.cpp` (the `Cut` filter's handling of `'` and `"`). Reported by H1
(as F10), reproduced here and confirmed with mutool, an engine independent of
the poppler that MERGEN itself verifies with.

`'` means *advance to the next line and show text*. The advance is a side effect
of the operator. Dropping the operator to remove its text also drops the advance
that every following line depends on.

Test document (three lines, `'` used for line breaks):

    line positions      before      after
    heading             y=131.8     y=131.8
    SECRET account …    y=151.6     removed
    Innocuous line      y=171.8     y=151.8   <- moved UP into the vacated slot

MERGEN's own cover rectangle is emitted at `60 672 470 22 re f`, i.e. exactly
that slot. So the surviving innocuous line is now drawn **underneath the black
rectangle**.

Two harms, and the second is the serious one:

1. Visible content is silently lost — the reader can no longer see a line that
   was never meant to be removed.
2. The output now shows a black box with fully extractable text beneath it. A
   reader seeing that box concludes the text under it was removed. It was not;
   it is a *different* line, and any tool prints it.

That is precisely the false guarantee MZ.md §3 cites as the reason `qpdf` was
added as a dependency — reproduced by the routine added to prevent it.

Fix: `'` and `"` must be decomposed rather than dropped. Emit the line advance
(`T*`, or the equivalent `TL`-based translation) and suppress only the show.
The same care applies anywhere an operator carries a state change alongside its
visible effect.

## V20 — CropBox offset: the wrong region is redacted  [CRITICAL]
`src/mainwindow.cpp` (`redactSelection`'s coordinate computation), consumed by
`src/redact.cpp`. Reported by H1 (as F8). **Premise measured here; the
consequence is arithmetic from those measurements — I did not drive the full UI
selection path myself.**

Measured on a page with `/MediaBox [0 0 612 792]` and `/CropBox [0 300 612 792]`:

    Document::pageSize(0)  = 612.0 x 492.0     <- the CROPBOX, not the MediaBox
    word 'SECRET' box.y    = 81.9              <- poppler display space, CropBox-relative

`redactSelection` computes `area.y = pageSize.height - box.y - box.height`, i.e.
`492 - 81.9 - 14 = 396`, and `redact.cpp` consumes that as **MediaBox-relative
PDF user space**. The line's true user-space position is y=700 (`72 700 Td` in
the content stream). The correct value would be `300 + 396 = 696` — the whole
CropBox offset is missing.

The test document deliberately places "Unrelated text lower down" at user-space
y=400. So a reader redacting the secret at y=700 would instead have the line at
y=400 removed, the black mark painted there, and the secret left in place — with
`ok=TRUE` reported, because `stillThere()` would find the secret gone from...
no, it would find it present. Whether it fails open or merely destroys the wrong
content depends on whether the secret's own words are short (V7).

A CropBox differing from the MediaBox is completely ordinary — every trimmed or
imposed print PDF has one.

Fix: convert display space to user space explicitly, adding the CropBox origin,
rather than assuming `pageSize()` describes the same box qpdf works in.

## V21 — the helper's 2 GiB cap is never enforced during the copy  [MEDIUM]
`src/mergen-open.cpp:80` vs the loop at :111. Reported by H2, reproduced here.

The size check is one-shot, against `st.st_size` at fstat time:

    if (st.st_size > kMaxBytes) { ... }      // checked once
    ...
    for (;;) { n = read(fd, buf, 64K); writeAll(buf, n); }   // no running total

Reproduced by appending to the file while the helper read it:

    file at fstat time :          5 bytes
    helper emitted     :    720,901 bytes
    file by then       : 26,214,405 bytes

A file that grows during the read — a log, or anything an attacker controls —
is copied without bound into the viewer's memory. H2 separately measured 500 MB
of input producing **1059 MB RSS** before poppler rejected it.

Fix: keep a running total in the copy loop and abort past the cap.

## V22 — heap-use-after-free in the control socket  [CRITICAL]
`src/control.cpp:89`. Reported by H3 with a clean ASan trace; recorded as
reported (not re-reproduced by me).

`onConnection` calls the command handler *between* taking the client socket and
replying on it. `runCommand("open …")` can enter a nested loop (password prompt,
pkexec). If the peer hangs up during that loop, the socket's own `deleteLater`
is collected **inside** the nested loop, and `client->write(reply)` then writes
into freed heap.

MERGEN's own second-instance handover triggers this unaided, because
`Control::send` gives up after 1000 ms.

## V23 — SEGV in the overlay via deleteLater inside a nested loop  [CRITICAL]
`src/overlay.cpp:129`. Reported by H3, reproduced by that agent.

Ctrl+K → Portals → click a portal pointing at a root-owned file →
`readElevated`'s **deliberately non-modal** loop → press Ctrl+I → the portal
list is freed → Qt resumes the mouse release on the freed widget.

## V24 — process abort on window close during a search  [CRITICAL]
`src/mainwindow.cpp` destructor. Reported by H3, **confirmed on an
uninstrumented Release build** — Qt's own check, not a sanitizer artefact.

`m_searchThread` is a child of the window. When `wait(3000)` expires the
destructor continues and `deleteChildren()` destroys a running `QThread`, which
is `qFatal`. `SearchWorker::run` only checks its cancel flag *between* pages, so
a single heavy page offers no cancellation point.

## V25 — a stale scroll animation drags the NEW document  [HIGH]
`src/pageview.cpp`. Reported by H3, reproduced by that agent.

`stopScrollAnimations()` is called only from `applyScale`. Open a document while
a jump is in flight and the stale animation drags the *new* document to the old
offset (measured: 18985). MZ.md §9 claims a relayout cancels any jump in
flight — it does not, for this path.

---

## The unifying defect H3 found, which I had missed

Three of the four worst findings are **one bug**: `deleteLater` combined with
nested event loops. A `DeferredDelete` posted inside a nested loop is collected
*by that loop*, not deferred past it. MERGEN has seven nested loops (password
prompt, pkexec elevation, modal dialogs), and `m_opening` guards exactly one
function against one re-entry.

Patching V22 and V23 individually leaves the pattern in place. This wants a
structural answer, not three point fixes.

## H3 corrected three of my priors — do NOT "fix" these

- **`deeds` in `showCommands` is SOUND.** `run`'s `std::function` copy is
  sufficient, because `dismiss()` never destroys `content`.
- **`m_focusBefore` and the `focusChanged` connection are CORRECT.** The `!now`
  guard is load-bearing; removing it would break alt-tab behaviour.
- **The `bool *lock` in `enterCompare`**: the suspicion was right but the
  mechanism is not what I assumed. `QWidget` emits `destroyed()` *before*
  deleting children, so `lock` is freed while both scroll-lock lambdas are still
  connected. It is latent — nothing emits `valueChanged` in that window today.

## A meta-finding about my own testing

H3: "the shipped corpus is 1–3 pages throughout. No scroll range to animate, no
search long enough to cancel. Adding one long document and one heavy page would
have caught F4 and F5 without any of this machinery."

That is correct and it is the most useful process note of the audit. The
acceptance suites are only as good as their fixtures.

---

## NOT REPRODUCED BY ME

**H3's F4 (abort on window close during search).** I built a 1000-page fixture
and destroyed the window 120ms into a search on a Release build with no
sanitizers. It survived cleanly. Most likely my document is simple enough that
the worker finished inside the destructor's `wait(3000)`, so the race never
opened. H3 reports confirming it on Release. Recorded as **reported, not
reproduced here** — reproducing it needs a search still running after 3 seconds,
which wants many heavy pages rather than many simple ones.

---

## V26 — privileged plaintext is never wiped, and outlives the document  [CRITICAL]
Reported by X3 with a heap-scanning harness; recorded as reported. Strictly
worse than V10, which only covered the crash path.

X3 measured **three resident copies** of an elevated document's bytes, and all
three survive `Document::close()`, the SearchWorker's destruction, and the
Document's own destruction. `m_data.clear()` drops a reference; it never
overwrites a byte, and poppler's separate copy is never touched at all. X3 then
crashed a harness *after* `close()` and recovered a planted marker from the core
three times. The core's ACL is `user:megas:r--`.

So a single `auth_admin` authentication becomes a **permanent, unauthenticated
copy of a privileged file on disk**.

X3 names the asymmetry precisely, and it is the sharpest observation of the
audit: `promptForPassword` wipes the password three times, with a comment citing
MX.md §5. Fifty lines away, `readElevated` returns the entire document that
password protected, and nothing wipes it, ever.

## V27 — privileged content comes to rest in several more places  [HIGH]
Reported by X3. Beyond the coredump: `recent.toml` (absolute path of the
privileged file), `portals.toml` (a SHA-256 **of the privileged content**), the
clipboard, and print-to-file — which, unlike redaction, has no `data()` guard at
all.

## V28 — the socket can raise an auth prompt and then lie about the result  [HIGH]
Reported by X3, reproduced by that agent.

`open <unreadable-path>` over the control socket makes MERGEN raise an **admin
authentication prompt in response to a local process's command** (`ps` caught
the `pkexec`). The open path then wedges in a nested loop with no timeout, and
during that window `open` answers **`ok` to opens that never happen** — X3 tested
`/etc/passwd` and a directory.

## V29 — redaction declines on real-world documents  [HIGH]
Reported by X3. On the valgrind manual, the speex manual, **and the project's own
`test.pdf`**, redaction refuses rather than working.

X3's conclusion is the uncomfortable one and I think it is right: "the Z10
verification evidently never included a document from a real producer." My
acceptance test used hand-built PDFs whose structure I chose to be convenient.

## V30 — the text matrix never advances, so far more is deleted than the bar covers  [CRITICAL]
Reported by X3 (as R-4), reproduced on both poppler and MuPDF.

Because the filter never advances the text matrix across glyphs, a redaction
**silently deleted 400 points of text outside the black bar** — the render showed
a single ink run left on the line, the bar itself.

X3 reviewed redaction independently of H1 and reached the same verdict by its
own route: ten findings, twelve reproductions, **every leak confirmed on both
poppler and MuPDF**. Two independent reviewers agreeing, with different
reproductions, on different engines.

## Confirmed sound by X3 as well
`addContentTokenFilter` is the correct API; the rotation round-trip is exact
(byte-identical areas at 0/90/180); split content streams work; and searching
every output raw *and* inflated found **no residue anywhere**, because the whole
document is rewritten from the object model rather than incrementally updated.
X3 also lists **16 load-bearing defences** — chief among them `mergen-open.cpp`
validating the descriptor rather than the path, which it calls the best
engineering in the tree, and `document.cpp:172`'s `Goto`-only link filter, one
line enforcing three separate §5 bans.

## V31 — the wipe asymmetry, confirmed by reading  [CRITICAL, supports V26]
Reported by X3; confirmed here directly in the source.

The password is wiped three times, with a comment naming the rule:

    mainwindow.cpp:660   typed.fill(QLatin1Char('\0'));
    mainwindow.cpp:664   typed.fill(QLatin1Char('\0'));
    mainwindow.cpp:667   password.fill('\0');
                         // The password lives no longer than the attempt: MX.md §5.

The privileged document those credentials protect is only ever released:

    document.cpp:38    m_data.clear();
    document.cpp:111   m_data.clear();

`QByteArray::clear()` drops the reference and frees the buffer. It does not
overwrite the bytes. And there are **two declarations of `m_data`** —
`document.h:160` (Document) and `document.h:190` (SearchWorker) — so the copies
multiply, which corroborates X3's measured count of three resident copies once
poppler's own is included.

Also confirmed: `Document::contentHash()` (document.cpp:201-203) hashes
**`m_data` itself** when the document arrived through the elevation helper, and
that hash is then written to `portals.toml`. A digest of a privileged file's
contents comes to rest in an unprivileged state file.

The care taken over the password and the total absence of care over the document
it unlocks is, as X3 put it, the finding — not any single leak.

## V32 — the crash's root cause, precisely  [CRITICAL, supersedes V9's diagnosis]
Reported by S4, whose diagnosis is sharper than the earlier ones and gives the
fix. Now confirmed by **four independent parties**: S1, H2, S4 and me.

    Document::isOpen()  ->  m_doc != nullptr        (document.h:102)

That is **true for a locked document**, because `adopt()` deliberately keeps the
handle so `unlock()` can retry without re-reading. So `runCommand`'s `"goto"`
guard is a no-op, and the next line calls `pageCount()` into
`Catalog::getNumPages()` on a document poppler has not decrypted.

S4 scopes it precisely: `search` does **not** crash, because `SearchWorker` has
its own independent lock check on a separate thread. That isolates the bug to
direct main-thread poppler calls guarded only by `isOpen()`.

Fix: `isOpen()` should report false while the document is locked. One line, and
it closes every caller at once rather than patching `runCommand` alone.
Files: `mainwindow.cpp:736-797`, `document.h:102`, `document.cpp:115-117`.

## V33 — diff bands are painted rotation-oblivious  [HIGH]
Reported by S4 with pixel-exact proof at all four rotations.

`computeDiff()` always measures bands against an **unrotated** render;
`paintDiffBands()` then paints them with no rotation awareness at all. S4
compared the actual output against two hypotheses and it matches the
rotation-oblivious formula exactly: a wide horizontal stripe where a tall
vertical one belongs at 90/270°, and the wrong half of the page at 180°.

`paintSearchHits` does this **correctly** and is pixel-verified — it is the
pattern to mirror.

Addendum: the two compare panes hold independent rotation state, so rotating one
alone desynchronises the (already wrong) highlights between them.

## V34 — opening a document while comparing leaves compare dangling  [HIGH]
Reported by S4, corroborating S3's V17 by a second route. `openPath()` and
`closeDocument()` never call `leaveCompare()`. Reproduced two ways: opening an
unrelated third document, and a load error on the primary while comparing. Both
leave `isComparing()==true` with two unrelated documents on screen.

## V35 — selectedText() line breaks are wrong at every non-zero rotation  [MEDIUM]
Reported by S4: three distinct failure modes at 90/180/270° — over-splitting,
losing the real break, and reversed line order. Affects Ctrl+C.

Notably it does **not** affect redaction, which uses `.simplified()` and the
separately proven-correct `selectionBounds()`.

## V36 — opening a document while presenting drops the layout  [MEDIUM]
Reported by S4. `setDocument()` unconditionally resets the zoom mode to
FitWidth regardless of `m_presenting`, so presentation silently degrades to a
scrolling column while still fullscreen with the toolbar hidden.

---

## More confirmed SOUND — S4's positive results

These were *proven*, not merely read, and must not be "fixed" into breakage:

- **`rotateRect`/`unrotateRect` are exact inverses — 290,400 cases, 0 mismatches.**
  Load-bearing for redaction.
- **End-to-end selection is geometrically correct** at every rotation and zoom
  tried, driven with real mouse events.
- **`linkAt` hit-testing correct 27/27** across rotations and zooms, and the
  early `return nullptr` in its page loop is safe given the layout invariant.
- **`paintSearchHits` is pixel-exact at all four rotations.**
- **The Esc chain is correct under every combination**, including a three-deep
  stack, confirmed with real key events.
- **Keyboard routing has no gaps**: no window shortcut swallows typing, F/Shift+F
  reach the right handler per focus state, focus is never stranded.
- **Night mode is geometrically orthogonal** to everything else.

## A method note worth keeping
S4: the modal-race crash only reproduces with a **genuinely separate OS process**
as the racing client. A same-process client produces empty replies and no crash
through nested-event-loop entanglement — which looks like a false negative but
is not. Recorded so it is not rediscovered at cost.

## V32a — the crash reduced to three lines, no socket or dialog needed  [CRITICAL]
Reproduced by me directly, confirming and **generalising** S4's diagnosis:

    Document d;
    d.openPath("locked.pdf");   // -> LoadStatus::NeedsPassword
    d.isOpen()                  // -> TRUE
    d.pageCount()               // -> SIGSEGV, core dumped

No socket. No dialog. No race. The control socket was only the route by which
the earlier agents happened to reach it.

The defect is that the invariant **`isOpen()` means "usable"** is false: it
reports true for a document poppler has loaded but not decrypted. Every caller
that checks `isOpen()` and then touches poppler on the main thread is exposed —
`runCommand`'s `goto`/`search`/`quit` handlers, `redactSelection`,
`showProperties`, `showOutline`, `markPortal`, `followPortal`, `chooseComparison`
and `computeDiff` all begin with exactly that check.

This raises the severity above "a socket race". It also makes the fix a
one-liner that closes every caller simultaneously, rather than a guard added to
each: `isOpen()` must be false while the document is locked, with `unlock()`
flipping it true on success. `adopt()` keeps the handle deliberately (so
`unlock()` can retry without re-reading), so the locked state needs to be
tracked explicitly rather than inferred from the pointer.

## V37 — an uncaught SIGXFSZ kills the process   [HIGH]   [A]
Reported by S2, reproduced by that agent: with `ulimit -f 0` in force, saving a
portal terminates MERGEN outright — exit 153, core dumped. There is no signal
handling anywhere in the source.

Any filesystem-limit condition during a state write is therefore fatal rather
than degraded, and (per V26) a core dump is itself a disclosure risk.

## V38 — loadPortals' `ends` counter silently destroys portals   [HIGH]   [A]
`mainwindow.cpp:804-847`. Reported by S2 with nine end-to-end cases.

The counter increments only on the literal key `page`, so a reordered, missing
or duplicated `page` line silently corrupts or discards **both ends** of a
portal — and the damage becomes permanent on the next save. It does not cross a
`[[portal]]` boundary, which bounds the blast radius but does not prevent it.

This is exactly the ordering fragility flagged in the brief, now confirmed with
consequences worse than expected: not a failed load, but silent permanent loss.

## V39 — a far-end rename breaks followPortal, though the hash is right there   [HIGH]   [A]
`mainwindow.cpp:897-934`. Reported by S2, verified in both directions with real
renames.

MZ.md §8's "survives a rename" claim holds for the **near** end (the document you
press Ctrl+J from) and fails for the **far** end (the one you are trying to
reach), which is located only by its stored path and never re-resolved by its
own stored hash — even though that hash is present and matches.

## V40 — markPortal reports success when the save failed   [MEDIUM]   [A]
Reported by S2, reproduced across a read-only directory, a dangling symlink, a
blocking file, and genuine ENOSPC (via an unprivileged 4 KB tmpfs). The reader
is told "Portal made" either way.

## V41 — no cap on portals; recent.toml's bracket scan is comment-blind   [MEDIUM/LOW]   [A]
Unlike `recent.toml`'s deliberate ten-item cap, portals are unbounded: S2 loaded
1,000,000 (~382 MB, permanently resident, and every future save rewrites all of
them). Separately, `recent.toml`'s whole-file bracket scan ignores comments, so a
crafted `#` line can inject a spurious recent entry.

---

## MZ.md §8's three claims, adjudicated by S2

1. **"Temp file, fsync, rename"** — right in intent, **wrong in mechanism**.
   Traced with an LD_PRELOAD syscall shim on the real ext4 state directory:
   `QSaveFile` uses `open64(O_TMPFILE)` → `fdatasync` → `linkat()`. There is no
   named temp file and no `rename()` syscall. Content genuinely is flushed before
   publish — arguably a stronger idiom than the documented one, but not the one
   documented. Real gap: the containing **directory is never fsync'd** after the
   link, so the publish itself is not crash-durable on all filesystems.
   *Action: correct the sentence in MZ.md, and fsync the directory.*

2. **"Never an error the reader sees"** — confirmed for the **read** side across
   dozens of adversarial inputs (truncation, CRLF, NUL, invalid UTF-8, 500 MB
   files, 100M-character values, a million portal blocks — all silent, no crash,
   no hang). But the **write** side can crash (V37) or lie (V40), and a malformed
   file can cause silent permanent loss (V38). The claim is true as written and
   incomplete as a guarantee.

3. **"Survives a rename"** — confirmed near end, refuted far end (V39).

## More confirmed SOUND, from S2
`tomlEscape`/`tomlUnescape` round-trip correctly across **200,020 cases** plus a
real end-to-end test with adversarial filenames (embedded newline, quote,
backslash, tab). No injection or traversal concern — `pkexec` is invoked with an
argv array and no shell anywhere. Symlinked state directories work
transparently. `recent.toml`'s ten-item cap and de-duplication hold at any input
size. Negative and overflowing page numbers pass through inertly, bounds-checked
downstream.

### V37 — confirmed by me, and broader than reported   [R]
Reproduced: `ulimit -f 0`, then open a document. Exit **153** (128+25 =
SIGXFSZ), core dumped.

It died before reaching the portal code at all — the crash came from
`openPath` → `pushRecent` → `saveRecent`. So it is not portal-specific: **any**
state write under a file-size limit terminates MERGEN. `recent.toml` is written
on every successful open, so this is on the most common path in the application.

Compounding: the core dump is itself the disclosure vector described in V26. A
disk quota being hit while a privileged document is open writes that document's
plaintext to `/var/lib/systemd/coredump/`.

Fix: SIGXFSZ (and SIGPIPE) need handling, and `QSaveFile::commit()`'s return
value needs checking — which also fixes V40's false "Portal made".

## V43 — poppler returns a NON-NULL 1x1 image when it refuses an allocation   [CRITICAL]   [R]
Reported by X2; reproduced here, and it is worse than a print bug.

    scale  20 -> wanted 11900x16840  (0.80 GB)  got 11900x16840  isNull()=false
    scale  60 -> wanted 35700x50520  (7.21 GB)  got     1x1      isNull()=false
    scale 120 -> wanted 71400x101040 (28.9 GB)  got     1x1      isNull()=false

Poppler writes "Bogus memory allocation size" to stderr and hands back a 1x1
image that **is not null**. X2 pinned the threshold at exactly 2^31 bytes.

Every `isNull()` check in MERGEN therefore passes on a failed render. Four sites
share the blind spot: `mainwindow.cpp:1519` (print), `pageview.cpp:298` (the page
cache), `mainwindow.cpp:1104`, `mainwindow.cpp:1421`.

Consequences, all silent:
- **Print** scales that one pixel to fill the sheet. X2 measured printing one A0
  page producing **a sheet of solid black** — 158 ms, exit 0, no dialog.
- **The page cache** stores a 1x1 as though it were the page.
- **Compare** renders both documents, gets 1x1 for each, finds no difference, and
  **reports the two documents identical**.

Fix: never trust `isNull()` alone. Check the returned size against the size
requested, and treat a mismatch as failure.

## V44 — kMinZoom defeats fit-width; a 793-byte file allocates 2 GB   [CRITICAL]   [A]
`pageview.h:123`. Reported by X2.

A 200,000 pt page needs a fit-width factor of 0.00588; `kMinZoom = 0.10` clamps
it to 0.10, i.e. **289x the area**. X2 measured a **793-byte file allocating
2,081 MB resident / 2,585 MB peak on open**, before any user action.

A tiny file causing gigabytes of allocation is a decompression bomb by another
name, and it needs no malformed structure at all — just a large MediaBox.

## V45 — Ctrl+I freezes for minutes   [CRITICAL]   [A]
`Document::properties()`. Reported by X2 with measurements: **26.3 s** on a
608 KB *one-page* file, **260 s (4 min 20 s)** on an 8.8 MB file, and **90.8 s
again on every repeat** because nothing is cached.

Font count drives it, not page count — 2,000 plain pages cost 157 ms. The
`fonts()` call is a whole-document scan, run synchronously on the GUI thread from
a keystroke.

## V46 — a 1000-page print freezes the app for 346 s   [CRITICAL]   [A]
Reported by X2: 5 min 46 s with **zero** of ~1,385 expected heartbeat ticks — no
progress, no cancel, no repaint. 10 pages = 3.9 s, 100 = 30.8 s, so it is linear
and predictable. Output was 1.04 GB from a 346 KB source.

## V47 — print-to-file overwrites the document you are reading   [HIGH]   [A]
Reported by X2. On a printer-less machine "Print to File (PDF)" is the *only*
combo entry. X2 measured a 3,874-byte 10-page source becoming a 956,628-byte
1-page raster. Qt asks a generic "already exists, overwrite?" that never says
*this is the file you currently have open*.

MZ.md §8 states this exact invariant for redaction ("the destination is never the
source"). Printing does not enforce it.

## V48 — "Current Page" and "Selection" print the whole document   [HIGH]   [A]
Reported by X2: `printRange()` is never read, so both leave `fromPage()==0` and
fall into the "All pages" branch — 10 pages / 10.3 MB for a one-page request.
Separately, `from` is never bounds-checked (only `to` is), so pages 50-60 of a
10-page document emit **one blank sheet**, silently.

---

## More confirmed SOUND, from X2

- **MX.md §369's five print claims are all TRUE.** Printer resolution is used
  rather than the screen cache (4958x7017 px = exactly 595pt x 600/72); the cap
  bites (`QPrinter(HighResolution)` reports 1200 dpi here, `qMin` -> 600); aspect
  ratio is kept; rotation is followed. Even the doc's "550 MB per A4 at 1200 dpi"
  arithmetic checks out at 556.8 MB, cut 4x to 139.2 MB by the cap.
- **Print-loop memory is flat**: 235.4 MB at 10 pages, 235.4 at 100, 237.5 at
  1,000. No leak.
- **The search worker is exemplary** — 122,000 hits, worst event-loop gap **5 ms**,
  +12.6 MB, progress and Cancel correctly wired. X2's words: "it's the model the
  print path should have followed."
- **`contentHash` streams properly** — RSS flat at 41.5 MB while hashing 500 MB.

## Two corrections X2 made to my briefs
- **`linkAt` does NOT run on every mouse move.** `mouseMoveEvent` never calls it;
  only `mousePressEvent` does. Move cost is flat at 0.0020 ms from 400 to 20,000
  links. My brief to S1/S3 asserted otherwise and was wrong.
- X2 **withdrew** one of its own findings (an apparent overwrite-guard bypass)
  after establishing it was its harness's fault. Recorded because that discipline
  is what makes the rest of the report trustworthy.

## V49 — the locked-document deref is broader than I proved   [CRITICAL]   [A]
X1 tested each `Document` accessor in isolation: **every one except
`contentHash` null-derefs** on a locked document, because poppler leaves
`catalog == nullptr` until decryption. I had only demonstrated `pageCount()`.

X1 also quantified it: **295 of 295 mutants** that reached the encrypted state
crashed. This is not a corner case, and the trigger is a completely benign file —
`qpdf --encrypt --user-password=secret --bits=256`.

Reinforces root cause A. The one-line fix closes all of them.

## V50 — document-controlled geometry reaches qRound() unvalidated   [HIGH]   [A]
`pageview.cpp:227`, with signed overflows downstream at `:231` and `:1087`.

A `/MediaBox` from the file is fed straight into layout arithmetic. In a **Debug**
build Qt's `Q_ASSERT` aborts; the **shipped Release** build (`CMakeLists.txt:19`,
`PKGBUILD:25`) compiles that assert out and silently converts out of range — a
1e9-point page renders as 1x1.

**X1 caught a blind spot in my own instrumentation, which is worth recording:**
GCC's default `-fsanitize=undefined` does **not** include `float-cast-overflow`.
My ASan+UBSan pass over all ten acceptance suites could never have seen the
primary conversion. X1 verified that with a micro-test.

## V51 — flattenOutline recurses unbounded   [HIGH]   [A]
`document.cpp:144`. X1 **corrected its own first result**: not a stack overflow at
depth 10,000 (that was the ASan build's larger frames), but ~210-260 bytes/level
on Release, so an 8 MB stack overflows near 35-40k. X2 independently measured
224 B/level and bracketed the same range.

The practical symptom on a stock desktop is a **55-second frozen window at depth
20,000**, superlinear. Cycles are caught by *poppler*, not MERGEN — there is no
visited-set here.

## V52 — synchronous CPU exhaustion, quantified   [HIGH]   [A]
X1's numbers corroborate X2's independently: `properties()` **257 s** on a 9 MB
40k-font file; **203 s** at 100k pages; compare mode **8.6 minutes**; 300k pages
costs **821 MB RSS at open**.

X1's summary is the one to keep: "MERGEN already has the right pattern
(`SearchWorker`, with a cancel flag) and applied it nowhere else."

---

## G-01 — the network guarantee HOLDS   [A]   (a PASS worth stating)

X1 built an `LD_PRELOAD` interposer (self-tested against a real connect, since no
`strace` on this host) and ran a kitchen-sink document — JavaScript, network,
XXE, `/Launch`, embedded files — plus `evil.pdf`, against the real binary.

**Zero `AF_INET`/`AF_INET6` sockets, zero connects, zero `getaddrinfo`. No JS ran.
No payload executed.** MZ.md §5's "no network access of any kind" is upheld.

One honest caveat X1 raises: Arch's libpoppler links libcurl, so an HTTP stack is
resident in the process, merely unreachable from MERGEN's API surface. The
guarantee holds "by not-calling rather than not-having."

## The fuzzing campaign, in numbers
12,000 mutations (4,000 naive / 6,000 structure-aware / 2,000 codec-targeted),
489 faults, **2 distinct signatures — both rediscoveries of V49 and V50, zero new
bug classes**. X1 validated its crash detector against known-bad files before
trusting the counts.

All 32 hand-built image-codec documents (JBIG2, JPX, CCITT, DCT, LZW, hostile
predictors, forced corrupt-font rasterisation) passed clean — and X1 correctly
attributes that hardening to poppler, not to MERGEN.

**X1 corrected itself twice**: an early "1M pages is harmless" claim was wrong
(those files reported `pageCount = 0` because poppler rejects a reused page
object; rebuilt with distinct objects, the attack works), and a buggy coverage
measurement had made round 1 look like it tested nothing.

**Acknowledged gaps** — all mutation was blind, with no libFuzzer/AFL++ harness.
Untested: `SearchWorker`'s second poppler handle running concurrently with
main-thread rendering (X1 calls this the most plausible remaining spot for a
memory-safety bug rather than a DoS), `/ObjStm` documents, and interactive UI
event fuzzing.
