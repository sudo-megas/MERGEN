# X3 — End-to-end threat model, and an independent second opinion on redaction

Scope: the whole system, crossing component boundaries. Written against the tree
at `472183b` (branch `ata`), source in `<repo>/src`, constitution in
`<repo>/build/docs/MZ.md`.

Two sections:

1. **Threat model** — attacker goals traced end to end, each with a verdict, the
   code that decides it, and a note on which defences are load-bearing.
2. **Redaction, independent review** — `src/redact.cpp` reviewed on its own
   terms, with reproductions actually executed.

Everything marked **REPRODUCED** was run. Everything marked **BY READING** is an
assessment of the code without a running proof.

---

## Verdicts at a glance

| Attacker goal | Verdict | Worst concrete outcome |
|---|---|---|
| Read a file I am not allowed to read | **PARTIAL** | A one-time admin authentication becomes a permanent, unauthenticated copy of the privileged document in `/var/lib/systemd/coredump/`. Reproduced end to end |
| Make the user believe a secret is gone | **ACHIEVABLE** | `Redact::run` returns `ok` with the secret under the black bar, extractable by poppler *and* MuPDF. Five distinct routes, all reproduced |
| Run code or reach the network from a document | **BLOCKED** | No network code exists; one `QProcess` with a literal program and a list of arguments; every dangerous PDF action type discarded at `document.cpp:172` |
| Persist something affecting a later session | **ACHIEVABLE** (same-uid writer) | Planted `recent.toml` / `portals.toml` entries survive normal use and are re-written by MERGEN itself. Reproduced |
| Make the viewer act on a file the user did not choose | **ACHIEVABLE** | Socket `open` has no gate at all; the Portals list opens a stored path with no hash check |
| Denial of service | **ACHIEVABLE** | One line — `goto 1` during the password prompt — SIGSEGVs the viewer. Reproduced, with backtrace |
| Interfere with another user's MERGEN | **BLOCKED** normally | `UserAccessOption` gives a 0700 socket. Residual: path squatting discloses the reader's file path and makes MERGEN exit silently. Reproduced |

The single most important sentence in this report: **MERGEN wipes the password with great care
and never wipes the document the password was protecting.**

---

## Section 1 — Threat model

### Attacker positions considered

| # | Position | What it can already do |
|---|---|---|
| **A** | A hostile **document** | Full control of the bytes MERGEN parses, renders, searches and redacts |
| **B** | A same-uid **local process**, *unconfined* | Everything the user can do; interesting only where MERGEN acts as a deputy |
| **B'** | A same-uid **local process**, *confined* (bwrap/firejail/container with `$XDG_RUNTIME_DIR` or `$HOME` reachable but not the user's documents) | Reach the socket and/or the state files, but not the files themselves |
| **C** | A **different local user** | Ordinary filesystem access; `/tmp` |
| **D** | Someone who **reads the disk later** — backup, forensics, a recycled machine | Everything left behind |

Position **B'** is the one MZ.md never considers, and it is where most of what follows lives.

---

### Goal 1 — "Read a file I am not allowed to read"

**Verdict: PARTIAL — and the weakness is not in the helper.**

**The helper itself holds.** `src/mergen-open.cpp` is 132 lines and each of its checks is
load-bearing:

- `argc != 2` and `path[0] != '/'` (lines 57-63) — one absolute path, nothing else.
- **It validates the descriptor, not the name** (lines 65-104): `open()` first, then
  `fstat`, `S_ISREG`, the size cap and the `%PDF-` magic are all read *through the fd it
  already holds*. There is no window in which the name can be swapped. This is the single
  best piece of security engineering in the tree and the comment at lines 8-14 says exactly
  why it is written that way. It must not be "simplified" into a `stat(path)`.
- `S_ISREG` (line 76) stops the action being pointed at a device or a fifo; `kMaxBytes`
  (line 29) stops a 2 GB+ read; the magic check stops it becoming a general-purpose
  privileged `cat`.
- The polkit action (`data/mergen.policy.in`) is `auth_admin` for **all three** of
  `allow_any`/`allow_inactive`/`allow_active` — not `auth_admin_keep` — so every invocation
  authenticates afresh, and `org.freedesktop.policykit.exec.path` pins the binary. It is
  never installed setuid (`CMakeLists.txt` installs it as a plain executable).

One narrow observation, **BY READING**: the magic test is five bytes. A root-owned file whose
first five bytes are `%PDF-` is dumped in full whatever it really is. Since the caller must
already hold admin credentials to get that far, this is not an escalation — but it does mean
the helper's promise ("a root-owned file that is not a PDF is refused") is really "a file
that does not *begin* `%PDF-` is refused".

**The weakness is what happens to the bytes afterwards.** The helper hands the plaintext of a
file that required an administrator's password to `MainWindow::readElevated`
(`mainwindow.cpp:709`), and from that moment nothing in MERGEN treats it as sensitive.

**REPRODUCED — three unwiped copies, none removed by `close()`.** Harness
`/tmp/x3/residue.cpp`, compiled against the built objects, driving the real
`Document::openData(bytes, "/root/secret.pdf")` path with a marker string, then scanning its
own writable anonymous mappings:

```
marker present in the file bytes: yes
openData status=0 pages=1
[A] while open, marker copies in this process's heap: 3
[B] after handing the bytes to a SearchWorker:      3
[C] after Document::close():                        3
[D] after the SearchWorker is destroyed too:        3
[E] after the Document itself is destroyed:         3
```

`Document::close()` (`document.cpp:108-113`) calls `m_data.clear()`, which drops a reference
and resets a length. It does not overwrite a byte. Poppler's own copy from
`loadFromData` is never touched at all, and `QProcess`'s internal read buffer holds yet
another before `readAllStandardOutput()` ever returns.

**REPRODUCED — those bytes reach the disk, and a crash to put them there is one line away.**
The chain, all of it executed:

1. `Control::onConnection` keeps serving the socket from inside **any** nested event loop.
   `QInputDialog::getText` in `promptForPassword` (`mainwindow.cpp:656`) is one such loop, and
   `readElevated`'s `QEventLoop` (`mainwindow.cpp:691-693`) is another.
2. `Document::adopt` (`document.cpp:63-76`) keeps the poppler handle and returns
   `NeedsPassword`, so **`Document::isOpen()` returns true for a document that is still
   locked**. Every consumer that guards on `isOpen()` is wrong for the duration of the prompt.
3. `runCommand`'s `goto` guard is exactly that (`mainwindow.cpp:757`), so it passes and calls
   `m_doc->pageCount()` on a locked document.

One line from an unprivileged same-uid process — `goto 1` — killed pid 182801 with SIGSEGV.
The trace from `coredumpctl` names every step:

```
#0  pthread_mutex_lock                       (libc)          <- SEGV_MAPERR
#1  Catalog::getNumPages()                   (libpoppler)
#2  mergen::MainWindow::runCommand()
#3  the listenForCommands lambda
#4  mergen::Control::onConnection()
...
#16 QEventLoop::exec()
#17 QDialog::exec()                                          <- the password prompt
#18 QInputDialog::getText()
```

4. `systemd-coredump` writes the whole image to `/var/lib/systemd/coredump/`, and the ACL on
   it is `user:megas:r--` — **readable by the unprivileged user, forever, with no further
   authentication**.

I proved the last link independently rather than assuming it: `/tmp/x3/residue2` crashes
*after* `Document::close()` has run, and the marker comes back out of the core file three
times:

```
$ grep -ao 'PRIVILEGEDMARKER0xDEADBEEF' residue2.core | wc -l
3
```

So a one-time `auth_admin` authentication is converted into a permanent, unauthenticated
copy of the privileged document on disk — recoverable by position **B**, **B'** and **D**.

**The asymmetry is the finding.** `promptForPassword` (`mainwindow.cpp:653-672`) wipes the
typed string twice and the `QByteArray` once, with a comment citing MX.md §5: *"The password
lives no longer than the attempt."* Fifty lines away, `readElevated` returns the entire
document the password was protecting, by value, and **nothing anywhere wipes it**. MERGEN is
careful with the key and careless with the plaintext.

**Other resting places for privileged bytes, traced end to end:**

| Destination | Reached by | Verdict |
|---|---|---|
| `recent.toml` | `pushRecent(m_doc->path())`, `mainwindow.cpp:571` | The **absolute path** of the root-owned file is written to a user-readable state file and persists. Not content, but it names what the reader was shown |
| `portals.toml` | `markPortal` -> `contentHash()`, `document.cpp:201-204` | For an elevated document the hash is taken **over `m_data`** — a SHA-256 of privileged content, persisted in a user-readable file. An offline-verifiable fingerprint: enough to confirm *which* known document the reader opened |
| Clipboard | `PageView::copySelection`, `pageview.cpp:683-688` | Privileged text goes to the compositor clipboard, readable by any client with clipboard access. No permission check |
| Print-to-file | `printDocument`, `mainwindow.cpp:1479` | **No `m_doc->data()` guard**, unlike redaction. The elevated document renders straight into a reader-chosen PDF/PS file. Arguably intended, but it is the one path by which the bytes leave deliberately, and it is the only one MERGEN never mentions |
| Redacted copy | `redactSelection`, `mainwindow.cpp:948-953` | **BLOCKED**, and this guard is load-bearing twice over — see below |
| Core dump / swap | any crash | Reproduced above |
| Error messages | `readElevated`, `mainwindow.cpp:684-712` | **Safe.** `SeparateChannels` keeps stderr out of the byte stream, and no error string ever interpolates document content |

**Load-bearing: `if (!m_doc->data().isEmpty())` in `redactSelection` (`mainwindow.cpp:948`).**
It stops qpdf being pointed at a path the process cannot read, *and* it stops a root-owned
document's bytes being rewritten to a reader-chosen destination. MZ.md §9 justifies it on the
first ground only; the second is the more important one. If elevated redaction is ever
implemented, that second reason has to be answered separately.

**And a confused-deputy channel that needs no elevation at all.** For position **B'** the
socket is a read primitive: `open <path>` answers `ok`/`err` for any path on the filesystem
(a readability + is-it-a-PDF oracle), and then `search <term>` followed by `next` answers
`ok` versus `err: no search hits` — a content oracle over a document the caller cannot open
itself. Both reproduced. MZ.md §9 says *"Every command here is one the reader could already
have issued from the keyboard; the socket adds reach, not capability."* That is true of the
**reader** and false of the **caller**.

**Severity:** the memory/core-dump chain is **high** — it defeats the entire purpose of the
polkit action, is triggerable by any same-uid process, and leaves evidence on disk that
outlives the session. The oracles are **medium**, and matter only for confined callers.


### Goal 2 — "Make the user believe a secret is gone when it is not"

**Verdict: ACHIEVABLE.** This is Section 2 of this document, so only the system-level part is
stated here.

I asked the wider question the brief poses — *does anything else in MERGEN claim to remove or
hide content?* — and traced every candidate. The answer is that redaction is the **only**
place MERGEN makes a removal claim, which is why its failure is not diluted by anything else:

- **Night mode** transforms the decoded image only (`document.cpp` render path); it claims
  nothing about content and hides nothing.
- **Presentation mode** withdraws chrome, not content.
- **Annotation rendering** is explicitly `HideAnnotations = false` (`document.cpp:89`) — MERGEN
  never claims to hide markup, and MZ.md §13 records the absence of a hide switch as an open
  ruling rather than a silent behaviour.
- **The properties overlay** *warns* about JavaScript, forms and attachments
  (`document.cpp:328-339`) rather than claiming to have neutralised them, and the wording —
  "MERGEN never runs it" — is accurate.
- **The password** is the one other thing MERGEN claims to destroy, and that claim **holds**
  (`mainwindow.cpp:653-672`).
- **The recent list** drops paths that have gone away, which is housekeeping, not a promise.

So the surface is exactly one routine, and R-1 through R-8 are what happens on it. The
cross-component point worth adding here: `redactSelection` derives the rectangle from
`selectionBounds()` (`pageview.cpp:347-371`), which is the **union of the selected words'
boxes** — a rectangle that is generally much larger than the selection a reader sees
highlighted. Everything inside that rectangle is painted opaque black, so MERGEN's visual
assertion covers strictly more than its textual one, while `stillThere` only ever checks the
textual one. The gap between "what the bar covers" and "what was verified" is the hole every
finding in Section 2 walks through, and it is created by two components that were written
separately: selection geometry in `pageview.cpp` and verification in `redact.cpp`.

---

### Goal 3 — "Run code, or reach the network, from a document"

**Verdict: BLOCKED.** MZ.md's claim is correct, and here is precisely what makes it correct,
so that none of it is removed by accident.

- **There is no network code in the tree.** A grep for `QNetworkAccessManager`, `QTcpSocket`,
  `QUdpSocket`, `QSslSocket`, `QHostAddress`, `http` and `curl` across `src/` returns nothing
  but the word "curl" inside the empty-state watermark drawing (`pageview.cpp:856-869`).
  Qt6::Network is linked for `QLocalServer`/`QLocalSocket` and nothing else, and a Unix domain
  socket has no network surface by construction.
- **There is exactly one `QProcess` in the tree** (`mainwindow.cpp:677`) and it is the pkexec
  invocation. It uses `setProgram("pkexec")` + `setArguments({MERGEN_HELPER_PATH, path})` — a
  **list**, so `execve` with no shell anywhere. No `system()`, no `popen()`, no `fork()`, and
  the helper path is a compile-time constant. A document cannot influence either argument
  except by being the file the reader asked to open.
- **Every dangerous PDF action type is discarded before anything can act on it.**
  `Document::pageLinks` (`document.cpp:172-176`) accepts `Poppler::Link::Goto` and *only*
  `Goto`, then drops `isExternal()`. That single line is what refuses `LinkExecute` — PDF's
  "launch an application" action — along with `LinkBrowse` (URLs), `LinkJavaScript`,
  `LinkSound` and `LinkMovie`. `pageLinks` is the only producer of link data in the tree and
  hold-to-peek is its only consumer.
- **The outline is filtered the same way**: `flattenOutline` resolves a destination only when
  `externalFileName().isEmpty() && uri().isEmpty()` (`document.cpp:134`), leaving `page = -1`
  otherwise, and both consumers guard on it (`mainwindow.cpp:1366`, `1382`).
- **`peekPage` bounds-checks** its argument against the page count (`mainwindow.cpp:1410`).
- **JavaScript is read but never run**: `m_doc->scripts()` is consulted once, to produce a
  warning string (`document.cpp:328-331`).
- **Forms** are never filled — there is no form-field write path at all.

The residual code-execution surface is poppler's own parsers (fonts, images, filters) reached
with attacker-controlled bytes, which is a memory-safety question for the fuzzing audit rather
than a design question. Nothing in MERGEN's own design opens a door.

**These are the lines to protect:** `document.cpp:172`, `document.cpp:176`, `document.cpp:134`,
`mainwindow.cpp:679`. Each is one line, each looks like an ordinary filter, and each is the
whole of a §5 ban.

---

### Goal 4 — "Persist something that affects a LATER session"

**Verdict: ACHIEVABLE for position B/B' (a same-uid writer). BLOCKED for a document.**

A malicious *document* cannot influence either state file: `recent.toml` receives only the
path the reader opened, and `portals.toml` only what `markPortal` was explicitly asked to
record. Nothing derived from document *content* is written except the content hash, and that
is opaque. Good.

A same-uid process that can write `~/.local/state/mergen/` is a different matter, and MZ.md
§8's rule — *"If it is missing, unreadable or malformed, MERGEN starts with no portals... a
bad state file is never an error the reader sees"* — means a **poisoned** state file is also
never an error the reader sees.

**REPRODUCED.** I planted both files and launched MERGEN with `XDG_STATE_HOME` pointed at them:

```
portals.toml:  [[portal]] hash="aa" path="/tmp/x3/PLANTED Quarterly Report.pdf" page=0
                          hash="bb" path="/etc/passwd"                          page=2147483647
recent.toml:   "/tmp/x3/PLANTED Quarterly Report.pdf", "/etc/shadow", ...
```

MERGEN started clean — no crash, no warning, nothing in the log. Then, after one ordinary
`open` through the socket, `recent.toml` came back as:

```
recent = [
    "<repo>/test.pdf",
    "/tmp/x3/PLANTED Quarterly Report.pdf",
    "/etc/shadow",
]
```

**The planted entries survived normal use and were re-written by MERGEN itself.**
`pushRecent` (`mainwindow.cpp:1859-1863`) prunes only entries whose file does not exist, and
`/etc/shadow` exists. `portals.toml` was left untouched entirely — `savePortals` is called
only when a portal is *made*, so a planted file persists indefinitely.

What that buys the attacker is Goal 5, below: entries in the Open dropdown and the Portals
list that point wherever they like.

Two smaller notes, **BY READING**:

- `loadPortals` takes `page` from `value.toInt()` with no bound (`mainwindow.cpp:840`), and
  `2147483647` rides all the way to `PageView::scrollToPage`. That is **safe**, because
  `scrollToPage` clamps on `index < 0 || index >= m_layout.size()` (`pageview.cpp:190-192`).
  A real bound, in the right place.
- `loadRecent` caps at `kRecentLimit` entries while parsing (`mainwindow.cpp:1803`), so a
  huge `recent.toml` cannot grow the list. `loadPortals` has **no such cap** — every
  well-formed `[[portal]]` block is appended, so `portals.toml` is an unbounded allocation
  driven by a file on disk. Bounded by disk size only; a low-grade memory DoS.

---

### Goal 5 — "Make the viewer act on a file the user did not choose"

**Verdict: ACHIEVABLE.** `MainWindow::openPath` is documented as "the single entry point for
every way a document is opened" (`mainwindow.h:45-48`). That is true, and it is why the entry
points are worth listing together — the header names four, and there are seven:

| Entry point | Code | Gate on the path |
|---|---|---|
| `argv[1]` | `main.cpp:22-23` | The reader typed it |
| File dialog | `chooseFile`, `mainwindow.cpp:599` | The reader picked it |
| Compare | `chooseComparison`, `mainwindow.cpp:1030` | The reader picked it |
| Drag and drop | `dropEvent`, `mainwindow.cpp:1191` | Single local file — **but the `.pdf` check is only in `dragEnterEvent`** |
| **Control socket** | `runCommand`, `mainwindow.cpp:744` | **None whatsoever** |
| **Recent menu** | `rebuildRecentMenu`, `mainwindow.cpp:1889` | Whatever `recent.toml` says |
| **Portals list** | `showPortals`, `mainwindow.cpp:1010-1018` | Whatever `portals.toml` says — **and no hash check at all** |
| Portal follow | `followPortal`, `mainwindow.cpp:929` | Hash must match the open document |

**The socket is unconditional and reproduced.** `open <any path>` is honoured with no
validation of any kind, and if the path is unreadable it goes straight into the elevation
path. From one line sent by an unprivileged process:

```
$ ps -eo pid,ppid,cmd | grep pkexec
129625  125137 /usr/bin/pkexec /usr/lib/mergen/mergen-open /tmp/x3/unreadable.pdf
```

**MERGEN raised an administrator authentication prompt because a local process asked it to**,
with MERGEN's own trusted framing — *"Authentication is required to open a PDF you do not have
permission to read"* — at a moment of the attacker's choosing. That is a credential-phishing
primitive that position **B** does not otherwise have, and the `auth_admin` default means the
prompt asks for the **admin** password specifically.

**The state files turn a write primitive into a click.** `rebuildRecentMenu` labels each entry
`QFileInfo(path).fileName()` and `showPortals` labels each row with the two basenames — both
attacker-chosen, since the attacker controls the whole path. `/tmp/evil/Quarterly Report.pdf`
displays as `Quarterly Report.pdf`. Neither label is sanitised, so a `U+202E` in a planted
filename reverses the displayed text; low severity on its own, but it is exactly the kind of
help this goal wants.

`showPortals`'s activation handler is the sharpest of these because it skips the one check
that portals are built around:

```cpp
const auto go = [this](QListWidgetItem *item) {
    const QString path = item->data(Qt::UserRole).toString();
    ...
    if (QFileInfo::exists(path)) { openPath(path); m_view->scrollToPage(page); }
};
```

`followPortal` gates on `contentHash()`; this list does not gate on anything. A reader who
opens the command overlay, types "portals" and picks a row is opening a path from a file on
disk with no relationship to the document in front of them.

**`followPortal` is hash-gated, but the hash is attacker-chosen.** An attacker who knows the
SHA-256 of a document the reader will open — a public paper, a company template, or simply a
document the attacker sent them — plants a portal keyed to that hash on page 0. The reader
opens their own document, presses `Ctrl+J` expecting to jump within it, and MERGEN opens the
attacker's file instead. Content-addressing makes the portal *robust*; it does not make it
*authentic*.

**Drag and drop, BY READING.** `dragEnterEvent` (`mainwindow.cpp:1181-1189`) requires exactly
one local file ending `.pdf`; `dropEvent` (`mainwindow.cpp:1191-1198`) re-checks the count and
`isLocalFile()` but **not the extension**. Qt only delivers a drop that `dragEnterEvent`
accepted, so this is a latent inconsistency rather than a live hole, and MERGEN's own load
path refuses non-PDFs anyway. It should still be symmetric.

**What holds, and is load-bearing:** **nothing auto-opens.** `main.cpp` opens `argv[1]` and
nothing else; `loadRecent` and `loadPortals` run in the constructor but only populate menus.
A poisoned state file cannot open anything on its own — it always needs a click. That is the
difference between this being a nuisance and being a remote-ish code path, and it must stay
that way: any future "reopen last document" feature would convert every finding here into a
zero-click one.

---

### Goal 6 — "Denial of service"

**Verdict: ACHIEVABLE, several ways, one of them a crash. All reproduced.**

| Vector | Effect | Reach |
|---|---|---|
| `quit` over the socket | Window closes, no confirmation | One line, position B/B' |
| `goto 1` during the password prompt | **SIGSEGV** | One line; see Goal 1 |
| `open <unreadable path>` | Spawns pkexec; the nested loop has **no timeout and no cancel** (`mainwindow.cpp:690-694`), so with no agent answering it never returns | One line |
| Squatting the socket path | MERGEN hands over and exits with no window | Position C, conditionally |
| Large `portals.toml` | Unbounded `m_portals` growth | Position B |
| `enterCompare` on a large document | `computeDiff` renders **every page of both documents** synchronously on the main thread (`mainwindow.cpp:1099-1104`) | Self-inflicted |

**The pkexec wedge is the interesting one, because it also makes the control surface lie.**
While the elevation loop is spinning, `m_opening` stays `true`, so every subsequent
`openPath` returns immediately (`mainwindow.cpp:519-521`) — and `runCommand` then reports the
result as `m_doc->isOpen() ? ok : err(...)` (`mainwindow.cpp:749`), which is true because some
*earlier* document is still open. Reproduced:

```
current doc contains 'needle'?                     -> ok      (test.pdf is loaded)
open <repo>/colour.pdf                 -> ok
after that 'open', does the doc still contain 'needle'? -> ok  <- colour.pdf never loaded
open /etc/passwd                                   -> ok      <- not even a PDF
open /tmp                                          -> ok      <- a directory
```

Before the wedge, `open /etc/passwd` correctly returned `err: could not open /etc/passwd`.
So a script driving MERGEN gets `ok` for opens that never happened, indefinitely, from one
line sent by anyone. The re-entrancy guard is right; reporting success when it fires is not.

**Load-bearing here:** the `m_opening` guard itself (`mainwindow.cpp:519-523` with its
`QScopeGuard`) is what stops the *re-entrancy* being far worse than a lie — without it, a
socket `open` during the pkexec or password loop would re-enter `openPath` while a
half-constructed document is live. Keep the guard; fix the reply.

---

### Goal 7 — "Interfere with another user's MERGEN, or another instance of mine"

**Verdict: BLOCKED for a different local user in the normal configuration; ACHIEVABLE for a
same-uid process; conditionally achievable for a different user via path squatting.**

**Load-bearing: `m_server->setSocketOptions(QLocalServer::UserAccessOption)`
(`control.cpp:27`).** Measured on a live instance:

```
srwx------ 1 megas megas 0 /run/user/1000/mergen-1000.sock
```

0700, inside a 0700 `$XDG_RUNTIME_DIR`. Position **C** cannot connect. I also checked the
fallback: with `XDG_RUNTIME_DIR` unset, Qt creates `/tmp/runtime-megas` mode 0700 and the
socket is 0700 inside it, so even then it is not connectable by another user. That one line
in `control.cpp` is the whole of the cross-user defence.

**The residual cross-user risk is squatting, not connecting.** `Control::socketPath()`
(`control.cpp:31-37`) falls back to `QDir::tempPath()` when `QStandardPaths` yields no runtime
location — giving the fully predictable `/tmp/mergen-$UID.sock` in a world-writable directory.
`Control::listen()` then *probes before taking it* (`control.cpp:44-49`), and if anything
answers it concludes an instance is already running.

**REPRODUCED** (same-uid squatter, but the mechanism is identical for a different user once
the path is in a shared directory):

```
squatter listening on .../mergen-1000.sock
$ mergen /tmp/x3/secret-plan.pdf
victim mergen exit code: 0
SQUATTER RECEIVED: open /tmp/x3/secret-plan.pdf
(no mergen running: it handed over and exited)
```

The victim's file **path is disclosed to whoever holds the socket**, and MERGEN exits
silently with no window — the reader sees their viewer simply not start. The same-instance
handover in `main.cpp:30-35` sends first and never checks the reply, so there is no point at
which a wrong answer is noticed.

Two smaller notes on the socket, **BY READING**:

- The verb is lowercased (`control.cpp:84`), so `quiT` is `quit`. Confirmed live. Harmless,
  but worth knowing it is case-insensitive.
- `Control::send` does `line.toUtf8().trimmed() + '\n'`, which strips only leading and
  trailing whitespace — an embedded newline in `argv[1]` survives into the wire format. It is
  **not** exploitable, because `onConnection` reads exactly one line per connection and then
  disconnects (`control.cpp:77-91`). **That "one line per connection" rule is load-bearing**:
  it is the only thing standing between a filename containing a newline and command
  injection into the control surface.
- `QLocalServer::removeServer` on a stale path (`control.cpp:54`) is correct for the crashed-
  instance case Z8 asks for, and I confirmed a relaunch takes over a stale socket cleanly.

---

### Load-bearing defences — the list to protect

Named here so they are not removed as tidying. Each is small, each looks incidental, and each
is the whole of a guarantee:

1. **`mergen-open.cpp:65-104` — validate the descriptor, never the path.** No TOCTOU window.
2. **`mergen-open.cpp:57-63, 76, 80, 101` — argc, absolute path, `S_ISREG`, size cap, magic.**
3. **`mergen.policy.in` — `auth_admin` on all three defaults (not `auth_admin_keep`), plus
   `exec.path`.** Every invocation authenticates; the binary is pinned.
4. **`mainwindow.cpp:948` — `if (!m_doc->data().isEmpty())` blocks elevated redaction.**
   Stops qpdf on an unreadable path *and* stops privileged bytes reaching a chosen destination.
5. **`document.cpp:172, 176` — only internal `Poppler::Link::Goto`.** This is what refuses
   `LinkExecute`, `LinkBrowse` and `LinkJavaScript`. One line, three §5 bans.
6. **`document.cpp:134` — outline entries with a URI or external file resolve to no page.**
7. **`mainwindow.cpp:679` — `setArguments({...})`, a list, so `execve` with no shell.**
8. **`control.cpp:27` — `UserAccessOption`.** The whole cross-user defence.
9. **`control.cpp:77-91` — one line per connection.** The whole anti-injection defence.
10. **`mainwindow.cpp:519-523` — the `m_opening` re-entrancy guard with its `QScopeGuard`.**
11. **`pageview.cpp:190-192` — `scrollToPage` clamps.** Absorbs unbounded page numbers from
    `portals.toml`.
12. **`mainwindow.cpp:1803` — `loadRecent` caps while parsing.** (`loadPortals` should too.)
13. **`pageview.cpp:355-357` — a selection spanning pages is refused, not half-honoured.**
14. **`main.cpp` opens `argv[1]` and nothing else — nothing auto-opens.**
15. **`mainwindow.cpp:680` — `SeparateChannels`**, so helper diagnostics can never be spliced
    into the document bytes.
16. **`redact.cpp:212-215` and `QPDFWriter::write()` on a fresh `QPDF`** — see R-9.

### The three cross-component interactions that matter most

Each is invisible to a per-file review, because neither file is wrong on its own:

1. **Password prompt × control socket.** `QDialog::exec()` keeps the socket server running,
   and `Document::adopt` leaves `isOpen()` true while the document is still locked. Neither
   is a bug alone; together they are a one-line remote SIGSEGV, and — because of (2) — a way
   to put privileged plaintext on disk.
2. **Elevation × process lifetime.** Nothing wipes `Document::m_data`, poppler's copy, or the
   `QProcess` buffer, and nothing suppresses core dumps. The password is wiped with great
   care; the document it protects is not. The polkit prompt's promise ends at the moment the
   bytes arrive.
3. **Elevation × the control socket.** `open <unreadable>` lets any local process summon an
   administrator authentication prompt in MERGEN's name, then wedges the open path in a
   nested loop with no timeout — during which the socket cheerfully answers `ok` to opens
   that never happen.

---

## Section 2 — Redaction, independent review

### R-1 — CRITICAL. `Redact::run` reports success while text under the black bar is fully extractable

**Files:** `src/redact.cpp:150-153` (`Cut::inside`), `src/redact.cpp:166-189` (`stillThere`),
`src/mainwindow.cpp:966` (area), `src/pageview.cpp:347-371` (`selectionBounds`).

**Status: REPRODUCED.** Verified with poppler (`pdftotext`, and `Poppler::Page::text()` —
MERGEN's own verifier) and independently with MuPDF (`mutool draw -F txt`). Driven through
MERGEN's real code path: `Document::words()` -> union-of-boxes -> `unrotateRect` ->
y-flip -> `Redact::run`, compiled against the built objects.

**Root cause — two defects that compose:**

1. `Cut::inside()` tests **only the origin of the text object**:

   ```cpp
   bool inside() const {
       const QPointF at = m_text.times(m_ctm).origin();
       return m_area.contains(at);
   }
   ```

   A show operator is judged by where its text *starts*, never by the extent of the
   glyphs it paints. A `Tj` that begins one point left of the rectangle and paints
   200 points of text through it is kept in full — and then an opaque black rectangle
   is painted over it by `redact.cpp:219-222`. That is precisely the "draw a box over
   it and call it redacted" failure MZ.md §3/§5 added the `qpdf` dependency to avoid;
   the difference is that here MERGEN *also tells the reader it succeeded*.

2. `stillThere()` searches the written file **only for the words the reader selected**.
   It cannot see anything else the bar now covers. So defect 1 turns into a *reported
   success* rather than a refusal.

**Reproduction** (`/tmp/x3/attack1.pdf`, built by `/tmp/x3/mk.py`):

```
BT /F1 12 Tf 3 Tr 1 0 0 1  60 700 Tm (SECRET99-4111111111111111) Tj ET   % invisible, x 60..233
BT /F1 12 Tf 0 Tr 1 0 0 1 200 700 Tm (Alice) Tj ET
BT /F1 12 Tf 0 Tr 1 0 0 1 235 700 Tm (Smith) Tj ET
BT /F1 12 Tf 0 Tr 1 0 0 1  60 660 Tm (Contract of employment) Tj ET
```

The reader selects `Alice Smith` and asks for it to be removed.

```
expected='Alice Smith'
area=200.0000 697.5160 65.6720 11.1000
REDACT ok=1 error=                       <- MERGEN reports the redaction succeeded
```

The output, read back:

```
$ pdftotext attack1-red.pdf -
SECRET99-4111111111111111
Contract of employment

$ mutool draw -F txt attack1-red.pdf     # MuPDF, a different engine entirely
SECRET99-4111111111111111
Contract of employment
```

`Alice` and `Smith` were genuinely removed. The credit-card-shaped string whose glyphs
sit under the black bar was not, because its operator *starts* at x=60 and the bar
starts at x=200. MERGEN's own verifier saw that string — `Poppler::Page::text()`
returns it — and passed the file anyway, because it was only ever asked about
`Alice` and `Smith`.

**Severity: critical.** This is the one routine in the tree whose failure mode is a
false guarantee, and this is that failure mode. Threat model: a document author who
knows a reader will redact before forwarding. It needs no cleverness beyond placing a
show operator so that it starts outside, and ends inside, the rectangle the reader
will draw.

**Fix direction.** The containment test must be over the *painted extent* of the
operator, not its origin — which means measuring glyph widths (font metrics), the
one thing the state machine deliberately does not do. Where that is too much,
the honest alternative is: (a) split show operators at the rectangle boundary rather
than keeping or dropping whole ones, and (b) make `stillThere` verify the *rectangle*
rather than the *string* — extract `page->text(area)` from the output and refuse if
**anything at all** comes back inside the redacted rectangle. (b) alone closes the
reported-success hole for every variant below, is three lines, and needs no metrics.


### R-2 — CRITICAL. Only the page content stream is filtered; text in Form XObjects survives untouched

**File:** `src/redact.cpp:210-215`.

```cpp
QPDFPageObjectHelper target = all[static_cast<size_t>(page)];
target.addContentTokenFilter(std::make_shared<Cut>(area, &removed));
```

The filter is attached to the **page's** content stream. A page's text is very often not
there: `Do` on a Form XObject draws from a stream the filter never sees, and the same is
true of Type 3 font glyph procedures and of annotation appearance streams. Form XObjects
are not exotic — every `pdftk`/`qpdf` overlay, every `\includegraphics` of a PDF figure,
every imported page, every stamp and most Illustrator/InDesign output uses them.

**Status: REPRODUCED**, poppler and MuPDF (`/tmp/x3/attack3.pdf`).

```
page /Contents:  BT ... 240 700 Tm (Alice) Tj ET      BT ... 240 685 Tm (Smith) Tj ET   q /X1 Do Q
XObject /X1:     BT ... 140 700 Tm (XOBJSECRET-987654321) Tj ET
```

Reader selects `Alice Smith`:

```
REDACT ok=1 error=
$ pdftotext attack3-red.pdf -  ->  XOBJSECRET-987654321
$ mutool draw -F txt ...       ->  XOBJSECRET-987654321
```

Rendered at 100 dpi, the black bar covers px 331-376 and the XObject string runs px
196-391 — it is drawn *through* the bar and is still there in full.

Note what saves the tool in the simpler case: if the *selected* text is itself inside an
XObject, nothing is removed, `removed == 0`, and MERGEN declines with "Nothing removable
was found in that area" — or if something else was removed, `stillThere` finds the
selected text and refuses. The hole is exactly when the selected text is page-level and
the survivor is not.

**Severity: critical.** **Fix direction:** recurse the filter into the page's Form
XObjects (composing their `/Matrix` and the CTM at the `Do`), or — much better and much
cheaper — verify the *rectangle* rather than the string, which catches this and every
other stream MERGEN does not walk, including annotation appearance streams it can never
rewrite.

### R-3 — HIGH. The verification is vacuous whenever every selected token is under three characters

**File:** `src/redact.cpp:183-187`.

```cpp
for (const QString &word : needle.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
    if (word.size() >= 3 && text.contains(word)) {
        return true;
    }
}
return false;
```

If no token reaches three characters, the loop body never runs and `stillThere` returns
`false` — "clean" — **without having checked anything at all**. Initials, a two-letter
country or ward code, a two-digit patient/case number, a CJK name of two characters: all
of these are exactly the things people redact, and for all of them the verification MZ.md
§9 calls "not a formality" is a no-op.

**Status: REPRODUCED** (`/tmp/x3/attack4.pdf`).

```
BT ... 1 0 0 1  60 700 Tm (aaaaaaaaaaaaaaaaaaaaaaaaaaaaa XY) Tj ET   % 'XY' lands at x 256.8
BT ... 1 0 0 1 200 685 Tm (42) Tj ET
```

Reader selects `XY 42` (`expected='XY 42'`); `42` is removed, `XY` is not (its operator
starts at x=60, outside the rectangle):

```
REDACT ok=1 error=
$ pdftotext attack4-red.pdf -  ->  aaaaaaaaaaaaaaaaaaaaaaaaaaaaa XY
$ mutool draw -F txt ...       ->  aaaaaaaaaaaaaaaaaaaaaaaaaaaaa XY
```

Also note the same clause the other way: `stillThere` returns `false` for an *empty*
`expected` (`redact.cpp:168-170`) — reachable only via the public `Redact::run` API, since
`redactSelection` guards `text.isEmpty()`, but it is the wrong default for a function
whose contract is "prove it is gone".

**Severity: high.** **Fix direction:** drop the length filter; compare the whole
`simplified()` needle and every token, and treat "I could not check" as failure, not
success.

### R-4 — HIGH (integrity, not confidentiality). The text matrix never advances, so redaction silently deletes content far outside the black bar

**File:** `src/redact.cpp:53-110`. `handleToken` updates the text matrix for `Tm`, `Td`,
`TD`, `T*`, `'`, `"` and `BT`, but a show operator never advances it by the width of the
glyphs it painted. Every show operator between two positioning operators is therefore
judged at the same point — the position of the first.

**Status: REPRODUCED** (`/tmp/x3/attack5.pdf`).

```
BT /F1 12 Tf 1 0 0 1 100 700 Tm (SECRET) Tj (      this tail is far right of the bar and must survive) Tj ET
```

Reader selects only `SECRET`; the rectangle is `area=100 697.5 48.7 11.1`, i.e. 48 points
wide. Rendered at 100 dpi, the source has ink from px 140 to px 569 on that line. The
redacted output has **exactly one run of ink on that line: px 139-206** — the black bar.
Everything from px 235 to 569 has been deleted, four hundred points outside the rectangle
the reader drew, with no mark and no warning.

MZ.md §5 bans "editing a PDF in place"; it does not contemplate a redacted copy quietly
losing paragraphs. A reader forwarding this copy has silently dropped content that was
never selected and is not visibly marked as removed.

**Severity: high** for document integrity; it is not a confidentiality failure, and in the
confidentiality direction it errs safe. **Fix direction:** advance the text matrix by the
shown width (needs font metrics), or — accepting that metrics are out of scope — split
show operators at the rectangle boundary rather than dropping whole ones.

### R-5 — HIGH. Annotation text is never removed, and redaction still reports success

**File:** `src/redact.cpp:210-215` (same root as R-2).

Annotation appearance streams live outside `/Contents` entirely, so no token filter can
reach them — and MERGEN never strips `/Annots`. Poppler extracts them (`page->text()`
returns them; so does `pdftotext`, and so does MuPDF), so text a reader believes they have
removed can sit in a FreeText note, a Widget's `/AP` (which is where a *form field's value*
is drawn — and MZ.md §4 has MERGEN warn about form fields elsewhere), or a stamp.

**Status: REPRODUCED** (`/tmp/x3/annotsec.pdf`): page text `Alice` / `Smith`, plus a
FreeText annotation with `/Contents (ANNOTSECRET-55555)` and a matching `/AP` over the same
rectangle. Reader selects `Alice Smith`:

```
REDACT ok=1 error=
$ pdftotext annotsec-red.pdf -   ->  ANNOTSECRET-55555
$ mutool draw -F txt ...         ->  ANNOTSECRET-55555
```

Worth noting for the visual half of the promise: the black bar is appended to the *page
content stream* (`redact.cpp:219-222`), and annotations are composited **after** page
content, so the bar is drawn *underneath* every annotation on the page. In the render of
the output the annotation string runs straight across the bar (ink at px 281-457 with the
bar at 333-376). The bar cannot hide annotation content even in principle.

**Severity: high.** **Fix direction:** the rectangle-based verification in R-1 catches this
automatically. Removing the content properly means also editing or dropping intersecting
annotations, which is a real piece of work; refusing loudly when the rectangle intersects
an annotation would be an honest interim answer.

### R-6 — HIGH. The "never writes over the document it read" guard is symlink-bypassable, and the source is destroyed

**File:** `src/redact.cpp:195-197`.

```cpp
if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) {
    return {false, QObject::tr("Redaction never writes over the document it read.")};
}
```

`absoluteFilePath()` cleans `.` and `..` but **does not resolve symbolic links**.
`canonicalFilePath()` does. Both a symlinked file and a symlinked parent directory walk
straight through this guard.

**Status: REPRODUCED**, twice.

```
$ ln -sf /tmp/x3/src1.pdf /tmp/x3/link-to-src.pdf
$ md5sum src1.pdf                      bf62d785...
$ probe src1.pdf link-to-src.pdf 0 ...  ok=1 error=
$ md5sum src1.pdf                      6a0186fa...      <- the SOURCE was rewritten

$ ln -sfn /tmp/x3/realdir /tmp/x3/aliasdir
$ probe realdir/doc.pdf aliasdir/doc.pdf 0 ...  ok=1
$ pdftotext realdir/doc.pdf -           "Public paragraph text"   <- original lost its text
```

This breaks two rules of the constitution at once: MZ.md §5 "Editing a PDF in place — every
write produces a new file", and MZ.md §8 "It never modifies the open document, and the open
document is never the destination." The reader's only copy of the unredacted original is
replaced by the redacted one, irrecoverably.

It is easy to hit by accident — `~/Documents -> /mnt/data/Documents` is an ordinary setup —
and it is also plantable: the suggested destination is fully deterministic
(`mainwindow.cpp:956-957`, `<dir>/<basename>-redacted.pdf`), so a hostile same-uid process
can pre-create that name as a symlink to any file the user can write and have MERGEN
overwrite it with a PDF. (The save dialog's overwrite prompt is the only thing in the way.)

**Severity: high.** **Fix direction:** compare `canonicalFilePath()`, and when the
destination does not yet exist, canonicalise its *directory* and compare that plus the file
name. Refusing to write to an existing symlink at all would be stronger still.

### R-7 — MEDIUM (honesty of the mark). The black bar can be silently clipped away

**File:** `src/redact.cpp:219-222`.

```cpp
const std::string cover = "q 0 0 0 rg " + ... + " re f Q\n";
target.addPageContents(QPDFObjectHandle::newStream(&pdf, cover), false);
```

The cover is appended *after* the page's own content and wrapped in `q … Q`. That protects
the rest of the page from the cover, but it does not protect the cover from the page: `Q`
restores the state saved by the cover's own `q`, and cannot undo a clipping path installed
by an unbalanced `q … W n` earlier in the stream. A page whose content stream ends with a
clip still in force swallows the bar entirely.

**Status: REPRODUCED** (`/tmp/x3/clip.pdf`, content ending `q 0 0 1 1 re W n`):
`REDACT ok=1`, the text *is* removed — and the render has **0 dark pixels** inside the
redaction rectangle. MZ.md §9 asks for the bar so "the page shows that something was taken
rather than quietly closing over it"; here it quietly closes over it.

Confidentiality is unaffected (the text really is gone), so this is an honesty and
verifiability defect, not a leak. **Fix direction:** wrap the cover in its own
`q … Q` *after* first emitting a `Q` for every unbalanced `q` seen by the filter (the
filter already tracks that stack, `redact.cpp:67-73`), or write the cover into a Form
XObject with its own reset state.

### R-8 — HIGH (the tool does not work). On real documents the selection almost never starts at an operator origin, so redaction declines

Direct consequence of the origin-only test (R-1). Three files, driven through MERGEN's own
selection code:

| document | selection | result |
|---|---|---|
| `/usr/share/doc/valgrind/valgrind_manual.pdf` p6 | `Start Guide` | `Nothing removable was found in that area.` |
| `/usr/share/doc/speex/manual.pdf` p3 | `1 Introduction` | `The text is still in the rewritten file, so it was discarded.` |
| **the project's own `test.pdf`** p1 | `needle` | `Nothing removable was found in that area.` |

`test.pdf`'s line is `BT /F1 12 Tf 72 660 Td (searchable haystack needle page 1) Tj ET` —
one operator for the whole line, which is what LaTeX, Word, Ghostscript and every other
real producer emit. Redaction works only when the reader's selection happens to begin
exactly where a show operator begins.

This is the flip side of R-1 and it is important to state alongside it: **the common case
fails safe.** The origin test usually puts the operator *outside* the rectangle, nothing is
removed, and MERGEN declines. The lie in R-1/R-2/R-3/R-5 appears only when something *else*
in the rectangle happened to be removable. That is a narrow escape, not a design.

**Severity: high** as a functional defect; MZ.md §10 Z10 records redaction as verified, and
the verification evidently never included a document from a real producer.

### R-9 — What holds, and why

Stated plainly, because these are the parts that must not be weakened:

- **`addContentTokenFilter`, not `filterContents`** (`redact.cpp:212-215`). The Z10
  amendment in MZ.md §9 is correct and the code follows it. Verified: `base.pdf` selection
  `CONFIDENTIAL` produces an output from which both poppler and MuPDF return only
  `Public paragraph text`. The text is genuinely gone from the content stream.
- **`QPDFWriter::write()` on a freshly parsed `QPDF`, not an incremental update.** I
  searched the output files for the removed strings in raw bytes *and* after inflating every
  Flate stream: **no trace, in any of them.** "What is not written is not in the file"
  (`redact.cpp:100-101`) is true, and it is true *because* the whole document is rewritten
  from the object model. An incremental-update implementation would have left the original
  object intact and this whole routine would be theatre.
- **The rotation round-trip is exact.** `Document::rotateRect` / `unrotateRect`
  (`document.cpp:392-427`) invert each other, `pageSize()` returns the *unrotated* size, and
  the y-flip at `mainwindow.cpp:966` is therefore in the right space. Measured: selecting the
  same word at view rotations 0, 90 and 180 produces byte-identical
  `area=100.0000 697.1020 102.6760 12.9500`. Rotation contributes no error at all.
- **Multiple content streams are handled.** A page with `/Contents [4 0 R 6 0 R]` where the
  `BT`/`Tm` live in the first stream and the `Tj` in the second is filtered correctly
  (`/tmp/x3/split.pdf`): qpdf coalesces, the filter's matrix state carries across the
  boundary, and the text is removed. Verified with poppler and MuPDF.
- **Pages with unterminated text objects** (`BT` with no `ET`) are handled without incident.
- **Elevation cannot reach redaction** (`mainwindow.cpp:948-953`). A document read through
  the helper has `m_doc->data()` non-empty and is refused before any path is handed to qpdf.
  This is load-bearing twice over: it stops qpdf being pointed at a root-owned path, and it
  stops a root-owned document's bytes being written to a reader-chosen destination.
- **A selection spanning two pages is refused** (`pageview.cpp:355-357`) rather than
  half-honoured, exactly as MZ.md §9 says.
- **`stillThere` treats an unreadable output as dirty** (`redact.cpp:171-179`): a file
  poppler cannot load, or that is locked, or whose page is missing, returns `true` and the
  output is deleted. Failing closed here is right and must stay.
- **The verification does fire.** The speex case above is `stillThere` catching a genuine
  survivor, deleting the file and telling the reader nothing was saved. When the survivor is
  in `expected`, this works.

### R-10 — Smaller observations (BY READING, not reproduced)

- **`redact.cpp:229-232`** deletes the destination when `removed == 0`, and
  **`redact.cpp:238-243`** deletes it when the text survives — but the `catch` at
  **`redact.cpp:233-236`** does not. An exception thrown by `QPDFWriter::write()` after it has
  begun writing leaves a partially written file at the reader's chosen path, named
  `…-redacted.pdf`, containing whatever had been emitted. Add the `QFile::remove` to the
  catch.
- **`redact.cpp:67-73`**: `q`/`Q` save and restore the CTM but not `m_leading`. Leading *is*
  part of the PDF graphics state, so a `TL` inside `q … Q` leaks out and mis-places every
  subsequent `T*` and `'`. Fails in the safe direction (wrong position → operator kept), and
  compounds R-8.
- **`redact.cpp:53-62`**: every non-`tt_word` token is pushed into `m_operands`, including
  array delimiters, names and strings. `operand()`/`numbers()` index backwards from the
  operator, so `[ (a) -100 (b) ] TJ` puts five entries in `m_operands`. Harmless for the
  operators that matter today (they are all preceded by bare numbers), but it is a latent
  trap: an operator preceded by a mixed operand list would read the wrong slots.
- **No handling of `Ts` (text rise) or `Tz` (horizontal scale).** Both move where glyphs
  land relative to the tracked origin. Safe direction only.
- **`redact.cpp:98`**: `"` takes `aw ac string` and `'` takes `string`; both are treated as
  next-line-then-show, which is right, and the line advance is applied *before* the
  containment test, which is also right.
- **Pages whose `MediaBox`/`CropBox` origin is not (0,0)**: `pageSize()` returns the crop-box
  *size* and poppler's word boxes are relative to the crop box, but `Cut::inside` compares
  against raw PDF user-space coordinates. Confirmed on `/tmp/x3/offsetbox.pdf`
  (`MediaBox [0 500 612 1292]`): `area=100 697.1 104.2 12.95` while the text actually sits at
  user-space y=1200, so nothing matches and MERGEN declines. Redaction is simply
  non-functional on such pages. Fails safe.
- Disproved hypothesis, recorded so it is not re-investigated: I expected
  `Poppler::Page::text(QRectF())` to miss text on an offset-origin page (it uses the crop box
  as a device-space rectangle). It does not — it returned both strings on
  `offsetbox.pdf`. The verifier's *coverage* of the page is fine; its *question* is what is
  wrong.

