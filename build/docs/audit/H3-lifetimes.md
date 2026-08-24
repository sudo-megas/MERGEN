# H3 — Object lifetime, ownership and concurrency audit

**Scope:** the whole of `<repo>/src/`, with emphasis on
`mainwindow.cpp`, `pageview.cpp`, `overlay.cpp`, `document.cpp`, `control.cpp`.
Read-only audit; nothing outside this file was modified.

**Reference:** `build/docs/MZ.md` (constitution). Documented deliberate
tradeoffs are not reported as bugs; places where the code fails to do what
MZ.md claims *are*.

**Method:** full read of every translation unit, plus an
AddressSanitizer + UndefinedBehaviorSanitizer build at `/tmp/mergen-asan`
(`-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`, Debug),
driven by purpose-built harnesses in `/tmp/mgtest/` under
`QT_QPA_PLATFORM=offscreen`. `valgrind` is **not** installed on this machine
(checked); `gdb` is.

Findings are appended as they are confirmed. Each is marked
**REPRODUCED** (sanitizer output or crash in hand) or **BY READING**
(argued from the code, not yet triggered).

---

## F1 — CRITICAL — `Control::onConnection` writes to a `QLocalSocket` that a nested event loop already deleted — **REPRODUCED**

**Where:** `<repo>/src/control.cpp:89` (read), freed via the
`deleteLater` connected at `<repo>/src/control.cpp:73`.
Root cause spans `control.cpp:71–92` and
`<repo>/src/mainwindow.cpp:744` (`runCommand` → `openPath`).

**One sentence:** `Control::onConnection` calls the command handler *between*
taking the client socket and replying on it, and the handler can enter a nested
event loop (password prompt, `pkexec` wait) long enough for the peer to hang up
— which runs the socket's own `deleteLater` inside that nested loop, leaving
`client` dangling for `client->write()` / `flush()` / `disconnectFromServer()`.

```cpp
// control.cpp:71
void Control::onConnection() {
    while (QLocalSocket *client = m_server->nextPendingConnection()) {
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        ...
        const QString reply = m_handler ? m_handler(verb, argument) : ...;  // nested event loop
        client->write(reply.toUtf8() + '\n');   // <-- line 89, use-after-free
        client->flush();
        client->disconnectFromServer();
    }
}
```

**Why the deferred delete fires early.** Qt only withholds a `DeferredDelete`
while the loop level that posted it is still the current one. `disconnected` is
delivered from *inside* the modal loop, so the event is stamped one level deeper
than the loop that then dispatches it, and Qt considers the posting loop to have
returned. It is deleted before `runCommand` returns.

**Concrete trigger (exactly what the harness does):**
1. MERGEN is running and holds `$XDG_RUNTIME_DIR/mergen-$UID.sock`.
2. Any client sends `open <password-protected.pdf>` — e.g.
   `printf 'open /tmp/locked.pdf\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/mergen-$UID.sock`,
   or MERGEN's own `Control::send`, which is what a **second `mergen file.pdf`
   launch** uses (`main.cpp:32`).
3. `MainWindow::openPath` reaches `promptForPassword`, which calls
   `QInputDialog::getText` — a modal nested event loop.
4. The caller gives up and closes the socket. `Control::send` does exactly this
   on its own: `waitForReadyRead(kReplyMs = 1000 ms)` fails, `send` returns, the
   stack `QLocalSocket` is destroyed, the peer end closes. **No unusual client
   is needed — MERGEN's own second-instance handover reproduces it.**
5. The reader answers or cancels the password dialog. `runCommand` returns and
   `onConnection` writes into freed memory.

The same shape applies to the other nested loop on this path,
`MainWindow::readElevated` (`mainwindow.cpp:691–693`), which spins a
`QEventLoop` while `pkexec` waits on the polkit agent — a much longer window
than one second.

**Sanitizer output (abridged; full log `/tmp/mgtest/h3` run):**

```
==94233==ERROR: AddressSanitizer: heap-use-after-free on address 0x7bf6b6edf3b0
READ of size 8 at 0x7bf6b6edf3b0 thread T0
    #0 mergen::Control::onConnection() <repo>/src/control.cpp:89
    ...
0x7bf6b6edf3b0 is located 0 bytes inside of 16-byte region
freed by thread T0 here:
    #1 QObject::event(QEvent*)                          <-- DeferredDelete
    #4 QCoreApplicationPrivate::sendPostedEvents(...)   <-- inside the modal loop
previously allocated by thread T0 here:
    #1 (libQt6Network) QLocalServer::nextPendingConnection
SUMMARY: AddressSanitizer: heap-use-after-free <repo>/src/control.cpp:89
```

**Severity:** critical. It is remote-ish only in the sense that any local
process that can reach the socket triggers it, but the ordinary
second-instance path reaches it unaided, and the write lands in freed heap
with attacker-influenced length.

**Fix direction:** hold the client in a `QPointer<QLocalSocket>` across the
handler call and re-check it before writing; and/or do not connect
`disconnected → deleteLater` until after the reply has been written; and/or
answer synchronously and never call a handler that can block from inside
`onConnection` (queue the command and reply `ok` first). The minimal change is:

```cpp
QPointer<QLocalSocket> guard(client);
const QString reply = ...;
if (!guard) continue;            // the caller hung up while we were busy
```

**Reproduction harness:** `/tmp/mgtest/h3.cpp`, built with `/tmp/mgtest/build.sh h3`,
run as `XDG_RUNTIME_DIR=/tmp/mgtest QT_QPA_PLATFORM=offscreen /tmp/mgtest/h3`.
Encrypted fixture made with
`qpdf --encrypt --user-password=secret --owner-password=owner --bits=256 -- test.pdf /tmp/mgtest/locked.pdf`.

---

## Interim status — superseded

An interim verdict table stood here. It was written before the Qt ordering
question below had been settled experimentally and **two of its rows were
wrong** (suspects 1 and 2 were called "sound as written"; they are sound only by
a one-event margin, and for a different reason than stated). It has been removed
rather than left to be read. The authoritative verdicts are in
**"The eight named suspects — final verdicts"** at the end of this document, and
the findings F1–F11 are the evidence behind them.

---

## F2 — HIGH — a search pass started and abandoned in the same event-loop turn leaks its `SearchWorker` (and the document bytes it carries) — **REPRODUCED**

**Where:** `<repo>/src/mainwindow.cpp:1656` (`new SearchWorker`),
`1663` (the only thing that ever frees it), `1672` (the queued `run`), and
`<repo>/src/mainwindow.cpp:248–254` (`~MainWindow`).

**One sentence:** the worker is owned by nothing — its sole cleanup is
`connect(worker, &SearchWorker::done, worker, &QObject::deleteLater)`, so a
worker whose queued `run()` is never delivered never emits `done`, never
`deleteLater`s, and is leaked outright along with its `QByteArray` copy of the
document.

```cpp
// mainwindow.cpp:1656
auto *worker = new SearchWorker(m_doc->path(), m_doc->data(), needle);  // no parent
worker->moveToThread(m_searchThread);
...
connect(worker, &SearchWorker::done, worker, &QObject::deleteLater);   // 1663 — the only owner
QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection);        // 1672
```

`~MainWindow` calls `m_searchThread->quit()` immediately after `cancelSearch()`.
If `quit()` beats the worker's queued `run` metacall, the thread's event loop
returns without ever running the worker; `QThreadPrivate::finish` flushes
`DeferredDelete`, but no `DeferredDelete` was ever posted for this worker.

**Concrete trigger:** open a document, press <kbd>Ctrl</kbd>+<kbd>F</kbd>, type a
term, press <kbd>Enter</kbd>, and close the window in the same turn (fast
keyboard, a scripted `mergen`, or `search` + `quit` back-to-back over the
control socket). Reproduces intermittently — roughly 2 runs in 5 on this
machine.

**LeakSanitizer output (abridged):**

```
==100822==ERROR: LeakSanitizer: detected memory leaks
Indirect leak of 120 byte(s) in 1 object(s) allocated from:
    #2 mergen::SearchWorker::SearchWorker(...)  <repo>/src/document.cpp:431
    #3 mergen::MainWindow::startSearch()        <repo>/src/mainwindow.cpp:1656
Indirect leak of 152 byte(s) ...  mergen::MainWindow::startSearch()  mainwindow.cpp:1652  (the QThread)
Indirect leak of 160 byte(s) ...  mainwindow.cpp:1658  (hitFound connection)
Indirect leak of  88 byte(s) ...  mainwindow.cpp:1663  (done→deleteLater connection)
Indirect leak of  64 byte(s) ...  mainwindow.cpp:1672  (the queued run metacall)
```

For a document opened through the elevation helper, `m_doc->data()` is the
**entire file**, so the leak is the size of the PDF, not 120 bytes.

**Severity:** high. It is only a leak, but it is an unbounded one on a path a
scripted reader hits routinely, and it is a symptom of the real problem: the
worker has no owner.

**Fix direction:** give the worker an owner that does not depend on it running.
Either parent it to nothing but track it and `delete`/`deleteLater` it
explicitly in `cancelSearch()` and `~MainWindow`, or — simplest —
`connect(m_searchThread, &QThread::finished, worker, &QObject::deleteLater)` in
addition to the `done` connection, so the worker dies with the thread whether or
not it ever ran.

---

## F3 — HIGH — `m_searchWorker->cancel()` dereferences a `QPointer` that the worker thread may clear between the null check and the call — **BY READING**

**Where:** `<repo>/src/mainwindow.cpp:1675–1686` (`cancelSearch`),
against `<repo>/src/mainwindow.cpp:1663` and
`<repo>/src/document.h:178`.

```cpp
void MainWindow::cancelSearch() {
    if (!m_searchWorker) {          // GUI thread reads the QPointer
        return;
    }
    disconnect(m_searchWorker, nullptr, this, nullptr);
    m_searchWorker->cancel();       // <-- worker may already be deleted on the search thread
    m_searchWorker = nullptr;
    ...
}
```

`m_searchWorker` is a `QPointer<SearchWorker>` written on the GUI thread and
cleared by `~QObject` **on the search thread** (the `done → deleteLater`
connection at line 1663 is a direct, same-thread connection, so the worker is
destroyed by its own thread's event loop). A `QPointer` is not a synchronisation
primitive: it makes the null *check* atomic, it does not make check-then-use
atomic.

**Concrete trigger:** start a search on a document short enough that the pass
finishes on its own, and press <kbd>Esc</kbd> (or the Cancel button, or start a
second search, or close the window) in the window between the worker emitting
`done` on the search thread and the GUI thread's `cancelSearch` reaching
`->cancel()`. `cancel()` then writes `1` into `m_cancelled` inside a freed
`SearchWorker`.

The window is genuinely small (a few microseconds), which is why the acceptance
suites never hit it; it is not zero, and it widens on a loaded machine. I did
**not** reproduce it — reported by reading, with the mechanism spelled out
above.

Note the *other* half of this suspect is **sound**: the "current pass" guard
(`sender() != m_searchWorker` at `mainwindow.cpp:1693, 1700, 1711`) does what it
claims. Late queued `hitFound` events from a replaced worker are correctly
dropped, and the ABA hazard (a new worker landing on the freed address of an old
one) cannot bite, because `done` is emitted *after* every `hitFound` of that
pass and is delivered in order, so by the time a worker's address can be reused
its queued hits have already been consumed.

**Fix direction:** take a plain raw copy under a mutex, or better, do not let the
worker delete itself: have the GUI thread own it (see F2's fix) and cancel
through a `QAtomicInt` held by the *window*, not by the worker, so the flag
outlives the object.

---
## F4 — CRITICAL — closing the window during a search that cannot stop within 3 s aborts the process (`qFatal` in `~QThread`) — **REPRODUCED**

**Where:** `<repo>/src/mainwindow.cpp:248–254`.

```cpp
MainWindow::~MainWindow() {
    cancelSearch();
    if (m_searchThread) {
        m_searchThread->quit();
        m_searchThread->wait(3000);     // <-- may time out
    }
}                                        // ~QWidget::deleteChildren() then deletes the QThread
```

**One sentence:** `m_searchThread` is a **child of the window**
(`new QThread(this)`, `mainwindow.cpp:1652`), so when `wait(3000)` times out the
destructor carries on and `QObjectPrivate::deleteChildren()` destroys a still-
running `QThread`, which Qt 6 answers with `qFatal` — an unconditional
`abort()`, not a warning.

`SearchWorker::run()` only tests the cancel flag **between pages**
(`document.cpp:443–447`), so a document with one very heavy page has no
cancellation point at all until that page finishes. `Poppler::Document::load`
at `document.cpp:435` is likewise ahead of every check.

**Concrete trigger (exactly what the harness does):**
1. Open a document with one very dense page (mine: 900 000 show-text operators,
   `/tmp/mgtest/huge.pdf`, 4.3 MB — a dense vector plate or a big scanned
   composite is the real-world equivalent).
2. <kbd>Ctrl</kbd>+<kbd>F</kbd>, type a term, <kbd>Enter</kbd>.
3. <kbd>Esc</kbd> — `closeSearch()` → `cancelSearch()`; the flag is set but the
   worker is mid-page and cannot see it.
4. Close the window. `cancelSearch()` is now a no-op, `wait(3000)` expires, and
   the process aborts.

**Backtrace (gdb, ASan build):**

```
Thread 1 "h5" received signal SIGABRT, Aborted.
#4  QMessageLogger::fatal(char const*, ...)      (libQt6Core)
#6  QThread::~QThread()                          (libQt6Core)
#7  QObjectPrivate::deleteChildren()             (libQt6Core)
#8  QWidget::~QWidget()                          (libQt6Widgets)
#9  mergen::MainWindow::~MainWindow()            <repo>/src/mainwindow.cpp:254
#10 main
...
Thread 6 "QThread"  TextPage::addChar(...)       (libpoppler)   <-- still working
```

**Cross-checked without sanitizers.** Rebuilt the same harness against a plain
`-DCMAKE_BUILD_TYPE=Release` tree (`/tmp/mergen-plain`) and it aborts
identically (`rc=134`, core dumped). This is Qt's own runtime check, not a
sanitizer artefact.

**Severity:** critical — an unconditional process abort on the ordinary
"close the window" gesture, and the 3 s budget is a guess that a real document
can exceed.

**Fix direction:** three independent things are wrong and each is worth fixing.
(a) The thread must not be a child of the window, or the destructor must
`wait()` unbounded after `quit()` — a timed `wait` whose timeout path is
`abort()` is not a timeout. (b) `SearchWorker::run` should check
`m_cancelled` far more often than once per page — poppler offers no per-page
cancellation, so the honest options are to accept an unbounded wait or to
detach the thread and let it die on its own. (c) `cancelSearch()` returning
early on a null `QPointer` means the destructor cannot cancel a worker that
`closeSearch` already forgot about; the cancel flag should live with the window,
not with the worker (see F3).

---
## F5 — HIGH — a scroll animation is never cancelled by a relayout or by a document change, contradicting MZ.md §9 — **REPRODUCED**

**Where:** `<repo>/src/pageview.cpp:327–334` (`stopScrollAnimations`),
its **only** caller `pageview.cpp:1093` (inside `applyScale`), against
`pageview.cpp:207` (`relayout`), `pageview.cpp:902–920` (`resizeEvent`) and
`pageview.cpp:112–144` (`setDocument`).

**What MZ.md §9 claims:**

> Animations are interruptible and always start from the value the bar actually
> holds rather than from where the previous one intended to land… **A relayout
> cancels any jump in flight, because it was aimed at coordinates that have
> moved.**

**What the code does:** `stopScrollAnimations()` is called from exactly one
place — `applyScale()`, i.e. zoom and rotation. `relayout()` itself does not
call it, `resizeEvent()` does not call it on either branch, and `setDocument()`
does not call it. So the two relayouts that matter most — the window being
resized, and the document being replaced — leave the jump flying at coordinates
that no longer exist.

**Measured (`/tmp/mgtest/h7`, 30-page document, offscreen):**

```
--- A: resize during a jump (a relayout) ---
mid-flight:        value=0      running=1
just after resize: value=0      running=1   <-- §9 says this should be 0

--- B: new document opened during a jump ---
mid-flight:          value=10431  running=1
right after openPath: value=0     running=1
settled on the NEW document: value=18985  max=36311
```

**B is the user-visible one.** `PageView::setDocument` explicitly puts the bar
back to 0 (`pageview.cpp:138`), and then the surviving animation immediately
drags the *new* document down to offset 18985 — a position derived entirely from
the *previous* document's layout. The reader opens a file and watches it scroll
itself to somewhere around page 19 of a document they have not looked at yet,
with the page counter running along behind it.

C is harmless but shows the same thing: after `closeDocument()` the animation is
still `Running` against a scrollbar whose range is now `[0,0]`.

**Concrete trigger:** open a document of more than a handful of pages, press
<kbd>Ctrl</kbd>+<kbd>K</kbd> and jump to a far page (or press <kbd>Enter</kbd>
on a search hit, or click an outline entry), and within ~200 ms either resize
the window or open another document. **Note the shipped test corpus cannot show
this:** `test.pdf`, `outline.pdf`, `outline-v2.pdf` are 3 pages, `annot.pdf` and
`colour.pdf` are 1, `evil.pdf` is 2 — at those sizes there is no scroll range to
animate across, which is why ten clean acceptance suites say nothing about it.

**Memory safety:** not affected. `QPropertyAnimationPrivate::targetObject` is a
`QPointer`, and the animation is a child of the `PageView` that owns the
scrollbar, so the animation can never outlive its target. That half of suspect
#4 is sound — see the cleared-suspects section.

**Severity:** high — it is the one motion behaviour MZ.md singles out as the
reason the animation exists, and it is inverted.

**Fix direction:** call `stopScrollAnimations()` at the top of `relayout()`
(which covers `resizeEvent`, `setDocument` and `applyScale` in one place), or at
minimum from `setDocument()` and both `resizeEvent()` branches.

---

## F6 — MEDIUM — `MainWindow` destroys both `Document`s before it destroys the `PageView`s that hold raw pointers to them — **BY READING** (latent; not triggered)

**Where:** `<repo>/src/mainwindow.h:160–169` (member declaration
order) against `pageview.h:234` (`Document *m_doc`) and
`pageview.cpp:298` (`m_doc->renderPage(...)`, **unguarded**).

C++ destroys a class's members *before* its base classes. `MainWindow`'s members
include `std::unique_ptr<Document> m_doc` and
`std::unique_ptr<Document> m_compareDoc`; its base `QMainWindow` → `QWidget`
destructor is what deletes the child widgets, `m_view` and `m_compareView`.
So the order is:

```
~MainWindow() body          cancelSearch(); thread quit/wait
members, reverse order      m_compareDoc.reset()   <-- Document freed
                            m_doc.reset()          <-- Document freed
~QMainWindow → ~QWidget     deleteChildren()  -> ~PageView(m_view)
                                              -> ~PageView(m_compareView)
```

Both `PageView`s spend the whole of their own destruction holding a raw
`Document *m_doc` that points into freed memory, and `MainWindow`'s
`QObject` part is still connected (connections are torn down in `~QObject`,
which runs *after* `~QWidget::deleteChildren()`), so any signal a child emits
during teardown lands in a `MainWindow` whose `m_doc` is already gone —
`onPageChanged` (`mainwindow.cpp:618–633`) dereferences `m_doc` with no guard at
all.

`PageView::cachedPage` (`pageview.cpp:291–305`) also dereferences `m_doc`
without a null check; it is currently reachable only when `m_layout` is
non-empty, which today implies a document was set, but that is an invariant held
by convention rather than by a check.

**Status: I could not trigger it.** Harnesses `/tmp/mgtest/h1`, `h2` scenarios
1–6 and `h4` scenario 4 destroy the window with compare active, with a search in
flight, with animations running and with overlays presented, all clean under
ASan. Qt does not paint or emit scroll signals during widget destruction, so
nothing currently reaches across the gap. This is reported as a **latent
ordering inversion**, not a live use-after-free: the class is written as though
the views outlive the documents, and the language guarantees the opposite.

**Severity:** medium — no reproduction, but it is one added teardown-time
`Q_EMIT` or one `PageView` destructor body away from being a use-after-free, and
the raw `m_view` / `m_overlay` / `m_compareView` pointers are never nulled, so
nothing would catch it.

**Fix direction:** reset both documents explicitly and in the right order in the
`~MainWindow` **body** (before the members and the base run):
`m_view->setDocument(nullptr); delete m_compareView; m_compareView = nullptr;
m_compareDoc.reset(); m_doc.reset();` — or hold the views as `QPointer` and
null-check. Declaring the `unique_ptr`s *after* the widget pointers would not
help, since the widgets are owned by the base class, not by the members.

---
## F7 — CRITICAL — `Overlay::present()` destroys the previous content with `deleteLater`, which fires *inside* a nested event loop and pulls the widget out from under its own event handler — **REPRODUCED (SEGV)**

**Where:** `<repo>/src/overlay.cpp:126–130` (the `deleteLater`),
reached from `<repo>/src/mainwindow.cpp:1476` (`showProperties`)
while `<repo>/src/mainwindow.cpp:1010–1018` (`showPortals`'s `go`
lambda) is on the stack, via the non-modal nested loop at
`<repo>/src/mainwindow.cpp:687–694` (`readElevated`).

**One sentence:** an overlay row's handler can enter a nested event loop, and
`Overlay::present()` called during that loop collects the very widget whose
click handler is still on the stack — so when the loop returns, Qt resumes
`QAbstractItemView::mouseReleaseEvent` on freed memory.

This is the same mechanism as F1, in a second place, and it is the general
pattern rather than an accident of the socket: **MERGEN's only widget-destroying
idiom (`deleteLater`) and its several nested event loops are individually
correct and jointly unsound.** Qt withholds a `DeferredDelete` only while the
loop level that posted it is still current; an event posted from inside a nested
loop is stamped deeper than the loop dispatching it, so it is collected there.

**The chain, all of it reachable from the mouse and the keyboard:**

1. <kbd>Ctrl</kbd>+<kbd>K</kbd> → command overlay.
2. Choose the **Portals** row → `run()` → `showPortals()` → `present()` swaps
   the portal list in.
3. Click a portal whose far end is a **root-owned document** (the elevation path
   exists precisely so those can be opened) → `go` → `openPath()`.
4. `Document::openPath` returns `NoPermission`, so `readElevated()` runs
   `pkexec` and spins a nested `QEventLoop` — **deliberately non-modal**, so
   that "the window carries on painting instead of going grey"
   (`mainwindow.cpp:687–689`). Every shortcut, the toolbar and compare mode stay
   live; `m_opening` guards `openPath` re-entry and nothing else.
5. While the polkit prompt is up, the reader presses <kbd>Ctrl</kbd>+<kbd>I</kbd>
   → `showProperties()` → `present()` → the portal list is `deleteLater`'d and
   **collected inside the elevation loop**.
6. `pkexec` finishes, `go` returns, and Qt carries on handling the mouse release
   on a `QListWidget` that no longer exists.

**Sanitizer output (`/tmp/mgtest/h10`):**

```
### portal list=0x7b6b81689950 rows=1: noperm.pdf p1  ↔  many.pdf p5
### clicking the portal row
###   [in elevation loop] portal list alive? 1 — pressing Ctrl+I
###   [in elevation loop] portal list destroyed? 1
==185114==ERROR: AddressSanitizer: SEGV on unknown address 0x000000003212
The signal is caused by a READ memory access.
    #0 QAbstractItemView::mouseReleaseEvent(QMouseEvent*)   (libQt6Widgets)
    #1 QListView::mouseReleaseEvent(QMouseEvent*)           (libQt6Widgets)
    #2 QWidget::event(QEvent*)                              (libQt6Widgets)
    ...
rdi = 0x00007b6b81689950   r12 = 0x00007b6b81689950     <-- the freed portal list
```

The press and the release were both delivered to a live widget; the object died
between them, inside MERGEN's own nested loop. Nothing about the harness is
doing the freeing — `Overlay::present` is.

**A second, non-elevated route to the same crash.** Harness `/tmp/mgtest/h8`
shows the *modal* loops do it too: the command overlay's **Open** row enters
`chooseFile()`'s `QFileDialog` loop, and the hold-to-peek timer
(`pageview.cpp:102–109`, 300 ms) keeps running inside a modal loop like any
other `QTimer`, so `peekPage()` → `present()` collects the command overlay's
`QLineEdit` while `run()` is still on its stack. Confirmed destroyed inside the
loop; I did not get a clean crash frame out of that variant (my harness's own
follow-up key event faulted first, which is a harness artefact and not evidence
— recorded here so it is not double-counted). The elevation route above is the
one with the honest backtrace.

**Severity:** critical. A reproduced SEGV, reachable with no timing trick
sharper than "press Ctrl+I while the password prompt is up", and the elevation
loop is open for as long as the reader takes to authenticate.

**Fix direction:** the general one is worth more than any local patch.
1. `Overlay::present()` should not free the outgoing content from inside a
   caller that may be one of its own children. Take the old widget, `hide()` it
   and `setParent(nullptr)`, and hold it in a member that is released on the
   *next* `present()`/`dismiss()` at a known-safe point — or delete it through a
   zero-timer owned by the Overlay rather than `deleteLater`.
2. `readElevated`'s nested loop should block input to the window
   (`QApplication::setOverrideCursor` is not enough — a real modal guard, or
   disabling the central widget and the toolbar for the duration), or the
   elevation should be made asynchronous so no loop is needed at all.
3. Anything that re-enters MERGEN from a nested loop needs the same treatment
   `m_opening` gives `openPath`: a single re-entrancy guard covering
   `enterCompare`, `leaveCompare`, `setPresenting`, `startSearch` and every
   `present()` caller, not one guard on one function.

**Reproduction:** `/tmp/mgtest/h10.cpp`, built with `/tmp/mgtest/build.sh h10`,
run as
`XDG_STATE_HOME=/tmp/mgtest/state PATH=/tmp/mgtest/bin:$PATH QT_QPA_PLATFORM=offscreen ./h10`.
`/tmp/mgtest/bin/pkexec` is a two-line stand-in for polkit's (sleep, then `cat`
the file) so the elevation path can be exercised without a polkit agent;
`/tmp/mgtest/noperm.pdf` is `many.pdf` at mode 000;
`/tmp/mgtest/state/mergen/portals.toml` holds one portal pointing at it.

---

## F8 — MEDIUM — every route except `openPath` stays live inside `readElevated`'s nested loop — **REPRODUCED (state corruption, no crash)**

**Where:** `<repo>/src/mainwindow.cpp:687–694`, against the
`m_opening` guard at `mainwindow.cpp:519–523`.

`openPath` guards *itself* against re-entry and nothing else, but the loop it
spins is non-modal by design. Harness `/tmp/mgtest/h9` drives four routes into
that window:

| Scenario | What was done inside the elevation loop | Result |
|---|---|---|
| h9 S1 | <kbd>Ctrl</kbd>+<kbd>I</kbd> (properties) | command-overlay content **destroyed inside the loop** — this is F7's mechanism |
| h9 S2 | <kbd>Ctrl</kbd>+<kbd>D</kbd> `enterCompare()` | compare mode entered against the document `openPath` is halfway through replacing; on return `isComparing()==1` with a comparison computed from the *old* left-hand document |
| h9 S3 | `leaveCompare()` | `m_compareView` deleted mid-`openPath`; survives, but the diff bands `m_view` is still painting are never cleared |
| h9 S4 | <kbd>Ctrl</kbd>+<kbd>F</kbd> search, then <kbd>F5</kbd> presentation | a `SearchWorker` is handed the path and bytes of the document about to be replaced; presentation mode engages under a half-open document |

None of the four crashed under ASan. All four leave MERGEN in a state its own
invariants say cannot happen — most plainly S2, where `enterCompare`'s guard
(`isComparing() || !m_doc->isOpen()`, `mainwindow.cpp:1039`) passes because the
*previous* document is still open.

**Related, and simpler:** `openPath()` never calls `leaveCompare()`. Opening any
document while comparing leaves `m_compareView` on screen showing the old
comparison, `isComparing()` true, and `m_view`'s diff bands from the previous
pair still painted — `PageView::setDocument` (`pageview.cpp:112–144`) clears the
cache, words, links and hits but **not `m_diffBands`**. MZ.md §5 ruling 4 says
compare "is a mode, not a workspace, and it exits back to one document";
this route never exits it. Reproduced in `/tmp/mgtest/h2` scenario 3 (clean under
ASan — it is a state bug, not a memory bug).

**Severity:** medium — no memory error reached, but it is the substrate F7's
crash grows out of, and the compare-mode leak-through is a plain contradiction
of a documented ruling.

**Fix direction:** one re-entrancy guard covering the whole window, not one
function; `openPath` should `leaveCompare()` first; `setDocument` should clear
`m_diffBands`.

---
## F9 — MEDIUM — `enterCompare`'s `bool *lock` is freed while both scroll-lock connections are still connected — **BY READING** (does not fire today)

**Where:** `<repo>/src/mainwindow.cpp:1058–1071`.

```cpp
auto *lock = new bool(false);
m_compareView->connect(m_compareView, &QObject::destroyed, [lock] { delete lock; });
const auto bind = [lock](PageView *from, PageView *to) {
    QObject::connect(from->verticalScrollBar(), &QScrollBar::valueChanged, to, [=](int value) {
        if (*lock) return;                    // <-- reads *lock
        *lock = true;
        to->verticalScrollBar()->setValue(value);
        *lock = false;
    });
};
bind(m_view, m_compareView);
bind(m_compareView, m_view);
```

**The ordering is backwards, and I measured it rather than assumed it.** Qt 6's
`QWidget::~QWidget()` emits `destroyed()` **before** `d->deleteChildren()`, and
`~QObject` (which tears down incoming connections) runs after both. Probe
`/tmp/mgtest/ord.cpp`:

```
deleting scroll area:
  SCROLLAREA destroyed() emitted; scrollbar already gone? 0
  scrollbar destroyed
```

So on `leaveCompare()` → `delete m_compareView` (`mainwindow.cpp:1082`) the
sequence is:

1. `emit destroyed(m_compareView)` → **`delete lock`** — `lock` is now freed.
2. `deleteChildren()` destroys `m_compareView`'s own scrollbar, which is only
   now disconnecting **connection 2** (sender `m_compareView`'s bar, context
   `m_view`).
3. `~QObject` disconnects **connection 1** (sender `m_view`'s bar — still very
   much alive — context `m_compareView`).

Between step 1 and step 3, both lambdas are still connected and both would read
and write `*lock` through a dangling pointer. Any `valueChanged` on **either**
scrollbar in that window is an immediate use-after-free on a heap `bool`.

**It does not fire today.** I traced every candidate: `~QAbstractSlider` does not
emit `valueChanged`; `QAbstractScrollArea` nulls its viewport before `~QWidget`
runs; and the parent `QHBoxLayout` learns of the removal only via `ChildRemoved`
in `~QObject`, whose `invalidate()` posts a `LayoutRequest` rather than resizing
`m_view` synchronously. Harness `/tmp/mgtest/h2` scenario 2 — three
enter/leave/re-enter cycles with 40 interleaved scroll steps each, then
destruction with compare still active — is clean under ASan.

This corrects the interim note earlier in this file, which said the free happens
after both connections are dead. **It does not.** The suspicion in the brief was
right; the mechanism is just narrower than "a scrollbar lambda outlives the free"
— the lambdas outlive the free by construction, and only the absence of a
`valueChanged` in that window keeps it from being a live bug.

**Window close is the safe order, for the wrong reason.** There, `m_viewRow`'s
children go in creation order: `m_view` first (its scrollbar dies, killing
connection 1; its role as connection 2's context dies too), then
`m_compareView` frees `lock` with nothing left attached. Correct by accident of
insertion order.

**Severity:** medium — latent, but it is a raw `new`/`delete` pair whose
lifetime is shorter than the two lambdas that dereference it, which is exactly
the shape that becomes a crash on a Qt upgrade or a `setRange` added to a
teardown path.

**Fix direction:** stop heap-allocating the flag. `auto lock = std::make_shared<bool>(false)`
captured by value into both lambdas makes the flag outlive whichever connection
dies last, costs one allocation, and deletes the `destroyed` handler entirely.
A `bool` member on `MainWindow` would do just as well and allocate nothing.

---

## F10 — LOW — `showCommands`'s `deeds` is freed before the widgets whose slots read it — **BY READING** (does not fire today)

**Where:** `<repo>/src/mainwindow.cpp:1225–1226`, with the readers at
`1228` (`rebuild`), `1233` (`add`) and `1305` (`run`).

Same shape as F9 and the same measured ordering: `content` emits `destroyed`
(freeing `deeds`) **before** `deleteChildren()` destroys `entry` and `list`, and
before `~QObject` drops the `textChanged` / `returnPressed` / `itemClicked`
connections that `rebuild` and `run` are attached to. For the window between,
`deeds` is dangling and three connected lambdas dereference it.

Nothing in `~QLineEdit` or `~QListWidget` emits those signals, so it does not
fire. Harness `/tmp/mgtest/h6` scenario 3 (30 rounds of command → outline →
properties → re-present → dismiss) is clean under ASan.

**The specific worry in the brief is answered, and the answer is yes.** `run`
copying the `std::function` before dismissing (`mainwindow.cpp:1311`) *is*
sufficient, and for a simpler reason than it looks: `Overlay::dismiss()` only
hides — it never destroys the content — so `deeds` is not at risk at that point
at all. The content is destroyed only by a later `Overlay::present()`, and that
is a `deleteLater`. Opening the command overlay twice is likewise safe: the
second `showCommands` builds `content2` and `deeds2` before `present()` posts
`content1`'s deferred delete, and the two never alias.

The one thing that *does* bite is F7 — but that is `present()`'s deferred delete
landing inside a nested loop, not `deeds` at all.

**Severity:** low. **Fix direction:** hold `deeds` in a `std::shared_ptr`
captured by value into the three lambdas, or make it a `QVector` member of a
small content `QWidget` subclass so it dies with its own destructor rather than
from a `destroyed` handler that runs too early.

---

## F11 — LOW — `IconSet`'s function-local statics hold `QPixmap`s that outlive `QApplication` — **REPRODUCED as benign on this platform**

**Where:** `<repo>/src/iconset.cpp:61–77` — `cache()` holds a
`static QHash<CacheKey, QIcon>`, alongside `cachedPaletteKey()` and
`cachedPixelRatio()`.

`QIcon` owns `QPixmap`s; a function-local static is destroyed by `__cxa_atexit`
**after** `main` returns, i.e. after `QApplication` is gone. Destroying GUI
resources without a live `QGuiApplication` is the classic static-destruction-
order hazard.

**Tested, not assumed.** Harness `/tmp/mgtest/h6` scenario 5 warms the cache
(the Nerd Font *is* installed here — `hasGlyphFont()` reported 1, so real
pixmaps were rendered), churns the palette five times, then returns from `main`.
Clean under ASan + UBSan, no warning, no crash. On MERGEN's platform
(`QT_QPA_PLATFORM=offscreen` here, Wayland in production) `QPixmap` is backed by
`QRasterPlatformPixmap` — plain memory, no server-side handle to release — so
there is nothing left to talk to by the time the destructor runs.

**The two-window question does not arise.** MZ.md §9 makes a second instance
hand its argument to the first and exit (`main.cpp:30–35`), so two
`MainWindow`s never coexist in one process. If they ever did, the cache would
still be correct: it is keyed by `(code, pixels)` with the palette tracked in a
separate static and dropped wholesale on any change
(`iconset.cpp:153–157`), and Qt's palette is process-wide, so two windows could
not disagree about it.

**Severity:** low — real in principle, benign in practice on the only platform
MERGEN supports, and only at exit.

**Fix direction:** none required. If it is ever wanted, clear the cache from a
`QCoreApplication::aboutToQuit` connection so the pixmaps die while the
application is still up.

---

## Suspects that are sound, and should not be changed

Recorded explicitly, because knowing what *not* to touch is worth as much as the
defect list.

**`Overlay`'s `qApp` `focusChanged` connection (`overlay.cpp:107–115`) is
correct.** The connection's context object is the `Overlay` itself, so it is
torn down in `~QObject`. It can still fire during `~QWidget` — but every
destruction-time focus change Qt makes passes `nullptr` as the new focus widget
(`QWidgetPrivate::deactivateWidgetCleanup` → `setActiveWindow(nullptr)`,
`QWidget::clearFocus()`, and `setParent(nullptr)` on a widget owning the focus
all route through `QApplicationPrivate::setFocusWidget(nullptr, …)`), and the
handler's first line is `if (!isVisible() || !now) return;`. The `!now` guard is
load-bearing and should stay. `hide()` inside `dismiss()` is likewise safe:
`setVisible(false)` sets `WA_WState_Hidden` *before* `hide_helper()` moves focus,
so the handler sees `isVisible() == false` and returns rather than re-entering
`dismiss()`.

**`m_focusBefore` (`overlay.h:63`, `overlay.cpp:141,158`) is correct.** It is a
`QPointer`, it is only captured when the overlay is not already up (so stacking
outline over command over properties never loses the original), and `dismiss()`
falls back to `parentWidget()->setFocus()` when it has gone null. I traced every
path in the brief — content swapped by `present()`, document closed underneath,
window de-activated, presentation mode engaged — and found no way to restore
focus to a dead widget. The only residual is cosmetic: if focus was in the
toolbar's page box and presentation mode has since hidden the toolbar,
`dismiss()` hands focus to a hidden widget and it goes nowhere.

**The scroll animations are memory-safe (`pageview.cpp:163–187, 265–266`).**
`QPropertyAnimationPrivate::targetObject` is a `QPointer`, so an animation can
never write into a destroyed scrollbar; the animation is parented to the
`PageView` that owns the bar, so target and animation die together, and
`QAbstractAnimation::~QAbstractAnimation` stops without virtual dispatch. The
scrollbars are never replaced (`setVerticalScrollBar` is never called), so the
target cannot go stale. `setStartValue(bar->value())` on a restart is exactly
right and is what MZ.md §9 asks for. The animation objects are created once and
reused — there is no leak per jump. What is wrong is only *when*
`stopScrollAnimations()` is called — see F5.

**The search "current pass" guard is correct** (`mainwindow.cpp:1693, 1700,
1711`) — see F3 for why, including why the ABA hazard cannot bite. The comment
at `mainwindow.cpp:1688–1691` is an accurate description of a real Qt behaviour
and the guard it justifies.

**Two `PageView`s sharing one `Overlay` is safe on both destruction paths.**
`m_overlay` is a child of `m_view` (`mainwindow.cpp:216`), not of `m_viewRow`,
so `leaveCompare()`'s `delete m_compareView` cannot touch it. On window close,
`m_viewRow` deletes `m_view` first — taking the overlay with it — then
`m_compareView`; `MainWindow::m_overlay` dangles from that moment, but nothing
dereferences it before `~QObject` cuts the connections. It is the same latent
raw-pointer inversion as F6 and wants the same fix (`QPointer`), not a
restructure. The real consequence of the shared overlay is presentational, not
lifetime: `present()` does `setGeometry(parentWidget()->rect())`
(`overlay.cpp:144–146`), so in compare mode every overlay is confined to the
left-hand pane rather than centred on the window. Worth a look against MZ.md §6,
but it is not this audit's call.

**`Redact::run`'s `int *removed`** (`redact.cpp:199, 215`) is fine: `removed` is
declared outside the `try`, and the `Cut` filter holding `&removed` dies with the
`QPDF` at the end of that block, so the pointee always outlives the pointer.
(The separate `QByteArray` temporary defect in this file belongs to another
agent's report and is not duplicated here.)

---
## Smaller notes

- **`mainwindow.cpp:1322` / `1735–1737` — a raw `QObject *` smuggled through a
  `QVariant` property.** `entry->setProperty("mergenCommandList", …)` stores the
  command list as a bare pointer, and `eventFilter` does `qobject_cast` on
  whatever comes back — a vtable read, so a stale value is a use-after-free
  rather than a null. It is safe today only because `entry` and `list` are
  siblings and `entry` (created first) is destroyed first, so `list` cannot
  predecease its own reference. `QVariant::fromValue(QPointer<QListWidget>(list))`
  costs nothing and removes the reasoning. **nit.**
- **`mainwindow.cpp:628` — unchecked downcast.**
  `static_cast<QIntValidator *>(const_cast<QValidator *>(m_pageEdit->validator()))`
  is correct only while nothing ever calls `setValidator` again. `qobject_cast`
  with a null check is the same line. **nit.**
- **`document.cpp:435–440` — a search over a password-protected document
  silently finds nothing.** `SearchWorker::run` re-opens the file itself and
  bails on `doc->isLocked()`, but the password the reader typed lives nowhere
  (correctly — MX.md §5 bans storing it), so every search on an unlocked
  encrypted document reports "No hits" rather than saying it cannot look. Not a
  lifetime defect; flagged because it is invisible and reads as a wrong answer.
  **low.**
- **`pageview.cpp:498–516` — `linkAt` returns a pointer into a `QHash` value.**
  Sound as used (both callers dereference it immediately and nothing mutates
  `m_links` in between), but the signature invites a caller that holds it across
  a `linksOf()` on another page, which would rehash. Returning `PageLink` by
  value would cost nothing. **nit.**
- **`pageview.cpp:291–305` — `cachedPage` dereferences `m_doc` with no null
  check**, relying on "`m_layout` non-empty implies a document". True today;
  see F6 for why that invariant is thinner than it looks. **nit.**

---

## The eight named suspects — final verdicts

| # | Suspect | Verdict | Evidence |
|---|---|---|---|
| 1 | `enterCompare` raw `bool *lock` | **Real hazard, latent.** The suspicion was right but the mechanism is narrower than "a scrollbar lambda outlives the free" — `lock` is freed by `destroyed()` *before* Qt tears down either connection, so both lambdas outlive the free by construction. Nothing emits `valueChanged` in that window today. `leaveCompare` is the risky order; window close is safe by accident of insertion order. | **F9** — ordering measured with `/tmp/mgtest/ord.cpp`; stress clean under ASan |
| 2 | `showCommands` `deeds` vector | **Sound in practice.** `run`'s copy of the `std::function` is sufficient, and for a simpler reason than expected: `dismiss()` never destroys `content` — only a later `present()` does, via `deleteLater`. Opening the overlay twice is safe. Same freed-before-its-readers ordering as #1, also latent. | **F10** |
| 3 | `Overlay::present` / `dismiss` / `m_focusBefore` / `focusChanged` | **Split.** `m_focusBefore` and the `focusChanged` connection are **correct** and should not be changed — the `!now` guard covers every destruction-time focus change. But `present()`'s `deleteLater` is the source of the worst crash in this report. | **F7 (critical)**; cleared-suspects section |
| 4 | Scroll animations vs `stopScrollAnimations` | **Split.** Memory-safe — `QPointer` target, parented to the scrollbar's owner, never re-targeted, `setStartValue(bar->value())` correct. But `stopScrollAnimations()` is called only from `applyScale`, so a relayout or a document change does **not** cancel a jump, contradicting MZ.md §9 outright. | **F5 (reproduced)** |
| 5 | Search worker thread | **Three defects.** Timed `wait(3000)` whose timeout path is `abort()`; a worker that leaks when its queued `run` never lands; a cross-thread `QPointer` check-then-use on `cancel()`. The "current pass" guard itself is **sound**. No `Poppler::Document` is shared across threads — `run()` opens its own handle, as MZ.md §9 requires. | **F4 (critical, reproduced), F2 (reproduced), F3** |
| 6 | Two `PageView`s sharing one `Overlay` | **Safe on both destruction paths.** Residual issue is presentational (the overlay is confined to the left pane in compare mode), plus the same raw-pointer inversion as #8. | cleared-suspects section |
| 7 | `IconSet` function-static caches | **Benign on this platform.** Warmed the cache with the real font and exited under ASan+UBSan: clean. Two windows cannot coexist by design. | **F11 (tested)** |
| 8 | `m_doc` / `m_compareDoc` vs `PageView`'s raw `Document *` | **Real ordering inversion, latent.** Members are destroyed before the base class deletes the widgets, so both `PageView`s spend their whole destruction holding freed `Document`s, with `MainWindow`'s connections still live. Could not trigger it. | **F6** |

---

## Findings, most severe first

| ID | Sev | What | Status |
|---|---|---|---|
| **F1** | critical | `Control::onConnection` writes to a `QLocalSocket` freed inside a nested loop | **REPRODUCED** — ASan heap-use-after-free |
| **F7** | critical | `Overlay::present`'s `deleteLater` collects a widget inside a nested loop, under its own event handler | **REPRODUCED** — SEGV in `QAbstractItemView::mouseReleaseEvent` |
| **F4** | critical | Closing the window during an uncancellable search aborts the process | **REPRODUCED** — `qFatal` in `~QThread`; confirmed without sanitizers |
| **F2** | high | `SearchWorker` leaked when its queued `run` never lands | **REPRODUCED** — LeakSanitizer |
| **F3** | high | Cross-thread check-then-use on `m_searchWorker` in `cancelSearch` | by reading |
| **F5** | high | Relayout / document change does not cancel a scroll animation (contradicts MZ.md §9) | **REPRODUCED** — measured |
| **F6** | medium | `Document`s destroyed before the `PageView`s holding raw pointers to them | by reading (latent) |
| **F8** | medium | Everything except `openPath` stays live and reentrant inside `readElevated`'s loop | **REPRODUCED** — state corruption |
| **F9** | medium | `bool *lock` freed while both scroll-lock connections are still connected | by reading (latent) |
| **F10** | low | `deeds` freed before the widgets whose slots read it | by reading (latent) |
| **F11** | low | `IconSet` statics hold `QPixmap`s past `~QApplication` | tested benign |
| — | nit | raw `QObject *` in a `QVariant` property; unchecked `QValidator` downcast; `linkAt` returning an interior pointer; unguarded `m_doc` in `cachedPage`; silent no-hits search on encrypted documents | by reading |

**The single theme.** Three of the four most severe findings are the same bug
wearing different clothes: MERGEN destroys widgets and sockets with
`deleteLater`, and it enters nested event loops in seven places
(`readElevated`, `promptForPassword`, three `QFileDialog`s, `QPrintDialog`,
`showAbout`). A `DeferredDelete` posted inside a nested loop is collected by
that loop, not deferred past it — so any object freed that way dies while the
call that entered the loop is still on its stack. `m_opening` guards exactly one
function against exactly one re-entry; nothing guards the rest. Fixing F1 and F7
one at a time will leave the pattern in place. The durable fix is a window-wide
re-entrancy guard plus never freeing a widget from a path a nested loop can
reach.

---

## Reproduction assets

All under `/tmp/` (scratch; nothing in the repository was modified).

| Path | What |
|---|---|
| `/tmp/mergen-asan` | ASan+UBSan Debug build (`-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`) |
| `/tmp/mergen-plain` | plain Release build, for confirming F4 is not a sanitizer artefact |
| `/tmp/mgtest/build.sh` | links a harness against the ASan objects (`-fPIC` required — Qt's protected `staticMetaObject` symbols cannot take copy relocations) |
| `/tmp/mgtest/build-plain.sh` | the same against the uninstrumented objects |
| `/tmp/mgtest/h1..h10.cpp` | the harnesses; `h3` = F1, `h4` = F2, `h5` = F4, `h7` = F5, `h9` = F8, `h10` = F7 |
| `/tmp/mgtest/ord.cpp` | the `QWidget` `destroyed()`-vs-`deleteChildren()` ordering probe behind F9 |
| `/tmp/mgtest/bin/pkexec` | two-line stand-in for polkit's, so the elevation path runs without an agent |
| `/tmp/mgtest/many.pdf` | 30 pages — the shipped corpus has none over 3, which is why F5 was invisible to the acceptance suites |
| `/tmp/mgtest/huge.pdf` | one page, 900 000 show-text operators — the uncancellable search behind F4 |
| `/tmp/mgtest/link.pdf` | two pages with an internal `GoTo` link, for hold-to-peek |
| `/tmp/mgtest/locked.pdf` | password-protected (`qpdf --encrypt`), for the password-prompt nested loop |
| `/tmp/mgtest/noperm.pdf` | mode 000, for the elevation nested loop |

Run everything with `QT_QPA_PLATFORM=offscreen`. `valgrind` is not installed on
this machine; `gdb` is, and was used for F4's backtrace.

**A note on the test corpus.** Every shipped fixture is 1–3 pages
(`test.pdf` 3, `outline.pdf` 3, `outline-v2.pdf` 3, `annot.pdf` 1,
`colour.pdf` 1, `evil.pdf` 2). At that size there is no scroll range to animate
across, no page loop long enough to cancel, and no search that takes measurable
time — which is a large part of why ten green milestones coexist with the
findings above. Adding one long document and one heavy page to the corpus would
have caught F4 and F5 without any of this machinery.
