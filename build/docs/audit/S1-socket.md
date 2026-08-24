# S1 — IPC control-socket audit

Scope: `src/control.cpp` / `src/control.h`, `MainWindow::runCommand` and
`MainWindow::listenForCommands` in `src/mainwindow.cpp`, and the single-instance
handover logic in `src/main.cpp`. Read-only audit against the intent recorded in
`build/docs/MZ.md` §3 and §9 ("The control socket"). This file is written
incrementally as findings are confirmed; entries are not held back to the end.

Method: full build at `build/mergen`, run headless
(`QT_QPA_PLATFORM=offscreen`) against isolated `$XDG_RUNTIME_DIR` scratch
directories under `/tmp/mz_audit/`, driven by plain-stdlib Python
(`socket.AF_UNIX`), independent of Qt on the client side. Numbers below are
measured, not estimated, unless marked "assessed by reading."

Constitution claims in force (not bugs if honored):
- §3: socket chosen over D-Bus specifically so nothing but the kernel needs to
  be running; drivable from a shell one-liner.
- §9: `QLocalServer` on `$XDG_RUNTIME_DIR/mergen-$UID.sock`, one line per
  command, `ok` / `err: <reason>`. "It accepts local connections only, by
  construction." "Every command is one the reader could already have issued
  from the keyboard; the socket adds reach, not capability." Second instance
  hands its argument to the first and exits. Stale socket from a crashed
  instance "is detected and replaced, not inherited."


**Index, most severe first** (file order below is chronological-by-discovery,
not severity — use this list to navigate):
1. Finding 0 — CRITICAL — reentrant socket connection during a password
   dialog crashes the process (SIGSEGV), reproduced twice, deterministic.
2. Finding 1 — HIGH — idle socket connections freeze the entire GUI thread,
   ~1s per connection, measured up to 5s with 5 connections.
3. Finding 3 — HIGH — a non-answering peer at the socket path (rogue
   listener, or ordinary timing accident) causes a launch to silently do
   nothing; the file argument is dropped with zero feedback.
4. Finding 2 — MEDIUM — oversized commands (~200KB+) are silently truncated
   and the truncated garbage is executed and reported as `ok`.
5. Finding 4 — held protections confirmed by direct testing, plus two
   low/nit observations (tab-vs-space parsing, silent no-arg second launch).
---

## Finding 1 — GUI-thread freeze proportional to number of idle socket connections (CONFIRMED, empirically reproduced)

**File/line:** `src/control.cpp:71-93` (`Control::onConnection`), specifically
line 77: `client->waitForReadyRead(kReplyMs)` with `kReplyMs = 1000`
(`control.cpp:20`), called on the main/GUI thread.

**Description:** `Control` and its `QLocalServer` are constructed with
`MainWindow` (`this`) as parent in `MainWindow::listenForCommands`
(`mainwindow.cpp:730-733`) and never moved to another thread (no
`moveToThread` call anywhere near it; the only `QThread` in `mainwindow.cpp`
is `m_searchThread`, used for search, unrelated). `onConnection()` drains
*all* currently pending connections in a `while (nextPendingConnection())`
loop, and for each one blocks the calling thread for up to 1000ms via
`waitForReadyRead` if that client hasn't sent anything yet. A client that
connects and sends nothing therefore blocks the entire GUI (rendering, input,
and all other socket replies) for a full second, and N such clients block it
for approximately N seconds, processed serially.

**Reproduction (measured):** Built `dos_test.py` (stdlib `socket.AF_UNIX`,
no Qt) against a live `mergen test.pdf` instance
(`/tmp/mz_audit/rd1/mergen-1000.sock`). Baseline: connect + `goto 1` + read
reply, with no other clients connected. Then: open N clients that connect and
send nothing, then issue the same `goto 1` from a fresh client and time the
full round trip.

| idle (silent) clients open | time for a legitimate `goto 1` to get its reply |
|---|---|
| 0 (baseline) | 0.000s |
| 0 (baseline, repeat) | 0.000s |
| 1 | 0.956s |
| 3 | 2.968s |
| 5 | 4.977s |

Scaling is linear at ~0.99s per idle connection, matching `kReplyMs = 1000`
paid once per silent client, serially, exactly as the code predicts.

**Control test (isolates the cause to silence, not connection count):**
`control_test.py` opens 5 *simultaneous* clients that all send `goto 1`
immediately (no idling) and measures each one's latency:
`N=5 active(non-idle) simultaneous clients: individual latencies=['0.000',
'0.000', '0.000', '0.000', '0.000'] max=0.000s total_wall=0.001s`. Five
concurrent connections cost nothing when they all speak immediately — the
entire cost is specifically the per-silent-connection `waitForReadyRead`
timeout, not accept-queue depth or connection count in general.

**Assessment:** This is not a documented tradeoff. MZ.md §9's "Threading"
note discusses only page rendering (night mode / compare), and says nothing
about the control socket running synchronously on the GUI thread; §3's
justification for choosing a socket over D-Bus is about not requiring a
session bus, not about accepting a same-thread blocking design. Any local
process belonging to the same user (the socket is user-scoped, so this is not
a cross-user attack, but the same user's own broken script, a hung shell
one-liner, or literally `nc -U $SOCK` left open) — anything that connects to
the socket and doesn't immediately write a full line — freezes the entire
application for a full second per such connection, with no timeout recovery
faster than 1s/connection and no upper bound on how many such connections a
client can open (limited only by `QLocalServer`'s default
`maxPendingConnections`, i.e. accept-queue backlog, not by any check in this
code). A trivial `while true; do timeout 0.1 nc -U $SOCK; done` from the
reader's own shell (e.g. a slightly-wrong keybinding, or a stuck script that
opens the socket then blocks before writing) is enough to make MERGEN
perceptibly unusable for as long as it runs.

**Severity:** High. Trivial to trigger (accidentally or deliberately) by
anything that can open the socket, requires no special privilege beyond what
the design already grants (same-user), and directly defeats the "drivable
from a keybinding" use case the socket exists for (§3) by making the whole
GUI stall.

**Suggested fix direction:** Move the control socket's accept/read handling
off the GUI thread (e.g. a dedicated `QThread` with its own event loop, or a
`QThreadPool` task per connection posting results back via queued
connection), or at minimum stop blocking synchronously: connect to
`QLocalSocket::readyRead` asynchronously instead of calling
`waitForReadyRead` from the GUI thread, and drive the command handling from
that signal once a full line has actually arrived, with a `QTimer`-based
per-connection timeout that disconnects the client without blocking anything
else in the meantime.

---

## Finding 2 — Oversized commands are silently truncated, the truncated garbage is executed, and the client is told `ok` (CONFIRMED, empirically reproduced; refines/replaces the "unbounded read → GB memory growth" hypothesis)

**File/line:** `src/control.cpp:77-85` — one `waitForReadyRead(kReplyMs)` and
one `client->readLine()` call per connection, not in a loop, with no
`maxSize` argument to `readLine()` and no length check on the result before
it is handed to the command handler.

**What I expected going in, and what I actually found:** The audit brief
raised "what happens if a client sends 1GB with no newline — memory growth?"
I tested this directly and it does **not** reproduce as stated — see the
"refuted" note below. But the same missing bound produces a *different*,
more concrete bug: well-formed, complete, newline-terminated commands whose
total line length exceeds roughly one OS socket-buffer's worth of bytes are
silently truncated mid-argument, and the truncated prefix is executed as if
it were the whole command, with the reply claiming success.

**Reproduction (measured):** `longsearch_test.py` sends a syntactically
complete `search <N bytes of 'a'>\n` (proper trailing newline) over a
correctly-connected socket, using a background thread to drain the reply
concurrently (so the test harness itself never blocks the exchange), and
reports how many bytes the client actually got to send and what came back:

| search-term size | bytes actually sent | server reply |
|---|---|---|
| 1,000 | full (1,008/1,008) | `ok` |
| 20,000 | full | `ok` |
| 70,000 | full | `ok` |
| 100,000 | full | `ok` |
| 150,000 | full | `ok` |
| 200,000 | full | `ok` |
| 300,000 | **truncated: 255,808/300,008**, then `Broken pipe` on the client's next `send()` | `ok` |
| 2,000,000 | **truncated: 292,352/2,000,008**, then `Broken pipe` | `ok` |

At and below 200,000 bytes the full line is delivered and the server behaves
correctly. Above roughly 212,992 bytes (300,000 and 2,000,000 both cut off
in the 250–292KB range) the client's write gets cut off — the server has
already stopped reading and moved on — yet **the server still answers `ok`**.
`/proc/sys/net/core/{wmem,rmem}_default` on this machine are both `212992`
bytes, i.e. the cutover lines up almost exactly with the default Unix-socket
kernel buffer size: the server's single `waitForReadyRead` + `readLine()`
pair drains whatever the kernel handed it in that one pass — up to about one
buffer's worth — and, finding no `\n` in it, still returns it as "the line"
rather than recognizing the line is incomplete. `runCommand("search",
<~256KB of truncated "a"s>)` then runs a real search for that garbage string
and reports success. The connection is then closed by the server while the
client still has unsent/unread bytes outstanding, producing an abortive
close (the client's subsequent `send()` gets `EPIPE`, matching a `RST`-style
teardown rather than a graceful one).

**Confirmed present with adversarial (no-newline) input too, same
mechanism:** a single unterminated line of `x` characters, sent as fast as
possible, is likewise captured only up to about the same ~70–290KB range
regardless of whether the client is trying to send 1MB, 20MB, or 100MB of it
(all three produced a captured chunk of exactly 73,110 reply bytes in
repeated runs — i.e. the captured amount does **not** grow with attacker
intent, it's capped by the same one-shot buffer read). The server echoes
this captured garbage back in full inside `err: unknown command <garbage>`,
so there is a bounded (not unbounded) reflection of attacker-supplied bytes
back to the attacker — on the order of the same ~100–300KB ceiling, not
larger.

**"Refuted" — no unbounded/GB-scale memory growth observed:** `VmRSS` of the
target process was `61,924 kB` at baseline and never exceeded `~62,588 kB`
(`VmHWM`, the high-water mark) across all of the above, including three
separate attempts to push 1MB/20MB/100MB blobs with no newline and a 5MB
single line. The process never accumulated attacker data proportional to
what the attacker tried to send; the true behavior is a single bounded
read, not the unbounded accumulation the raw `readLine()` signature
suggests. This part of the original concern is **not confirmed** — stated
here explicitly because a held protection deserves to be reported too.

**Assessment:** This is a real robustness defect, independent from the
memory-growth question. Nothing in MZ.md discusses a size limit on socket
commands, so this isn't a documented tradeoff being second-guessed; it's a
gap between "one line per connection" (§9's stated protocol) and what the
code actually does with a line that doesn't fit in one read. The severity
for MERGEN's actual command set is limited because the only argument a
reader would plausibly make very long is a file path (bounded in practice by
filesystem path limits, `PATH_MAX` = 4096 on Linux, far under the ~200KB
threshold) or a search term (unlikely to be hundreds of KB, but not
validated against it either). The concrete failure mode is: any command
whose argument legitimately exceeds the platform's default Unix-socket
buffer size is silently corrupted into a different, shorter command and
reported as having succeeded, rather than being rejected with a clear error
or fully received.

**Severity:** Medium. Bounded blast radius (no memory exhaustion, no code
execution, requires an oversized argument that legitimate MERGEN usage is
unlikely to produce), but a genuine correctness bug: `ok` is returned for a
command that was not, in fact, the command sent.

**Suggested fix direction:** Loop reading (accumulating into a `QByteArray`)
until a `\n` is found or a sane maximum line length is exceeded (e.g. a few
KB, generous for a path or search term), and reply with a clear `err:` for
an over-length line instead of silently acting on a truncated prefix. This
also naturally caps the reflection-amplification behavior described above
for oversized unknown verbs.

---

## Finding 0 (most severe) — Reentrant control-socket connection during a password-prompt dialog crashes the process (SIGSEGV) (CONFIRMED, empirically reproduced twice, deterministic)

**File/line:** `src/control.cpp:71-93` (`Control::onConnection`) is
reentered while a previous, still-on-the-C++-stack invocation of itself is
blocked inside `MainWindow::openPath` → `MainWindow::promptForPassword`
(`mainwindow.cpp:653`, called from `mainwindow.cpp:542`) → `QInputDialog::
getText()` → `QDialog::exec()`. The root cause on the MERGEN side is
`Document::adopt()` in `src/document.cpp:63-76`: line 69 does `m_doc =
std::move(doc);` for a still-*locked* (password-needed) document — the live
document object is swapped to the new, not-yet-unlocked one *before* a
password has been obtained, and stays that way for as long as the modal
prompt is up.

**Mechanism (from a full, symbol-resolved backtrace, see below):**
1. Connection 1 sends `open <path to a password-protected PDF>`.
   `Control::onConnection` (call A) invokes `runCommand` → `openPath` →
   `Document::openPath` → `Document::adopt`, which replaces `m_doc`'s
   internal `Poppler::Document` with the new, locked one and returns
   `NeedsPassword`.
2. `openPath` then calls `promptForPassword`, which calls `QInputDialog::
   getText(...)`, which calls `QDialog::exec()`. This starts a **nested**
   `QEventLoop`, still on the GUI thread, with call A's entire stack — all
   the way down through `Control::onConnection` — still resident, unreturned.
3. Because it's a full nested event loop (not a narrow single-fd wait), it
   still services `QSocketNotifier` events for the listening control socket.
   A second, unrelated connection (e.g. sending `goto 1`) arrives during
   this window and is accepted, and `Control::onConnection` is **called a
   second time, reentrantly, from inside the first, still-suspended call**.
4. Call B processes `goto 1`: `runCommand`'s `goto` branch calls `m_doc->
   pageCount()`, which reaches into poppler's `Catalog::getNumPages()` on
   the document object that call A just swapped in — a locked/encrypted
   document poppler has not finished unlocking. This crashes with `SIGSEGV`
   inside `pthread_mutex_lock()`, called from `Catalog::getNumPages()`.

**Full backtrace (via `coredumpctl debug <pid> -A "-batch -ex bt -ex quit"`),
innermost first, annotated:**
```
#0  pthread_mutex_lock () from libc.so.6                     <- crash site
#1  Catalog::getNumPages() () from libpoppler.so.163
#2  mergen::MainWindow::runCommand(...)                       <- call B: "goto 1"
#3  std::_Function_handler<...>::_M_invoke(...)
#4  mergen::Control::onConnection()                            <- REENTRANT call
#5–#8  Qt event dispatch (QSocketNotifier::event, notify_helper, ...)
#9–#16 glib main-context iteration inside a nested QEventLoop
#17 QDialog::exec()
#18 QInputDialog::getText(...)
#19 mergen::MainWindow::promptForPassword(QString const&)
#20 mergen::MainWindow::openPath(QString const&)
#21 mergen::MainWindow::runCommand(...)                        <- call A: "open <locked.pdf>"
#22 std::_Function_handler<...>::_M_invoke(...)
#23 mergen::Control::onConnection()                             <- original (outer) call
#24–#30 Qt event dispatch
#31–#36 glib main-context iteration (the ordinary QApplication::exec() loop)
#37 main()
```

**Reproduction (measured, deterministic across 2/2 independent runs):**
1. Built a password-protected test PDF: `qpdf --encrypt secret123 secret123
   256 -- test.pdf pw_protected.pdf`.
2. Started a fresh `mergen test.pdf` against an isolated `$XDG_RUNTIME_DIR`.
3. Connection 1: `open /tmp/mz_audit/pw_protected.pdf\n`, reply not awaited.
4. ~1 second later, connection 2 (fresh): `goto 1\n`.
5. Both runs: the process died with `SIGSEGV` (`dmesg`/shell reported
   "Segmentation fault (core dumped)"; `coredumpctl list <pid>` confirmed
   `SIG SIGSEGV`, `COREFILE present`) within ~1.3s of connection 1's command,
   ~0.3s after connection 2's. Both client connections received an empty
   read (`b''`) — the process died before replying to either.
   Run 1: PID 103899, coredump at 15:15:04. Run 2: PID 106034, coredump at
   15:15:59. Timings both times: conn2 "elapsed=0.33-0.35s", conn1
   "elapsed=1.33-1.35s" — consistent, not a rare race.

**Note on provenance:** while investigating why an earlier, long-lived test
instance of mine (unrelated PID 79359) had disappeared, `coredumpctl list
mergen` surfaced *other* SIGSEGV coredumps (PIDs 91781, 92784, 93534, 93579,
93813, none mine) with `Control::onConnection` / `promptForPassword` /
`openPath` already in their backtraces — evidence that a different,
concurrent session in this shared sandbox had independently found the same
crash while fuzzing password handling. I did not rely on that as the
finding; I built my own password-protected PDF and reproduced the crash
twice, cleanly, end to end, myself, with the mechanism above confirmed from
my own coredumps (PIDs 103899 and 106034). The two are almost certainly the
same underlying bug, found independently by two different lines of attack
(direct password-dialog fuzzing vs. socket reentrancy analysis).

**Assessment:** Not a documented tradeoff — MZ.md says nothing about the
control socket being reentrant, and §9's "Threading" note only discusses
moving *rendering* work off the GUI thread after measuring; it does not
anticipate the control socket re-entering itself. This is squarely a gap
between the "one line per connection" model §9 describes and what the
synchronous, blocking, same-thread implementation actually does when a
handler transitively pumps a nested event loop (any modal dialog reachable
from a socket command — a password prompt is the one this codebase has, but
the hazard class is general: any `QDialog::exec()`/nested-loop call reached
from `runCommand` reopens the door for reentrant `onConnection` calls while
application state is mid-mutation). This is trivially reachable by the same
user the socket already trusts, requires no elevated privilege, and is
100% reproducible with two plain socket writes and a two-line Python script.

**Severity:** Critical. Remote (same-user) denial of service via a two-line
reproduction, no special access beyond what the design already grants, deterministic. Because the
crash is a `pthread_mutex_lock` on an object mid-transition rather than a
clean null-pointer dereference, I cannot rule out worse-than-crash outcomes
under different timings/allocator states without further investigation
(not attempted — out of scope for a read-only audit) — flagging this
honestly rather than either over- or under-claiming.

**Suggested fix direction:** Two independent layers, either of which would
close this:
- Do not let `Control::onConnection` (or anything reachable from it) be
  reentered: track an in-progress flag and either queue/reject new
  connections' commands while a handler is running, or (better, and this
  also fixes Findings 1 and 2) move the socket handling off the GUI thread
  entirely so a modal dialog on the GUI thread cannot interleave with
  connection acceptance in the first place.
- Independently, `Document::adopt` should not publish a locked document as
  `m_doc` (or `MainWindow` should not treat `m_doc` as ready) until a
  password has actually been supplied and the document actually unlocked —
  i.e. keep the locked, pending document out of any state another code path
  (reentrant or not — e.g. a repaint, a resize, another overlay) could
  observe as "the current document."

---

## Finding 3 — Anything that wins the socket path (a pre-existing listener, or a race with another launch) silently absorbs the launch argument; the losing process gives the reader no feedback at all (CONFIRMED, empirically reproduced)

**File/line:** `src/main.cpp:30-35` (the handover branch) and
`src/control.cpp:39-56` (`Control::listen`, the probe/removeServer/listen
sequence) and `:58-69` (`Control::send`).

**Description, part A — `Control::send`'s result is discarded:**
```cpp
if (!window.listenForCommands()) {
    if (!path.isEmpty()) {
        mergen::Control::send(QStringLiteral("open ") + path);
    }
    return 0;
}
```
`Control::send` returns an empty `QString` both for "nothing is listening"
(the ordinary single-instance case, where this branch is not even taken)
*and* for "something is listening but never answered" (connect timeout
200ms, write timeout 1000ms, or **reply** timeout 1000ms). The return value
is never captured or checked. Whether the open actually reached a working
instance or not, this process prints nothing and exits 0 either way.

**Description, part B — `Control::listen`'s probe only checks that *some*
process accepts the connection, not that it is a healthy MERGEN that will
answer:**
```cpp
QLocalSocket probe;
probe.connectToServer(path);
if (probe.waitForConnected(kProbeMs)) {   // 200ms — succeeds for ANY listener
    probe.disconnectFromServer();
    return false;                         // "someone else is running"
}
QLocalServer::removeServer(path);
return m_server->listen(path);
```
A successful `connectToServer` only proves a process is listening at that
exact path — not that it is a live, responsive MERGEN. Whatever is listening
there wins the role of "the running instance" for this launch, and every
subsequent second launch, unconditionally.

**Reproduction (measured):** `rogue_listener.py` binds
`$XDG_RUNTIME_DIR/mergen-1000.sock` itself, *before* MERGEN ever runs, and
deliberately never replies to anything (standing in for: an attacker who
wins the race to the well-known path, a leftover/misbehaving unrelated
process, or — mechanically identical — a legitimate first `mergen` instance
that is still alive but wedged/unresponsive). Then launched
`mergen <repo>/annot.pdf` pointed at that runtime dir:

```
rogue listener up at .../mergen-1000.sock, mode=0o140755
accepted connection at T+0.000s, received after 0.002s: b''                                  <- the probe (connects, sends nothing, disconnects)
accepted connection at T+1.501s, received after 0.000s: b'open <repo>/annot.pdf\n' <- Control::send's actual payload
rogue listener exiting (after its own fixed run time; never replied)
```
Polling the real `mergen` process every 0.5s showed it alive at t=1.0s and
gone by t=1.5s (matches: ~probe + up to ~1000ms waiting for a reply that
never comes). `mergen`'s own stdout/stderr was **completely empty** — no
warning, no error, nothing. It exited 0. **No window ever opened. The file
the reader asked to open (`annot.pdf`) was never opened by anyone**, and the
absolute path was disclosed to whatever was squatting on the socket.

**Assessment:** §9 states "a second instance finding a live socket hands its
argument to the first and exits" — the code faithfully hands the argument
over, but never confirms the handover actually succeeded, and MZ.md does
not discuss what happens when it doesn't. This is a gap between the stated
intent (second launch raises the document in the first) and observed
behavior (second launch can silently do nothing at all, indistinguishable
from success to the reader, who just sees no window and no message).
Two realistic, non-adversarial ways this triggers with no attacker involved:
- The first instance is in the process of exiting (e.g. reader pressed
  `quit`, or it's handling `MainWindow::close()` from Finding 0/1-style
  delay) at the exact moment a second launch's probe succeeds against the
  still-open-but-dying socket.
- Any local process — not necessarily malicious, could be a stale test
  script, a misconfigured tool, or literally two copies of MERGEN's own
  socket test suite — bound to the same predictable path ahead of a real
  launch.
For a genuine cross-user attacker to plant the rogue listener, they need
write access to the directory that will hold the socket; under the intended
deployment (systemd sets `$XDG_RUNTIME_DIR` to a `0700` dir before MERGEN
ever runs — confirmed in Finding 4 below) that is not possible for another
uid, which meaningfully limits real-world exploitability of the *disclosure*
angle. The *robustness* angle (silent no-op on any same-user timing
accident) has no such precondition and is the more realistic concern.

**Severity:** High. Confirmed, deterministic reproduction of a launch that
silently does nothing — no document opens, no error is shown, and the
design's own core promise ("second instance hands over and exits, so the
reader sees their document") fails open (does nothing) rather than closed
(shows an error) or retried. The information-disclosure half (file path
sent to whatever answered the probe) is real but gated on an attacker
already having write access to the runtime directory, which is a narrower
precondition — reported with that caveat rather than overstated.

**Suggested fix direction:** Check `Control::send`'s return value in
`main.cpp`. On empty/failure, do not silently exit — fall through to
opening a normal window with the given path (treating a non-answering peer
the same as no peer at all), or at minimum print a diagnostic. Consider
also having the probe wait for a substantive reply (e.g. round-trip a
harmless no-op) rather than just a TCP/socket-level connect, so a wedged or
impostor listener is not mistaken for a healthy running instance.

---

## Finding 4 — Held protections confirmed by direct testing (not bugs; recorded because the brief asked for this explicitly)

- **Socket file permissions are exactly `0700`, owner-only, regardless of
  the containing directory.** Verified with `stat -c '%a %U:%G'` on the live
  socket special file: `700 megas:megas`. `QLocalServer::UserAccessOption`
  does what it claims. Since Unix-domain `connect(2)` honors the socket
  inode's own permission bits, this holds even if the containing directory
  were more permissive (see next point) — a non-owning uid gets `EACCES` on
  `connect()` regardless of directory mode.
- **The `QDir::tempPath()` fallback in `Control::socketPath()`
  (`control.cpp:31-37`) is very unlikely to ever execute on the deployed
  platform.** Tested directly: running `mergen` with `$XDG_RUNTIME_DIR`
  explicitly unset (`env -u XDG_RUNTIME_DIR`) did **not** produce an empty
  string from `QStandardPaths::writableLocation(RuntimeLocation)`. Instead,
  Qt 6.11.2 itself synthesizes and creates `/tmp/runtime-$USER` with mode
  `0700` (confirmed via `stat`), before MERGEN's own fallback logic is ever
  reached. So even the "no runtime dir" case lands in a private,
  non-`/tmp`-shared, correctly-permissioned location on this Qt version —
  the scenario the audit brief worried about (socket landing directly in
  shared, world-writable `/tmp`) does not reproduce on Qt 6.11.2/this Linux.
  This is a property of Qt's `QStandardPaths`, not of anything MERGEN's own
  code does, and I did not verify it holds on every Qt6 minor version — the
  explicit fallback in `control.cpp` is defensive code for a case I could
  not trigger, which is a reasonable thing to keep rather than a bug, but is
  effectively unexercised on the target platform (Arch Linux, per §3).
  **Not tested:** whether Qt's synthesized fallback dir would be reused
  as-is (without a permission/ownership check) if an attacker pre-created
  `/tmp/runtime-$USER` themselves before Qt ever ran — that is a question
  about Qt/glib's own hardening, not about MERGEN's code, and was out of
  scope for the time available.
- **A genuine race between two simultaneous real launches resolves cleanly,
  with no corruption, hang, or crash.** Launched two real `mergen`
  processes at effectively the same instant, same runtime dir, different
  files (`test.pdf` and `annot.pdf`). One (the loser) exited almost
  immediately; the other (the winner) ended up with **exactly** the loser's
  file open (verified via `/proc/<pid>/fd`, which showed only `test.pdf`
  open on the winner — originally launched with `annot.pdf` — with no
  duplicate or leaked file descriptors), and remained fully responsive to
  further control-socket commands afterward (`0.000s` round trip). This is
  the ordinary, non-adversarial version of the scenario in Finding 3, and
  unlike Finding 3's rogue-listener/non-answering case, it worked exactly as
  §9 describes: "a second instance finding a live socket hands its argument
  to the first." The difference between this success and Finding 3's
  failure is entirely about whether the winning process actually answers —
  when it does, the handover is correct; when it doesn't (or isn't there
  at all), Finding 3 applies.

- **`quit`'s reply is genuinely delivered before the process exits, with
  room to spare.** `runCommand`'s `quit` branch replies `ok` synchronously
  and only *schedules* `MainWindow::close()` via `Qt::QueuedConnection`
  (`mainwindow.cpp:790-794`), exactly as its comment claims ("Answer before
  leaving, so the caller is not left waiting on a socket that is about to
  close"). Measured directly: client received the `ok` reply 0.28ms
  after sending `quit`; the process itself did not actually disappear
  (polled via `kill(pid, 0)` at 5ms resolution) until 20.69ms after
  `quit` was sent -- 20.42ms of margin after the reply had already
  arrived. Confirmed working as designed, not a bug.

- **Parsing is robust against every malformed-input case tried; only one
  minor rough edge found.** Ran a battery of edge cases against a live
  instance (`parsing_test.py`, 21 cases, one connection each, non-destructive
  -- an earlier run of this same battery included a `quit` case that
  (correctly) terminated the instance mid-battery and produced a cascade of
  "connection refused" results for everything after it; that was a flaw in
  my *first* test script, not a MERGEN bug -- corrected and rerun cleanly
  with `quit` excluded, confirmed the instance survived the entire battery).
  Results:
  - Case-insensitive verbs confirmed (`OPEN`, `GoTo` both work) --
    `.toLower()` at `control.cpp:84` does what it claims.
  - Leading/trailing whitespace and multiple spaces between verb and
    argument are all handled correctly (`.trimmed()` on the whole line,
    then again on the split-out argument).
  - Trailing CR (`\r\n`, Windows-style line endings) is silently absorbed
    correctly, since `QChar::isSpace()` (used by `.trimmed()`) treats `\r`
    as whitespace -- a script piping CRLF-terminated input works fine.
  - Empty lines and whitespace-only lines produce a clean `err: unknown
    command ` (empty verb) -- no crash.
  - A bare verb+argument with **no trailing newline at all**, on a
    connection whose write side closes right after (EOF), is still accepted
    and processed (`readLine()` returns the buffered content at EOF even
    without a `\n`) -- more lenient than "one line per command" strictly
    implies, but not a problem in practice.
  - A NUL byte embedded in either the verb or the argument (e.g. `search
    foo\x00bar`) does **not** crash and does **not** truncate at the NUL --
    `QString` is length-prefixed, not a C string, so the byte just becomes
    literal content. Confirmed safe on a clean instance (my first attempt at
    this specific case appeared to coincide with the instance dying, which
    on investigation was the leftover `quit` from the flawed battery, not
    this input -- rerun in isolation on a fresh instance: `ok`, process
    survives).
  - Invalid UTF-8 (a lone `0xFF` byte, a stray continuation byte with no
    lead byte) does not crash -- `QString::fromUtf8` substitutes replacement
    characters and processing continues normally.
  - Valid non-ASCII UTF-8 (e.g. `café` as a search term) round-trips
    correctly.
  - Integer parsing on `goto` is robust: a non-numeric argument, a negative
    number, and a number far too large for `int` (`999999999999999999999`)
    all produce a clean `err:` reply (`toInt`'s `ok` output parameter
    correctly reports failure/overflow) rather than any wraparound or
    out-of-bounds access -- `page < 1` is checked before the page number
    ever reaches `Document`/poppler.
  - **One minor rough edge:** the verb/argument split
    (`control.cpp:83`, `line.indexOf(QLatin1Char(' '))`) matches a literal
    space only. `goto<TAB>1` is therefore parsed as one unknown verb
    (`"goto\t1"`) rather than as `goto` with argument `1`. Low severity --
    doesn't affect security, only a minor surprise for a reader whose shell
    one-liner or editor happens to insert a tab instead of a space. Given
    §3's explicit goal of being "drivable from a shell one-liner," splitting
    on a general whitespace run (e.g. a small regex) rather than a literal
    ASCII space would be marginally more forgiving, but this is a nit, not
    a defect.

- **A bare second launch with no file argument is a silent, total no-op --
  it does not even raise the existing window.** `main.cpp:30-35`: when
  `listenForCommands()` returns false (an instance is already running) and
  `path.isEmpty()` (no argv given), the code skips the `Control::send` call
  entirely and just `return 0`s. Measured: running `mergen` a second time
  with no argument while a real instance was up took 0.130s (consistent
  with just the 200ms-capped probe succeeding) and exited 0 with no output;
  the first instance's window was not raised or activated in any way
  (confirmed by contrast with the `open` case, `mainwindow.cpp:744-748`,
  which explicitly calls `raise(); activateWindow();` -- the no-argument
  path never reaches any code that could do that). Not a violation of any
  claim in MZ.md (§9 only describes handing over "its argument," which a
  no-argument launch doesn't have), but worth recording: a reader who
  launches bare `mergen` from a keybinding expecting it to at least focus
  the already-open window will find it does nothing visible at all.
  Severity: low/nit.

- **`open <path>` on the socket is exactly the same code path as opening a
  file from the keyboard (file dialog, drag-drop, recent list), by
  construction -- assessed by reading, and this is honestly not a
  socket-specific concern.** `Control::onConnection` -> `runCommand`'s
  `open` branch -> `MainWindow::openPath` (`mainwindow.cpp:744`,
  `:516-573`) is the identical function called by `chooseFile()` (file
  dialog) and drag-drop. It goes through the same `LoadStatus` handling
  (`NotFound`/`NoPermission`/`Invalid`/`NeedsPassword`), the same
  elevation-helper path for permission-denied files, and the same
  password-prompt path for encrypted documents (which is where Finding 0's
  crash lives -- that crash is reachable from the keyboard too, in
  principle, if some other code path could deliver a second command while
  the dialog is up; the socket just makes constructing that interleaving
  trivial and reliable, which is why it was found here). A large or
  malformed PDF given to `open` over the socket fails exactly the same way
  it would from the file dialog -- this was not separately fuzzed as a
  socket-specific concern since there is no socket-specific code on this
  path to distinguish it from the keyboard case; MZ.md's "the socket adds
  reach, not capability" claim holds for this command specifically, and by
  extension (per the grep-verified keyboard call sites recorded above) for
  `goto`, `search`, `next`, `prev`, and `quit` as well.

---
