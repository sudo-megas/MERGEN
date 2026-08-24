# H2 — Security audit of the privileged code path

Audit date: 2026-08-24. Branch `ata`, HEAD `472183b`.
Method: read + empirical reproduction. Nothing in the repository was modified;
all fixtures live in `/tmp/mgaudit` and `/tmp/mgqt`. No privilege was actually
escalated and no real protected system file was read.

## Scope

| Artefact | Path |
|---|---|
| Privileged helper | `<repo>/src/mergen-open.cpp` (132 lines) |
| Polkit action | `<repo>/data/mergen.policy.in` |
| Invocation | `MainWindow::readElevated`, `<repo>/src/mainwindow.cpp:674-712` |
| Re-entrancy surface | `MainWindow::openPath` + every v2.0 entry point, `src/mainwindow.cpp` |
| Build / install | `<repo>/CMakeLists.txt`, `<repo>/packaging/PKGBUILD` |

Stated model (MX.md §5, restated in MZ.md §6 and §9): one absolute path in;
validation happens on the descriptor, never the name; anything that is not a
regular file beginning `%PDF-` is refused; the action therefore cannot be turned
into a general-purpose read of privileged files; never setuid; the viewer never
runs privileged.

Findings are appended below in the order they were confirmed. A severity-ordered
index is at the end of the file.

---

## CONFIRMED-HOLDS — protections that genuinely work

Recorded first because a privileged helper is exactly where false confidence is
dangerous, and confirming the model is worth as much as breaking it.

### H-1. The descriptor, not the name, is validated. No TOCTOU inside the helper.
`src/mergen-open.cpp:65-128`. `::open()` is called exactly once; `fstat`,
`S_ISREG`, the size test, the magic read and the whole bulk read all operate on
that same `fd`. The path string is never used again after line 65. There is no
re-open, no `stat(path)`, no second `open`. A symlink swapped after line 65
cannot affect anything — the descriptor is already bound to an inode.

**Verdict: the claim in MX.md §5 ("only once it holds the file descriptor — not
before, so the name cannot be swapped underneath it") is accurate.**

### H-2. Non-regular files are refused. Confirmed by running.
```
directory /                     rc=1  stdout=0  stderr=not a regular file
directory (own)                 rc=1  stdout=0  stderr=not a regular file
/dev/zero                       rc=1  stdout=0  stderr=not a regular file
/dev/null                       rc=1  stdout=0  stderr=not a regular file
symlink -> /dev/zero            rc=1  stdout=0  stderr=not a regular file
/proc/self/fd/0 (stdin = pipe)  rc=1  stdout=0  stderr=not a regular file
```
Directories, character devices, block devices and FIFOs all fail `S_ISREG`.
`/dev/zero` reached through a symlink is refused too, so "read /dev/zero into
the viewer's memory" (attack item 4) does **not** work.

`/proc/self/fd/N` and `/dev/stdin` *are* followed, but they resolve to whatever
the caller already had open. Under `readElevated` those are `QProcess` pipes, so
they are refused as non-regular (reproduced above). When invoked by hand with a
regular file on stdin they read that file — a file the caller had already opened
themselves, so no privilege is gained.

### H-3. Argument handling is correct on every case tested.
```
no args                    rc=1  "expects exactly one argument: an absolute path to a PDF"
two args                   rc=1  same
empty string arg           rc=1  "path must be absolute"
relative path "real.pdf"   rc=1  "path must be absolute"
"./real.pdf"               rc=1  "path must be absolute"
"--help"                   rc=1  "path must be absolute"
5000-char path             rc=1  "File name too long"   (ENAMETOOLONG from open)
nonexistent                rc=1  "No such file or directory"
path with embedded \n      rc=0  (opened correctly; a newline is a legal byte in a path)
```
`argc != 2` is rejected, so no extra arguments are accepted. The absolute-path
requirement is *load-bearing* and correctly enforced: `man pkexec` states
"pkexec will run PROGRAM in username's home directory", i.e. the helper's CWD is
`/root`, so a relative path would resolve against root's home. Refusing it is
right. A NUL cannot appear in `argv` at all (execve terminates on it), so that
case is not reachable. Any option-looking argument is rejected by the same
`path[0] != '/'` test, and the helper parses no options of its own.

### H-4. Not setuid; ownership and mode are correct; the compiled-in path matches the install path.
```
<repo>/build/mergen-open  755 megas:megas   (no setuid bit)
/usr/lib/mergen/mergen-open           755 root:root     (no setuid bit)
```
`CMakeLists.txt:76` builds it as a plain executable with no `PERMISSIONS`
override; `CMakeLists.txt:83` installs it to `${CMAKE_INSTALL_LIBDIR}/mergen`
with default 755. `MERGEN_HELPER_PATH` (`CMakeLists.txt:38`) is
`${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open`, which for the shipped
configuration (`PKGBUILD` sets `-DCMAKE_INSTALL_PREFIX=/usr
-DCMAKE_INSTALL_LIBDIR=lib`) resolves to `/usr/lib/mergen/mergen-open`.
Verified end to end on this machine:

- `build/CMakeCache.txt`: `MERGEN_HELPER_PATH:STRING=/usr/lib/mergen/mergen-open`
- `build/io.github.sudomegas.mergen.policy`: `<annotate key="org.freedesktop.policykit.exec.path">/usr/lib/mergen/mergen-open</annotate>`
- `build/install_manifest.txt`: `/usr/lib/mergen/mergen-open`
- actual file on disk at that path, root:root 755.

All four agree. The policy is installed root:root 644 under
`/usr/share/polkit-1/actions/`. **Attack item 8 finds nothing.**

### H-5. `auth_admin` on all three slots is the correct choice, and it is what makes pkexec's own caveat harmless.
`data/mergen.policy.in:19-21`. `man pkexec` warns: "pkexec does no validation of
the ARGUMENTS passed to PROGRAM. In the normal case (where administrator
authentication is required every time pkexec is used), this is not a problem
since if the user is an administrator he might as well just run `pkexec bash`...
However, if an action is used for which the user can retain authorization (or if
the user is implicitly authorized) this could be a security hole."

The policy uses `auth_admin`, **not** `auth_admin_keep`, so no authorisation is
ever retained, and not `yes`/`auth_self`, so nobody is implicitly authorised.
This is precisely the configuration under which pkexec says the lack of argument
validation is not a problem. Nothing weakens it in the tree.

### H-6. The action cannot be aimed at a different binary.
`org.freedesktop.policykit.exec.path` pins the action to one absolute path.
`pkexec` selects an action by the resolved program path, so invoking pkexec on
any other program does not select this action — it falls through to
`org.freedesktop.policykit.exec`, which also requires `auth_admin`. There is no
`org.freedesktop.policykit.exec.argv1` annotation, but none is needed: the action
already names the exact program, and argv1 is the reader's chosen document.
Because the helper is root-owned and not world-writable, the pinned path cannot
be swapped.

### H-7. `readElevated` uses the argument-vector API. No shell anywhere.
`src/mainwindow.cpp:677-681`:
```cpp
QProcess pkexec;
pkexec.setProgram(QStringLiteral("pkexec"));
pkexec.setArguments({QStringLiteral(MERGEN_HELPER_PATH), path});
```
`setProgram` + `setArguments` + `start()` is `fork`/`execve` with an explicit
argv — no `/bin/sh -c`, no `startCommand()`, no string concatenation. Grepped
the whole tree: `QProcess` appears only here (`mainwindow.cpp:51` include, and
this function); there is no `system()`, `popen()`, `exec*p` or
`QProcess::startCommand` call anywhere in `src/`. A path containing spaces,
quotes, `;`, `$()` or newlines is passed as one opaque argv element and cannot
break out. **Attack item 6's shell-injection half: clean.**

Because `MERGEN_HELPER_PATH` is compiled in and begins with `/`, it is always
pkexec's `PROGRAM` positional argument and the reader-supplied path always lands
after it — so a reader-chosen path can never be consumed as a pkexec *option*.

### H-8. Large stdout/stderr do not deadlock the pipe, and stdout is not truncated.
This is the other half of attack item 6, and it holds. `SeparateChannels` keeps
the two pipes distinct, and `QProcess`'s read buffer is unlimited by default, so
as long as an event loop is spinning — which is exactly what the nested
`loop.exec()` provides — both pipes are drained by socket notifiers. Verified by
building a harness (`/tmp/mgqt/t.cpp`) that replicates `readElevated`'s exact
structure:

```
big-stdout(200MB): got=209715200 code=0 status=0
big-stderr(50MB)+small-stdout: stdout=4 code=0 status=0 (no deadlock)
```
200 MB of stdout arrives complete; 50 MB of stderr written before stdout does
not wedge the child. **No deadlock, no truncation.**

### H-9. No re-open, no `PATH`-relative helper, no shell in the policy path.
`MERGEN_HELPER_PATH` is a compile-time string literal (`CMakeLists.txt:38`, injected at `CMakeLists.txt:60`), not
read from config, environment or disk. The helper cannot be redirected at
runtime by anything short of editing the binary.

### H-10. Passwords and elevated bytes are not written to disk.
`promptForPassword` (`src/mainwindow.cpp:653-670`) zeroes both the `QString` and
the `QByteArray` after each attempt. Elevated bytes go to
`Document::openData`, never to a file. `recent.toml` stores only the path.
`redactSelection` (`src/mainwindow.cpp:948-953`) explicitly refuses to run on a
document that arrived through the helper (`!m_doc->data().isEmpty()`), matching
MZ.md §9. All as documented.

---

## FINDINGS

Severity-ordered index is at the end. Each finding says whether it was
**CONFIRMED by running something** or **assessed by reading**.

---

### F-1 — `readElevated` treats a still-running pkexec as a successful read, and accepts a truncated document
**Severity: HIGH** (correctness/integrity of the privileged read; not a
privilege boundary break)
**CONFIRMED by running** — `/tmp/mgqt/t2.cpp`

`src/mainwindow.cpp:688-712`:
```cpp
if (pkexec.state() != QProcess::NotRunning) {
    QEventLoop loop;
    connect(&pkexec, &QProcess::finished, &loop, &QEventLoop::quit);
    loop.exec();                       // <-- can return WITHOUT finished()
}
const int code = pkexec.exitCode();    // 0 on a process that never finished
if (pkexec.exitStatus() != QProcess::NormalExit || code != 0) { ...error... }
QByteArray bytes = pkexec.readAllStandardOutput();   // partial stream
```

`loop.exec()` has exactly two ways out: `QProcess::finished`, or somebody
calling `QCoreApplication::exit()/quit()`. The second happens routinely — Qt's
`QCoreApplication::exit()` walks `threadData->eventLoops` and exits **every**
loop on the stack, nested ones included. Three live UI paths reach it while the
polkit dialog is up:

| Trigger | Route |
|---|---|
| `quit` over the control socket | `runCommand` → `QMetaObject::invokeMethod(this, &MainWindow::close, Qt::QueuedConnection)` (`src/mainwindow.cpp:790-793`) — a queued call, delivered by whichever loop is spinning, i.e. the nested one |
| <kbd>Ctrl</kbd>+<kbd>Q</kbd> | `m_quitAction` → `MainWindow::close` (`src/mainwindow.cpp:438-441`) |
| Compositor close button | `MainWindow::close` |

`close()` on the last window makes Qt call `QCoreApplication::quit()`.

When the loop returns early the process is still alive, and **`QProcess::exitCode()`
returns 0 and `exitStatus()` returns `NormalExit` for a process that has not
finished** — so the guard on line 697 does not fire and the code falls straight
through to `readAllStandardOutput()`.

Reproduction, with an outer `app.exec()` running exactly as MERGEN has
(Qt 6.11.2):
```
### A: quit() at 300ms, child sleeps 3s
  -> QCoreApplication::quit() fired
  nested loop returned. stillRunning=1 exitCode=0 exitStatus=0 bytes=0
  readElevated verdict: SUCCESS (bytes accepted) ; bytes.isEmpty()=true

### B: child streams a document slowly, quit() at 300ms
  -> QCoreApplication::quit() fired
  nested loop returned. stillRunning=1 exitCode=0 exitStatus=0 bytes=69 head=[%PDF-1.7 AAAAAAA]
  readElevated verdict: SUCCESS (bytes accepted) ; bytes.isEmpty()=false
```

**Confirmed again against the real `mergen` binary**, using a `PATH`-planted
stand-in for pkexec (F-9) that streams a document over ~6 s. With the window
visible and `app.exec()` running, `open` was issued over the control socket
(entering `readElevated`'s nested loop for real), and `quit` was sent 1.5 s in:
```
  (1.5s in: helper still streaming) sending quit...
  [quit] reply: 'ok'
  [open] reply: 'err: could not open /tmp/mgaudit/noperm.pdf'
  mergen gone after 1.56s   (helper streams for ~6.0s)
```
The process left at 1.56 s of a 6 s transfer, so the nested loop **did** return
while the helper was still running. The `open` reply proves the code went down
the *success* branch and handed the partial buffer to `Document::openData` — the
only reason the reader was not shown a truncated document is that poppler
rejected a 400-byte fragment. A larger prefix, or a linearised document, would
not have been rejected.

Case A is benign only by accident: `bytes.isEmpty()` catches it and an error is
shown. **Case B is the bug** — 69 bytes of a partially streamed document are
returned as a *successful* elevated read. `openPath` then calls
`m_doc->openData(bytes, ...)` on a truncated PDF. Poppler reconstructs a missing
xref by scanning, so a truncated PDF frequently *does* load and render its
first pages, with nothing anywhere telling the reader the document is
incomplete. For a viewer whose stated purpose is reading a PDF faithfully,
silently displaying a prefix of a document as though it were the document is a
real integrity failure — and it is the *privileged* read, the one case where
the reader has least ability to check the file themselves.

**Who triggers it:** the reader themselves (Ctrl+Q or the close button while the
password prompt is up — a very natural "oh, cancel that" reflex), or anything
with access to the control socket issuing `quit`. No attacker required.

**Fix direction:** treat an early loop exit as failure. Record whether
`finished` actually fired (a `bool done = false;` set in the connect lambda) and
bail unless it did; or use `pkexec.waitForFinished(-1)` guarded by a watchdog;
or, better, check `state() == QProcess::NotRunning` before trusting
`exitCode()`. Additionally, `readElevated` should verify the returned bytes are
a plausible whole document rather than any non-empty buffer.

---

### F-2 — `~QProcess` runs with a live root child on that same path: `kill()` cannot work and the GUI thread blocks
**Severity: MEDIUM**
**CONFIRMED by running** (the destructor path); **assessed by reading** (the
root-child half, which cannot be reproduced without escalating)

Continuing F-1: after the early return, `pkexec` is a stack local in
`readElevated` and its destructor runs immediately. Qt's `~QProcess` does:
```cpp
if (d->processState != NotRunning) {
    qWarning(... "Destroyed while process is still running.");
    kill();
    waitForFinished();      // default 30 000 ms
}
```
**Confirmed by running** that Qt 6.11's destructor really does kill and wait: a
`QProcess` holding a live 10-second child was let fall out of scope, and the
child was dead before it managed its first write, with the destructor returning
in ~0 ms because `kill()` succeeded.
```
child pid=143630 state=2
destructor took 0 ms
  pid 143630 gone -> destructor killed the child
```
(No "Destroyed while process is still running" warning appeared on stderr in this
Qt version, so do not expect one in the logs.)

The real child is `pkexec`. Per kill(2), the sender needs a real/effective UID
matching the target's real or saved-set UID. Two phases:

- **Auth dialog still up:** pkexec has ruid = the reader, so `kill()` succeeds.
  Harmless.
- **After authorisation, while the helper streams as root:** pkexec has
  ruid = suid = 0. `kill()` returns `EPERM`, so `waitForFinished()` blocks the
  **GUI thread for up to 30 seconds**, and a `qWarning` is printed. The reader
  asked the application to quit and it appears to hang.

Two nuances that narrow this correctly rather than overstating it:
- Pre-authorisation, the destructor's kill is the *right* behaviour — abandoning
  the attempt tears down pkexec and its dialog cleanly.
- The destructor also closes the stdout pipe, so a root helper that is actively
  *writing* will take `EPIPE`/`SIGPIPE` and exit on its own. The genuine orphan
  case is therefore a helper blocked in `open()` or `read()` and not yet writing
  — i.e. exactly F-4.

**Fix direction:** never let the `QProcess` fall out of scope while running —
either wait for it properly, or `setProcessState`/detach deliberately, or move
the whole invocation behind an object with an explicit teardown that does not
rely on the destructor's kill-and-wait.

---

### F-3 — The `%PDF-` gate constrains five bytes and nothing else; there is no bound on what follows
**Severity: MEDIUM** (defence-in-depth, not a boundary — see the framing note)
**CONFIRMED by running**

`src/mergen-open.cpp:85-128`. The entire content test is `memcmp(magic, "%PDF-", 5)`.
Nothing checks a version digit, a `%%EOF`, a trailer, an xref, or that the byte
count is plausible. Once those five bytes match, the file is emitted verbatim to
completion.

```
exactly 5 magic bytes  ("%PDF-")                    rc=0  stdout=5   accepted
polyglot ("%PDF-\nSECRET-CONTENT-AFTER-MAGIC\n")    rc=0  stdout=33  accepted, in full
```

**And even those five bytes do not constrain the output.** The check reads the
magic from the fd, re-emits it (`src/mergen-open.cpp:106`), then streams the rest
from the *current offset* — so content written into the same inode after the
magic read is emitted verbatim. Demonstrated by rewriting the middle of an 8 MB
file while the helper was reading it:
```
  emitted bytes: 8388613
  output starts with %PDF- : True
  swapped marker present in the emitted stream : True
```
The answer to "does `%PDF-` at offset 0 constrain anything meaningful about what
gets emitted?" is therefore: **it constrains exactly the five bytes at the head of
the output stream, and nothing else.** (This particular race is not itself a
leak — the attacker must be able to *write* the swapped bytes, so they already
have them. It is proof that the gate is positional, not semantic.)

So the real guarantee is not "the helper reads a PDF" but **"the helper reads
any root-only file whose first five bytes are `%PDF-`, in its entirety"**. The
header comment at `src/mergen-open.cpp:8-14` and MX.md §5's "reading a PDF is
the only thing the helper can be made to do" both overstate this.

Supporting fact worth stating plainly: **root-only-readable does not imply
"genuinely a PDF", and an unprivileged local user can manufacture regular files
that are root-only-readable with first bytes of their own choosing** — the
standard example being `/proc/<pid>/environ` and `/proc/<pid>/cmdline` of a
setuid process the attacker launched, which the kernel re-owns to root:root 0400
while the *content* stays caller-supplied. (I confirmed the shape of these files
— regular, `st_size` 0, mode 0400 — but did not land a live setuid process to
photograph, so treat that specific instance as reasoned rather than
demonstrated.) The class is real; what makes it hard to weaponise is that you
also need secret data *after* the attacker-controlled head, which is uncommon.

**Framing, honestly:** because the action is `auth_admin` every time (H-5),
anybody who can get past the prompt could equally run `pkexec bash`. So the gate
is not today a privilege boundary — it is defence-in-depth protecting against
(a) a future relaxation of the policy to `auth_admin_keep`/`auth_self`, and
(b) an administrator who authenticates believing they are releasing exactly one
PDF. **(b) is the case that actually matters**, and it is undermined by F-8.
Severity is medium rather than low because the code and the constitution both
claim a guarantee the code does not provide, and future changes will be made on
the strength of that claim.

**Fix direction:** if the gate is meant to mean something, make it mean
something — require `%PDF-` followed by a version digit, and require a `%%EOF`
within the last kilobyte of the file (both cheap on an already-open fd). Failing
that, amend the comment and MX.md §5 to say what is actually enforced.

---

### F-4 — `open()` and `read()` in the helper can block forever, and neither side has a timeout: a wedged root process and a permanently stuck `m_opening`
**Severity: MEDIUM**
**CONFIRMED by running** (the blocking open)

`src/mergen-open.cpp:65` uses `O_RDONLY | O_CLOEXEC` with no `O_NONBLOCK`. On a
FIFO, `open(O_RDONLY)` blocks until a writer appears — **before** the
`S_ISREG` check at line 76 can reject it:
```
--- FIFO: does open() block before fstat? (5s timeout) ---
rc=124 (124 == timed out => blocked in open())
```
The S_ISREG check is therefore correct but unreachable in the one case where the
damage is done in `open()` itself.

`readElevated` has no timeout to match: `loop.exec()` (`src/mainwindow.cpp:693`)
waits forever, and there is no cancel button, no watchdog and no way for the
reader to abandon the attempt. Consequences of one wedged call:

1. A root process is parked indefinitely.
2. `m_opening` (`src/mainwindow.cpp:522`) stays `true` **for the rest of the
   session**, so *every* subsequent open — drop, `open` over the socket, recent
   files, the file dialog, portal-follow — silently does nothing, with no
   message (see F-7).
3. Quitting hits F-2's 30-second block and then orphans the root process.

**Reachability.** `Document::openPath` (`src/document.cpp:22-27`) filters with
`QFileInfo::isFile()`, which is false for a FIFO, so a FIFO named directly never
reaches the helper. The reachable routes are:
- **TOCTOU between MERGEN's check and root's open** (see F-8): another local
  user who owns a directory on the path replaces the regular file with a FIFO in
  the window between `QFileInfo::isFile()` and the helper's `open()`. The window
  is wide — it spans the whole polkit authentication dialog.
- A regular file on a hung NFS mount, or on a FUSE filesystem the reader
  themselves mounted, where `open()`/`read()` never returns.

**Fix direction:** open with `O_NONBLOCK|O_NOFOLLOW`-style care — specifically
`O_RDONLY|O_NONBLOCK|O_CLOEXEC`, `fstat`, reject non-regular, then clear
`O_NONBLOCK` with `fcntl` (regular files ignore `O_NONBLOCK` for reads, so this
costs nothing and closes the FIFO hang). On the viewer side, give `loop.exec()`
a watchdog timer and kill the child on expiry, and clear `m_opening` on every
exit path (it already is, via `QScopeGuard` — the problem is that the scope
never ends).

---

### F-5 — The 2 GiB cap is advisory: it is checked against `st_size` and never enforced during the read
**Severity: MEDIUM**
**CONFIRMED by running**

`src/mergen-open.cpp:80-83` tests `st.st_size > kMaxBytes` once. The bulk loop at
lines 111-128 then reads to EOF with **no byte counter at all**. `st_size` is a
snapshot and, for some regular files, a fiction.

Reproduction — a file that was 5 bytes at `fstat` time and grew afterwards:
```
st_size at start was ~5 bytes; helper emitted 700005 bytes (final file: 20000005)
```
140 000× over the size the cap was applied to. The cap bounds nothing.

The cap boundary itself is exact and the honest cases are handled correctly —
worth saying, because it means the bug is precision, not arithmetic:
```
100 GB sparse file, valid %PDF- header  -> "mergen-open: file is too large"   rc=1
st_size 2147483649 (cap + 1)            -> "mergen-open: file is too large"   rc=1
st_size 2147483647 (cap - 1)            -> accepted, emitted 2147483647 bytes
```
That last line is not reassuring: a file one byte under the cap streams a full
2 GiB into the viewer, which is F-10's measurement scaled up four-fold, and no
part of the pipeline objects.

Two ways this bites:
- **Growing file.** Any regular file being appended to while the helper reads
  it (a log, a download in progress, an attacker's file in a directory they own)
  emits far more than `st_size`.
- **`procfs`/`sysfs`.** These are regular files that report `st_size == 0`, so
  they pass the cap unconditionally. Confirmed:
  `/proc/self/environ`, `/proc/self/status`, `/proc/self/cmdline` all
  `type=regular size=0`, and the helper gets past `S_ISREG` and the size test on
  them, failing only at the magic check.

Everything the helper emits is buffered whole in the *unprivileged viewer's*
memory: `QProcess`'s read buffer is unlimited by default, then
`readAllStandardOutput()` allocates a second contiguous copy, then
`Document::openData` keeps it in `m_data` **and** `Poppler::Document::loadFromData`
takes its own. A 2 GiB document therefore costs roughly 6 GiB of RSS, and an
unbounded stream costs whatever the OOM killer allows. There is no size check
anywhere in `readElevated` or `openPath`.

The comment at `src/mergen-open.cpp:27-28` — "small enough that a device file
mistaken for a document cannot exhaust memory downstream" — describes an
intention the code does not implement.

**Fix direction:** count bytes in the read loop and abort past `kMaxBytes`,
rather than trusting `st_size`. Mirror the cap on the viewer side with
`QProcess::setReadBufferSize()` plus an explicit check before
`readAllStandardOutput()`.

---

### F-6 — **CRASH.** The control socket segfaults the viewer while `openPath` sits in its password nested loop
**Severity: HIGH**
**CONFIRMED by running — reproducible SIGSEGV with a backtrace**

This is the concrete answer to attack item 7. The `m_opening` guard covers
re-entry into `openPath`; it does not cover the *rest* of `MainWindow`, and the
control socket reaches straight past it into a document that is not in a usable
state.

**Reproduction** (`XDG_RUNTIME_DIR=/tmp/mgrt`, `QT_QPA_PLATFORM=offscreen`, a
256-bit encrypted PDF made with `qpdf --encrypt`):
```
$ mergen /tmp/mgaudit/locked.pdf &          # blocks in promptForPassword()
$ printf 'goto 1\n' | nc -U $XDG_RUNTIME_DIR/mergen-1000.sock
reply: b''
Segmentation fault (core dumped)            # EXIT_STATUS=139
```
Reproduced on every attempt — `systemd-coredump` recorded 7 `mergen` SIGSEGV
cores over the course of this audit. `goto 1` and `goto 99999` both crash;
`next`, `bogus`, `search`, `open` and `quit` do not.

**Backtrace** (gdb, unstripped build):
```
#0  pthread_mutex_lock ()                      from libc.so.6
#1  Catalog::getNumPages ()                    from libpoppler.so.163
#2  mergen::MainWindow::runCommand(QString const&, QString const&)
#3  std::_Function_handler<...>::_M_invoke      (the Control handler lambda)
#4  mergen::Control::onConnection()
...
#16 QEventLoop::exec()
#17 QDialog::exec()
#18 QInputDialog::getText(...)
#19 mergen::MainWindow::promptForPassword(QString const&)
#20 mergen::MainWindow::openPath(QString const&)
#21 main()
```

**Root cause.** `Document::isOpen()` is `return m_doc != nullptr;`
(`src/document.h:102`). `Document::adopt` deliberately *keeps* the poppler
handle in the `NeedsPassword` state so the caller can retry the password
(`src/document.cpp:66-71`). So during the prompt, `isOpen()` reports **true**
for a document whose `Catalog` has never been built, because the file is still
encrypted.

`runCommand`'s `goto` branch (`src/mainwindow.cpp:751-764`) gates on exactly
that:
```cpp
if (!m_doc->isOpen()) { return err("no document"); }
if (page < 1 || page > m_doc->pageCount()) { ... }   // <-- Catalog::getNumPages() on a locked doc
```
`Document::pageCount()` is `m_doc ? m_doc->numPages() : 0`
(`src/document.cpp:115-117`), and `Poppler::Document::numPages()` →
`Catalog::getNumPages()` dereferences a catalog that does not exist yet. Null
deref inside `pthread_mutex_lock`.

**Who could realistically trigger it.** Anything that can write one line to
`$XDG_RUNTIME_DIR/mergen-$UID.sock` while a password prompt is up. In the
documented workflow that is the reader's own scripts and keybindings — MZ.md §9
exists precisely to encourage `mergen`-driving keybindings — so an ordinary
`mergen goto` binding fired by muscle memory during a password prompt kills the
window and loses the session. It is not a *privilege* escalation (the socket is
`srwx------`, 0700, verified), but it is an unauthenticated-from-the-app's-view
remote-ish crash in the same function the privileged path runs through.

**Why it matters to the privileged path specifically.** The elevation path leads
*into* this state: a root-owned **and** password-protected PDF goes
`openPath` → `NoPermission` → `readElevated` → bytes → `openData` →
`NeedsPassword` → `promptForPassword`. From that moment the process is one
`goto` away from a segfault, with the root-owned document's plaintext bytes
sitting in `m_data` and now in a core dump (see F-11).

I verified the *elevation* nested loop itself is not crashable this way:
`Document::openPath` returns `NoPermission` at `src/document.cpp:26-27` before
touching `m_doc`, so during `readElevated` `m_doc` is either closed
(`isOpen()` false → `err: no document`) or the *previous*, fully-unlocked
document (`goto` works, but scrolls the wrong document — see F-7). The crash is
the password loop's; the two loops are siblings in the same function and MX.md
§8 treats them as one case ("Authentication and password prompts both run nested
event loops").

**Fix direction:** `isOpen()` must not claim a locked document is open — either
split the state (`isOpen()` vs `isReady()`) or have every `runCommand` branch
test readiness rather than mere handle-existence. Belt and braces: make
`Document::pageCount()` return 0 while `m_doc->isLocked()`.

---

### F-7 — `runCommand("open", …)` reports `ok` when the `m_opening` guard silently discarded the request
**Severity: LOW**
**CONFIRMED by running**

`src/mainwindow.cpp:740-747`:
```cpp
openPath(argument);          // no-op when m_opening is true
raise(); activateWindow();
return m_doc->isOpen() ? ok : err("could not open " + argument);
```
The reply describes *whether a document is open*, not whether **this** request
did anything. Reproduced against a viewer parked in the password prompt:
```
open /tmp/mgaudit/plain.pdf      reply='ok'      (nothing was opened)
```
The reader's script is told the file is on screen when a completely different
document is. Confirmed a second time inside the **elevation** loop specifically
(document already open, `readElevated` mid-transfer, non-modal, UI fully live):
```
    goto 1                      -> 'ok'                 (scrolls the OLD document)
    goto 99999                  -> 'err: no page 99999' (bounded by the OLD document)
    open /tmp/mgaudit/plain.pdf -> 'ok'                 (guard swallowed it; nothing happened)
    alive mid-elevation? YES-no-crash
```
This run also verifies F-6's scoping empirically: during the *elevation* loop
`m_doc` holds the previous, fully-unlocked document, so `goto` is safe there and
does not crash. The crash belongs to the password loop alone. The same silence affects the other guarded entry points, which have
no reply channel at all and so give no feedback whatsoever:

| Entry point | Line | Behaviour during a nested loop |
|---|---|---|
| `dropEvent` | `src/mainwindow.cpp:1191-1198` | `openPath` returns immediately; drop vanishes with no message |
| recent-files menu | `src/mainwindow.cpp:1889` | silently nothing |
| `chooseFile` (toolbar Open, Ctrl+O) | `src/mainwindow.cpp:595-606` | file dialog runs, selection silently discarded |
| `reloadDocument` | `src/mainwindow.cpp:608-615` | silently nothing |
| `followPortal` | `src/mainwindow.cpp:929-933` | `openPath` no-ops, then `m_view->scrollToPage(target)` runs anyway — jumps the **current** document to a page index belonging to the portal's *other* document |
| `showPortals` list activation | `src/mainwindow.cpp:1012-1018` | same, and without even the `m_doc->isOpen()` test `followPortal` has |

The two portal routes are the sharpest: they act on a page number from a
document that was never loaded. I checked `PageView::scrollToPage`
(`src/pageview.cpp:189-197`) — it is bounds-checked against `m_layout.size()`
and returns early, so this is a wrong-place jump, **not** an out-of-range access.
No memory-safety issue there.

**Fix direction:** have `openPath` return a status (or set a flag) so callers can
tell "refused because busy" from "opened". At minimum, show the reader a notice
instead of dropping the request on the floor.

---

### F-8 — Compare mode is not covered by the guard, and `PageView::setDocument` never clears the diff bands
**Severity: LOW** (state corruption, visual; **not** memory-unsafe)
**Assessed by reading** — I could not drive the file dialog to reproduce it end
to end, so this is a read finding, stated as such.

`enterCompare` (`src/mainwindow.cpp:1040-1075`) does **not** consult `m_opening`;
it calls `Document::openPath` on a *separate* `m_compareDoc`, so the guard was
never in its way. It is reachable during the elevation nested loop because
`m_doc->isOpen()` is still true for the previous document (see F-6's analysis of
`src/document.cpp:26-27`). Sequence:

1. Document A is open. The reader opens root-owned B; the polkit dialog goes up.
2. During the nested loop, <kbd>Ctrl</kbd>+<kbd>D</kbd> → `chooseComparison` →
   another nested loop (file dialog) → `enterCompare(C)`. Compare mode is now
   live: A vs C, with `m_view`'s diff bands computed for A's pages and the two
   scrollbars cross-bound.
3. The reader authenticates. `openPath` resumes and runs
   `m_view->setDocument(m_doc.get())` (`src/mainwindow.cpp:568`) — the left view
   is now **B**.

`PageView::setDocument` (`src/pageview.cpp:112-143`) clears `m_cache`, `m_words`,
`m_links`, `m_hits`, the selection, zoom, rotation and `m_pageSizes` — but **not
`m_diffBands`**. B is therefore painted with A's difference marks, scroll-locked
to C, and `isComparing()` is still true. Nothing tells the reader the marks refer
to a comparison that no longer exists — in a feature whose whole output is "this
region changed", that is the worst possible failure mode.

`paintDiffBands` (`src/pageview.cpp:387-398`) indexes `m_layout.at(page)` with a
page that is already known to be inside the layout, and `m_diffBands` is a
`QHash` lookup, so there is **no out-of-bounds access and no double-free**. I
looked for one and it is not there.

Note this is *not* specific to elevation: clicking toolbar Open while comparing
does the same thing, because `m_openAction` is never disabled
(`updateActionStates`, `src/mainwindow.cpp:485-503`, disables fit/rotate/search/
print/zoom but never Open) and nothing calls `leaveCompare()` on a new document.
The nested loop only makes it easier to reach.

**Fix direction:** clear `m_diffBands` in `PageView::setDocument`, and call
`leaveCompare()` at the top of a successful `openPath`.

---

### F-9 — `pkexec` is resolved through `$PATH`, so anything that can set the viewer's environment can substitute the entire privileged read — with no prompt at all
**Severity: MEDIUM**
**CONFIRMED by running — a planted `pkexec` was used in preference to the real one**

`src/mainwindow.cpp:678`:
```cpp
pkexec.setProgram(QStringLiteral("pkexec"));
```
A `QProcess` program with no `/` is looked up on `PATH`. The helper path beside
it is absolute and compile-time (H-9) — the *trusted* half is nailed down and the
half that provides the trust is not.

**Reproduction.** A shim at `/tmp/mgfake/pkexec` that ignores its arguments and
prints a document of its own choosing, with `PATH=/tmp/mgfake:$PATH`:
```
$ mergen &                        # real binary, offscreen, own runtime dir
$ printf 'open /tmp/mgaudit/noperm.pdf\n' | socket…
  open noperm.pdf -> 'ok'
  goto 1          -> 'ok'
$ cat /tmp/mgaudit/fake.log
FAKE-PKEXEC INVOKED argv: /usr/lib/mergen/mergen-open /tmp/mgaudit/noperm.pdf
```
The shim ran, MERGEN accepted its bytes, opened them as the document and reported
success — **no authentication prompt of any kind occurred**, and nothing in the
UI distinguishes this from a genuine authenticated privileged read.

**This is not a privilege escalation.** A user who controls their own `PATH`
already controls their own processes; nothing here gets an attacker root. What it
breaks is the *integrity of the story MERGEN tells the reader*. MX.md §5 says
"the authentication prompt belongs to the polkit agent, not to MERGEN" — but
MERGEN has no way to know a prompt ever happened, and will present substituted
content as though root had vouched for it. Realistic routes: a wrapper script or
`.desktop` `Exec=` line, a `~/.local/bin` or `~/bin` entry that precedes
`/usr/bin` in `PATH` (common on Arch), or any earlier compromise of a dotfile.

**Fix direction:** `setProgram(QStringLiteral("/usr/bin/pkexec"))`, or make it a
compile-time constant next to `MERGEN_HELPER_PATH` and have CMake find it.

---

### F-10 — The viewer buffers the whole elevated stream, unvalidated, with no cap: 500 MB in, 1 GB of RSS, before anything is checked
**Severity: MEDIUM**
**CONFIRMED by running** — direct confirmation of F-5's downstream half

Measured against the real `mergen` binary, with the fake helper emitting
`%PDF-1.7\n` followed by 500 MB:
```
baseline VmRSS      :   57 MB
peak VmRSS / VmHWM  : 1059 MB      (machine has 7119 MB total)
[open] reply        : 'err: could not open /tmp/mgaudit/noperm.pdf'
mergen stderr       : "Error: Couldn't find trailer dictionary" / "Couldn't read xref table"
```
Roughly **2× the stream size**, and every byte of it is buffered *before* poppler
is given the chance to reject the document — the rejection above happened after
the peak, not instead of it. `readElevated` (`src/mainwindow.cpp:709-712`) has no
`setReadBufferSize`, no length check, and no incremental parse; it calls
`readAllStandardOutput()` on whatever arrived. The amplification is
`QProcess`'s ring buffer + the contiguous `QByteArray` from `readAllStandardOutput`
+ `Document::m_data` (`src/document.cpp:58`, implicitly shared, so free) +
poppler's own copy inside `loadFromData`.

Scaled to the helper's *nominal* 2 GiB cap that is ~4 GB of RSS; and since F-5
shows the cap does not actually bound the stream, the real bound is the OOM
killer. `src/mergen-open.cpp:27-28` claims the constant exists "so that a device
file mistaken for a document cannot exhaust memory downstream" — downstream
exhaustion is exactly what was measured.

**Fix direction:** cap on both sides. In the helper, count bytes actually read.
In `readElevated`, `setReadBufferSize()` and abandon the transfer past the cap
rather than discovering the size after it is resident.

---

### F-11 — **The privileged document's plaintext is written to disk in a core dump.** Full chain demonstrated end to end
**Severity: HIGH**
**CONFIRMED by running — marker bytes recovered from the stored core**

F-6 gives a reachable crash. This is what that crash costs when it happens on the
elevation path, and the two compose into a complete leak of the thing the whole
feature exists to protect.

**The chain, executed against the real `mergen` binary:**

1. `mergen /tmp/mgaudit/noperm.pdf` — an existing file the reader cannot read
   (mode `000`, `os.access(R_OK)` → `False`). `Document::openPath` returns
   `NoPermission` (`src/document.cpp:26-27`).
2. `readElevated` runs the helper and returns the document's bytes.
3. The document is password-protected, so `openData` returns `NeedsPassword` —
   and `src/document.cpp:56-59` stores the bytes: `m_data = bytes`. The full
   plaintext of the privileged document is now resident.
4. `promptForPassword` opens its nested event loop.
5. One line on the control socket — `goto 1` — segfaults the process (F-6).
6. `systemd-coredump` writes the core.

**Result.** The core was dumped to
`/var/lib/systemd/coredump/core.mergen.1000.<boot>.105473.<ts>.zst`, and the
distinctive marker planted in the elevated document appears in it **twice**:
```
occurrences of the marker in the core: 2
MERGEN-AUDIT-ELEVATED-MARKER-7f3a9c
MERGEN-AUDIT-ELEVATED-MARKER-7f3a9c
```
(Two copies because `Document::m_data` and poppler's own `loadFromData` buffer
each hold one — the amplification of F-10 showing up again.)

The stored cores are `root:root 0640` with an ACL admitting the owning UID, in
`/var/lib/systemd/coredump/`, retained by systemd's own policy. `core_pattern`
is `|/usr/lib/systemd/systemd-coredump …` and `ulimit -c` is `unlimited` — the
Arch default, i.e. MERGEN's own stated target platform.

**Why this is a real finding and not a platform quirk.** MZ.md §8 is explicit:
"Nothing else is persisted." MX.md §5's whole design is that the privileged
bytes exist only in the viewer's memory — that is *why* the file is opened
through a helper into `loadFromData` rather than copied to a temp file, and why
the document "exists here only as bytes, with no readable path" is accepted as a
cost in MZ.md §9's redaction section. A crash defeats all of it: the content of a
file the reader had to summon an administrator to open is now a durable artefact
on disk, at a path the reader never chose and cannot see in
`~/.local/state/mergen/`.

Contributing detail: `Document::close()` (`src/document.cpp:111`) uses
`m_data.clear()`, which releases the buffer without zeroing it — unlike
`promptForPassword`, which is careful to `fill('\0')` the password
(`src/mainwindow.cpp:659-667`). The same care is not taken with the document
bytes, so they linger in freed heap and a *later* crash can still dump them.

**Who could realistically trigger it.** The reader themselves, by accident — a
scripted `mergen goto` keybinding fired while the password prompt is up. Or
anyone who can already write to the reader's runtime socket. No second account
and no malicious PDF needed.

**Fix direction:** fix F-6 first, then defend in depth: `prctl(PR_SET_DUMPABLE, 0)`
for the lifetime of an elevated document (or `setrlimit(RLIMIT_CORE, 0)` at
startup, which is what a viewer with no crash-reporting story should arguably do
anyway — §5 already bans crash reporting), and explicitly zero `m_data` before
releasing it.

---

### F-12 — The path is handed to root **by name**, un-canonicalised, with the whole authentication dialog as the swap window
**Severity: LOW** on its own; **MEDIUM composed with F-4 / F-10**
**CONFIRMED by running**

The helper's internal TOCTOU is genuinely closed (H-1). The one *between* the two
processes is not, and the stated model does not mention it.

`openPath` decides a file needs elevation using unprivileged `QFileInfo` checks
(`src/document.cpp:22-27`: `exists()`, `isFile()`, `isReadable()`), then hands
`info.absoluteFilePath()` to the helper (`src/mainwindow.cpp:534`).
`QFileInfo::absoluteFilePath()` makes a path absolute; it does **not** resolve
symlinks — `canonicalFilePath()` is the one that does, and it is not used.

Reproduced by opening a symlink to an unreadable file and logging what the helper
actually received:
```
$ ln -s /tmp/mgaudit/victimdir/doc.pdf /tmp/mgaudit/slink.pdf   # target is mode 000
$ mergen /tmp/mgaudit/slink.pdf
argv: /usr/lib/mergen/mergen-open /tmp/mgaudit/slink.pdf        <- the symlink, not the target
/tmp/mgaudit/victimdir/doc.pdf                                  <- what root will actually resolve
```
(A relative `./slink.pdf` is correctly made absolute by `main.cpp:22-23` before
anything else — that part is right, and matters because the helper's CWD is
`/root`.)

So root re-resolves the name, and the window between MERGEN's `stat` and root's
`open` spans the **entire polkit dialog** — seconds to minutes of a human typing
a password. Another local user who owns any directory component can swap the
target in that window.

**What it is worth to an attacker.** Less than it first looks, and I want to be
precise rather than alarming:
- Repointing at a different root-only PDF puts those bytes in the *victim's*
  viewer, not the attacker's. No exfiltration.
- Repointing at a FIFO is the reachable route to F-4's permanent wedge —
  `QFileInfo::isFile()` screens FIFOs out at MERGEN's end, so this swap is the
  way past that screen.
- Repointing at a huge or endless file is the reachable route to F-10.
- Devices and directories are still refused by the helper (H-2).

**Fix direction:** `canonicalFilePath()` before handing the path over closes the
symlink half. Closing it properly means passing a *descriptor* rather than a
name — the unprivileged side opens `O_PATH`, the helper receives it — but that is
a redesign, and given `auth_admin` the name-based version is defensible. It
should at least be *stated*, because MX.md §5 currently reads as though no
name/descriptor gap exists anywhere.

---

### F-13 — `allow_gui=true` hands `$DISPLAY` and `$XAUTHORITY` to a root process that never draws anything
**Severity: LOW**
**Assessed by reading — confirmed against `man pkexec` (polkit 127-3.1)**

`data/mergen.policy.in:24`:
```xml
<annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
```
`man pkexec` on this machine:

> The environment that PROGRAM will run it, will be set to a minimal known and
> safe environment in order to avoid injecting code through LD_LIBRARY_PATH or
> similar mechanisms. … As a result, pkexec will not by default allow you to run
> X11 applications as another user since the $DISPLAY and $XAUTHORITY environment
> variables are not set. These two variables will be retained if the
> `org.freedesktop.policykit.exec.allow_gui` annotation on an action is set to a
> nonempty value; **this is discouraged, though, and should only be used for
> legacy programs.**

`mergen-open` is 132 lines of `open`/`fstat`/`read`/`write`. It links nothing but
libc, opens no display, and would not notice if `DISPLAY` were unset. The
annotation buys nothing and re-widens an environment pkexec had deliberately
narrowed, in the one process in this project that runs as root. It also sits
oddly against the project's own X11 ban (§5) and against MZ.md's Wayland-only
target, where the retained variables are meaningless anyway.

**Fix direction:** delete the annotation. The authentication dialog is drawn by
the polkit *agent* in the reader's own session, not by the helper, so nothing
about the prompt depends on it.

---

### F-14 — The authentication prompt never names the file, so the administrator authorises blind
**Severity: LOW**
**Assessed by reading**

`data/mergen.policy.in:17`:
```xml
<message>Authentication is required to open a PDF you do not have permission to read</message>
```
The dialog identifies no document. An administrator standing at a colleague's
machine is asked to grant a root read and shown neither which file nor which
directory. Given F-12 they could not fully trust the name anyway, but "some PDF
somewhere" is the weakest possible informed consent, and the message is the only
place the reader and the administrator meet.

polkit's message variables (`$(user)`, `$(program)`, …) do not include argv, and
interpolating an arbitrary attacker-influenced path into an authentication dialog
would be its own hazard, so this is a real constraint rather than an oversight —
but MERGEN can say it itself: show the full path in the page view *before*
starting pkexec, so the reader knows what they are about to ask for.

**Fix direction:** display the path in the view ahead of the prompt; consider
naming the file in `<message>` only if polkit ever gains a safe mechanism.

---

### F-15 — `writeAll` can spin forever at 100% CPU as root if `write()` ever returns 0
**Severity: NIT**
**Assessed by reading**

`src/mergen-open.cpp:39-52`:
```cpp
while (len > 0) {
    const ssize_t n = ::write(STDOUT_FILENO, buf, len);
    if (n < 0) { if (errno == EINTR) continue; return false; }
    buf += n;
    len -= static_cast<size_t>(n);      // n == 0 makes no progress
}
```
A `write()` of `0` with `len > 0` advances nothing and loops forever. POSIX only
permits a 0 return when the count is 0 for regular files and pipes, so this is
theoretical rather than reachable — but it is an unbounded busy loop in a root
process, which is the wrong place for a theoretical.

**Fix direction:** `if (n == 0) return false;`.

---

## Further CONFIRMED-HOLDS found while attacking

### H-11. The control socket's permissions and path are sound; the `/tmp` fallback is unreachable on Linux.
`Control::socketPath()` (`src/control.cpp:31-37`) falls back to `QDir::tempPath()`
— world-writable `/tmp` — when `QStandardPaths::RuntimeLocation` is empty. I went
looking for a socket-squatting attack there (another local user pre-creating
`/tmp/mergen-$UID.sock`, `listen()`'s probe connecting to *them*, and `main.cpp:32`
then handing that attacker every document path before exiting). It does not work,
because the branch is dead on this platform. Measured:
```
XDG_RUNTIME_DIR=/tmp/mgrt   -> RuntimeLocation = /tmp/mgrt        empty=0
XDG_RUNTIME_DIR unset       -> RuntimeLocation = /tmp/runtime-megas  empty=0   (created 0700)
XDG_RUNTIME_DIR=/tmp/badrt (mode 0777, wrong)
                            -> RuntimeLocation = /tmp/runtime-megas  empty=0   (Qt rejects the bad dir)
```
Qt never returns an empty runtime location on Linux and validates a hostile
`XDG_RUNTIME_DIR` rather than trusting it. And the socket itself is created with
`QLocalServer::UserAccessOption` (`src/control.cpp:26`); verified on disk:
```
srwx------ 1 megas megas 0 mergen-1000.sock
```
0700, owner only. **No other local user can reach the socket.** The fallback is
still a latent hazard if it is ever relied on, and is worth deleting or
hard-failing rather than silently landing in `/tmp`.

### H-12. Modality is what protects the UI routes — and is exactly why the socket is the hole.
Worth stating because it explains the shape of F-6. `promptForPassword` uses
`QInputDialog::getText`, i.e. `QDialog::exec()`, which is *application-modal*: Qt
suppresses window shortcuts and input for non-modal widgets, so
<kbd>Ctrl</kbd>+<kbd>K</kbd> → `showCommands` (which contains the same
`m_doc->pageCount()` call at `src/mainwindow.cpp:1241`) cannot fire during the
prompt. The control socket is not a UI event; modality does not apply to it, and
it walks straight into the same call. The *elevation* loop
(`src/mainwindow.cpp:691-694`) is a bare `QEventLoop` with no modality at all —
deliberately, per MX.md §8, "so the window carries on painting" — so during
elevation every UI route is live and `m_opening` is the only guard.

### H-13. The search worker *does* guard the locked-document state.
`SearchWorker::run()` (`src/document.cpp:434-438`) checks
`if (!doc || doc->isLocked()) { Q_EMIT done(false); return; }` before touching
`numPages()`. Confirmed by running: `search hi` over the socket during the
password prompt replies `ok` and the process survives. This is the same hazard
F-6 trips over, handled correctly one file away — which is why F-6 reads as an
oversight rather than a design decision, and why fixing it at
`Document::pageCount()` would be consistent with what is already here.

### H-14. Elevated bytes are not written to `recent.toml`, and an elevated document is correctly barred from redaction.
`pushRecent` stores the path only. `redactSelection` (`src/mainwindow.cpp:948-953`)
refuses when `!m_doc->data().isEmpty()`, exactly as MZ.md §9 says. `Document::openPath`
clears `m_data` on a subsequent successful non-elevated open (`src/document.cpp:38`),
so elevated bytes do not survive into the next document's state.

### H-15. The helper leaks no descriptors and handles short reads correctly.
`::close(fd)` is on every exit path. The magic-read loop (`src/mergen-open.cpp:86-100`)
handles short reads and `EINTR`, cannot overflow the 5-byte buffer, and the 5
bytes it consumed are re-emitted before the bulk loop resumes from the current
offset — no truncation and no duplication. `memcmp` is only reached when all 5
bytes were actually read, so the uninitialised `magic` array is never compared
short. Exit codes are unambiguous (0 success, 1 failure) and do not collide with
pkexec's own 126/127, which `readElevated` handles separately.

---

## Severity index

| # | Severity | Finding | Evidence |
|---|---|---|---|
| F-6 | **HIGH** | Control socket `goto` segfaults the viewer during `openPath`'s password nested loop (`isOpen()` true for a locked document → `Catalog::getNumPages()` null deref) | **Confirmed** — 7 cores, gdb backtrace |
| F-11 | **HIGH** | The privileged document's plaintext is written to a persistent core dump; marker bytes recovered from `/var/lib/systemd/coredump/` | **Confirmed** — full chain executed |
| F-1 | **HIGH** | `readElevated` treats a still-running pkexec as success and accepts a truncated document (`exitCode()` is 0 before the process finishes) | **Confirmed** — harness + real binary |
| F-2 | MEDIUM | `~QProcess` runs with a live root child: `kill()` cannot work post-authorisation, GUI thread blocks up to 30 s | Confirmed (destructor path); reasoned (root child) |
| F-3 | MEDIUM | The `%PDF-` gate checks five bytes and nothing else; no structural validation of what follows | **Confirmed** |
| F-4 | MEDIUM | `open()` blocks before the `S_ISREG` check and neither side has a timeout: wedged root process, `m_opening` stuck true for the session | **Confirmed** (blocking open) |
| F-5 | MEDIUM | The 2 GiB cap is tested against `st_size` and never enforced on the stream — 5-byte file emitted 700 005 bytes | **Confirmed** |
| F-9 | MEDIUM | `pkexec` is resolved through `$PATH`; a planted shim replaced the entire privileged read with no prompt | **Confirmed** |
| F-10 | MEDIUM | The viewer buffers the whole elevated stream unvalidated: 500 MB in → 1 059 MB RSS | **Confirmed** |
| F-7 | LOW | `open` over the socket replies `ok` when the guard discarded it; six guarded entry points fail silently; portal routes jump the wrong document | **Confirmed** (socket); read (rest) |
| F-8 | LOW | Compare mode is not covered by the guard and `setDocument` never clears `m_diffBands` — stale difference marks on an unrelated document | Read |
| F-12 | LOW | Path handed to root by name, un-canonicalised; the swap window is the whole authentication dialog | **Confirmed** |
| F-13 | LOW | `allow_gui=true` retains `$DISPLAY`/`$XAUTHORITY` for a root process that never draws; `man pkexec` calls this discouraged | Read |
| F-14 | LOW | The authentication message names no file — the administrator authorises blind | Read |
| F-15 | NIT | `writeAll` spins forever as root if `write()` returns 0 | Read |

**Holds:** H-1 descriptor-not-name validation · H-2 non-regular files refused ·
H-3 argument handling · H-4 never setuid, paths agree end to end · H-5 `auth_admin`
is right · H-6 action cannot be aimed elsewhere · H-7 no shell, argv API ·
H-8 no pipe deadlock, no stdout truncation · H-9 helper path is compile-time ·
H-10 passwords wiped, elevated docs barred from redaction · H-11 socket is 0700,
`/tmp` fallback unreachable · H-12 modality protects the UI routes ·
H-13 search worker guards the locked state · H-14 elevated bytes not persisted to
`recent.toml` · H-15 no fd leaks, short reads correct.

## Where the stated model and the code disagree

MX.md §5 / MZ.md §6 make four claims. Two are true as written, two are not:

1. *"only once it holds the file descriptor — not before, so the name cannot be
   swapped underneath it"* — **true inside the helper** (H-1), but there is a
   second, much wider name/descriptor gap between MERGEN's unprivileged decision
   and root's `open` that the document does not mention (F-12).
2. *"anything that is not a regular file beginning `%PDF-` is refused"* — **true,
   literally**. The problem is what it implies and does not deliver (F-3).
3. *"reading a PDF is the only thing the helper can be made to do, so it cannot be
   turned into a general-purpose read of privileged files"* — **overstated**. It
   can be made to emit any root-only file whose first five bytes are `%PDF-`, in
   full and without bound (F-3, F-5). Under `auth_admin` this is defence in depth
   rather than a boundary, which is the honest framing and should be the one in
   the document.
4. *"It is never installed setuid, and MERGEN itself never runs with privilege"* —
   **true** (H-4).

MZ.md §8's *"Nothing else is persisted"* is contradicted in practice by F-11.
MX.md §8's *"a re-entry guard stops a second open arriving through the nested
loop"* is true of `openPath` and false of everything else `MainWindow` exposes
while that loop is up (F-6, F-7, F-8).

---

## Reproduction notes

Everything above can be re-run. Nothing in the repository was modified; the
fixtures live outside it.

- `/tmp/mgaudit/` — helper fixtures: `real.pdf`, `notpdf.bin`, `exactly5.pdf`,
  `short.bin`, `empty.bin`, `polyglot.pdf`, `fifo`, `link_shadow`, `link_zero`,
  `adir/`, `grow.pdf`, `swap.pdf`, `locked.pdf` (`qpdf --encrypt`),
  `noperm.pdf` (mode `000`), `slink.pdf`, `payload.pdf` (marker-tagged),
  `core.bin` (extracted core).
- `/tmp/mgfake/pkexec` — the `PATH` stand-in used for F-1, F-9, F-10, F-11 and
  F-12. It never invokes real polkit. **No privilege was escalated at any point
  in this audit, and no real protected system file was read** — every
  root-only-file question was answered by reasoning about code paths that were
  exercised unprivileged.
- `/tmp/mgqt/t.cpp`, `/tmp/mgqt/t2.cpp` — harnesses replicating `readElevated`'s
  exact structure (H-8, F-1). `/tmp/mgqt/sp.cpp` — `QStandardPaths` probe (H-11).
- `/tmp/mgrt` — a private `XDG_RUNTIME_DIR` for the socket tests, so the
  reader's real session socket was never touched.
- The viewer was run with `QT_QPA_PLATFORM=offscreen` throughout.

Repository state after the audit: `git diff` and `git diff --cached` are both
empty; the six untracked PDFs at the top level (`annot.pdf`, `colour.pdf`,
`evil.pdf`, `outline.pdf`, `outline-v2.pdf`, `test.pdf`) pre-date this session
(mtime 14:41; the audit began at 15:00) and were only read. This file lives under
`/docs/`, which `.gitignore:19` excludes.
