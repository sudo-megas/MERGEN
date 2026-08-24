# S2 — State-on-disk parser audit (recent.toml / portals.toml)

Audit of hand-rolled TOML-ish parsers in `src/mainwindow.cpp` against
MZ.md §8's claims: atomic write (temp file, fsync, rename), a bad state
file is never an error the reader sees, and portals are keyed by content
hash so they survive a rename.

**Scope:** `loadRecent()`, `saveRecent()`, `pushRecent()`, `recentFilePath()`,
`loadPortals()`, `savePortals()`, `portalFilePath()`, `tomlEscape()`,
`tomlUnescape()` — all in `src/mainwindow.cpp`.

**Method:** read-only audit. Real user state at `~/.local/state/mergen/`
backed up to `/tmp/mergen_audit_backup/mergen_real_backup/` before any
testing; all probes below point `XDG_STATE_HOME` at scratch directories
under `/tmp`, so the real files are not touched by test runs. A small
harness (`probe.cpp`) is linked against the real compiled `.o` objects
from `build/CMakeFiles/mergen.dir` to drive real `MainWindow` construction
and `openPath()` (both public). Where private/anonymous-namespace code
(`tomlEscape`/`tomlUnescape`, the `loadPortals`/`loadRecent` parse loops)
cannot be linked externally, it is tested via a byte-identical extract
(`sed` from the source at audit time) into a standalone harness — each
such case is labelled REPLICA. Everything else labelled REPRODUCED was
exercised through the real compiled binary.

This file was written incrementally as findings were confirmed (see
"Progress log" and the discovery-ordered sections below for full evidence,
commands, and byte-exact outputs). This section is the final summary,
written last, with findings resorted most-severe-first as the deliverable
requires.

---

## FINAL SUMMARY

### MZ.md §8's three claims, verdict first

**1. "Written atomically — temp file, `fsync`, rename." CONFIRMED IN INTENT,
WRONG IN MECHANISM.** Traced via an LD_PRELOAD syscall shim around real
`saveRecent()`/`savePortals()` calls, on both tmpfs and (to close that gap)
the real ext4 filesystem the actual state directory lives on. What
happens is `mkdir` → `open64(O_TMPFILE|O_DIRECTORY|O_RDWR|O_CLOEXEC)` →
`fdatasync` → `linkat(/proc/self/fd/N → target, AT_SYMLINK_FOLLOW)`. No
named temp file, no `rename()`/`renameat()` syscall at all — Qt's
`QSaveFile` uses the modern anonymous-inode idiom instead, which is
arguably *stronger* (the file never has a transiently-visible wrong name)
but is not literally what the sentence describes. The content-durability
intent holds (`fdatasync` genuinely runs before the file is published).
One durability gap the sentence implies but the code doesn't deliver:
**the containing directory is never `fsync`'d after the `linkat`**, so the
publish itself isn't guaranteed durable across a crash on all
filesystems/mount options, even though the content was.

**2. "If it is missing, unreadable or malformed, MERGEN starts with no
[state]... a bad state file is never an error the reader sees." CONFIRMED,
exactly as scoped — this is a claim about the *read* side.** Every load-time
malformation tested — truncated files, no closing bracket, CRLF, embedded
NUL, invalid UTF-8, a 500MB file, a 100-million-character single value, 1
million `[[portal]]` blocks, 500,000 empty headers, negative/overflowing
page numbers, comment-injected brackets, three-ends-in-one-block, missing
keys, duplicate keys — every single one completed with no crash, no hang,
no dialog, no visible error, across several dozen real-compiled-code test
runs. This claim is solid. It is worth being precise about what it does
*not* cover, since two adjacent things are true and matter:
  - The claim is about a bad file never producing a *visible* error — it
    is not a claim that a bad file's *effects* are harmless. The
    `ends`-counter finding below shows a single stray or reordered line
    can make `loadPortals()` silently and *permanently* destroy reader-made
    portals (the next `savePortals()` overwrites the only copy) — literally
    "never an error the reader sees," and also literally data loss with no
    error the reader sees, which is worse than the reassuring reading of
    the sentence suggests.
  - The claim doesn't cover the *write* path at all, and two write-path
    findings below matter more than the read-side claim being intact: a
    real POSIX failure mode (`RLIMIT_FSIZE`) crashes the whole application
    via an uncaught `SIGXFSZ` (this is not "a bad state file" — it's a
    normal write hitting a resource limit, so it's outside the letter of
    the claim, but it is very much an error the reader sees, and it's
    adjacent enough to the state-file-writing code that a reader would
    reasonably expect the same "never visible" treatment to apply); and
    `markPortal()` unconditionally shows "Portal made" whether or not the
    save actually succeeded, on every silent-failure path tested (full
    disk via a genuine `ENOSPC` reproduction, read-only directory, dangling
    symlink, a file blocking directory creation).

**3. "Documents are identified by a hash of their bytes rather than their
path, so a portal survives the file being moved." CONFIRMED for the near
end, REFUTED for the far end.** Renaming the document you invoke `Ctrl+J`
*from* has zero effect — the near end is matched against the live,
freshly-hashed document regardless of path, exactly as claimed. Renaming
the document you're trying to *reach*, while leaving the near end
untouched, breaks following completely: the far end is located solely by
its stored `path` field, which nothing ever refreshes, even though the
portal is still holding that exact document's correct hash. Grepped every
use of `hash`/`contentHash` in the file (six call sites total) to confirm
there is no fallback resolution-by-hash anywhere. Both directions tested
with real renames on disk and a real `Ctrl+J` trigger through the compiled
binary.

### Findings, most severe first

**HIGH — `loadPortals()`'s `ends` counter silently destroys reader-authored
portals on plausible malformations.** mainwindow.cpp:804-847. Which end
(`current.a`/`current.b`) a `hash`/`path`/`page` line writes into is
decided by an `ends` counter that only increments on the literal key
`page` — so a reordered `page` line, a missing `page` line, a duplicated
`page` line, or a third full end in one block each cause silent
corruption or total loss of that portal (both ends, every field), with the
very next `savePortals()` making the loss permanent. Confirmed end-to-end
with real compiled code across 9 distinct cases (see the headline finding
below); does **not** cross a `[[portal]]` boundary (confirmed refuted:
corruption never contaminates a neighboring, well-formed block). Fix
direction: parse each end as an explicit fixed-shape unit instead of
inferring position from a same-block counter tied to one key.

**HIGH — far-end rename permanently breaks `followPortal()`, contradicting
MZ.md's specific rationale for hashing.** mainwindow.cpp:897-934. `to->path`
is the only way the far document is ever located; `to->hash` is never used
to re-resolve a stale path, even though it's sitting right there in the
same struct and would match the renamed file exactly. Confirmed with real
renames + real `Ctrl+J` triggers in both directions. Fix direction: when
`openPath()` succeeds, check whether the newly-computed hash matches any
portal end whose stored path differs, and self-heal it.

**HIGH — an uncaught `SIGXFSZ` (RLIMIT_FSIZE exceeded during a state-file
write) crashes the entire application, not just the save.** Reproduced:
`(ulimit -f 0; ... markPortal ...)` → process killed, exit status 153 (=
128+25, confirmed via `kill -l XFSZ`), core dumped. No `SIGXFSZ` handling
anywhere in the source (`grep -rn "SIGXFSZ\|sigaction\|signal("
src/` — empty). A one-line `signal(SIGXFSZ, SIG_IGN)` at startup converts
this into an ordinary, already-handled `write()` failure.

**MEDIUM — no cap of any kind on portal count or per-field length,
unlike `recent.toml`'s deliberate 10-item cap.** Confirmed by code search
(no `kRecentLimit`-equivalent exists for portals) and empirically: 1,000,000
well-formed portals load and persist correctly (no crash, ~382MB resident,
permanently — not transient like `recent.toml`'s spike), and every future
`markPortal()` rewrites the entire list. A 100-million-character single
field value is accepted whole, no truncation. Fix direction: an explicit
load-time ceiling, mirroring `kRecentLimit`'s pattern.

**MEDIUM — `markPortal()` unconditionally reports "Portal made" even when
the save silently failed.** `savePortals()`'s `QSaveFile::commit()` result
is discarded; the notice is shown regardless. Reproduced across four
distinct real failure conditions (read-only directory, dangling symlink,
directory-blocking file, and a genuine `ENOSPC` via an unprivileged
mount-namespace 4KB tmpfs) — every one completes `markPortal()` normally
and would show the false-success notice. The `ENOSPC` case additionally
confirms a genuine strength of the design: the failure is atomically
graceful (old version kept intact, nothing corrupted/truncated) — only the
notice is wrong. Fix direction: check `commit()`'s return value and show a
different notice on failure.

**LOW-MEDIUM — `recent.toml`'s array-boundary scan is whole-file and
comment-blind.** mainwindow.cpp:1795-1800. `text.indexOf('[')` /
`text.lastIndexOf(']')` span the entire file with no regard for `#`
comments or the `recent = ` key; any quoted string in that span is treated
as an entry. Demonstrated: a `#`-comment containing a bracket and a quoted
path injects a spurious recent-file entry. Not reachable through MERGEN's
own writes; reachable via hand-edited or corrupted files. Fix direction:
anchor parsing to a `^recent\s*=\s*\[` line instead of a whole-file scan.

**LOW — directory-entry durability gap in the atomicity mechanism.** No
`fsync` on the containing directory after `linkat` publishes the file, on
either tmpfs or the real ext4 state-directory filesystem; MERGEN's own code
does none either. A narrow, filesystem-dependent crash window, not unique
to this codebase. See claim 1 above.

**LOW — `recentFilePath()` and `portalFilePath()` disagree under a
relative (spec-invalid) `XDG_STATE_HOME`.** The hand-rolled
`recentFilePath()` honors a relative value literally (resolves against the
process cwd); `portalFilePath()`'s `QStandardPaths` call ignores it and
falls back to the real default `~/.local/state`. Confirmed empirically —
**this specific test caused a real write to the user's actual
`portals.toml`, immediately restored from the pre-audit backup and
verified byte-identical (see the Environment disclosures section and the
final re-verification below).** Fix direction: have `recentFilePath()`
delegate to `QStandardPaths` the same way `portalFilePath()` already does.

**LOW / not reachable — embedded NUL in a state-file path.** Preserved as a
literal in-memory character by both loaders; the underlying `QFileInfo::exists()`
vs `QFile::open()` split (the latter silently truncates at the NUL and can
open a *different* file than requested) is a real Qt/OS-layer quirk, but
every current MERGEN call site checks `exists()` first and is therefore
protected. Recorded as a latent footgun, not a live bug.

**NIT — informational only.** Negative page numbers pass through
unchanged (`toInt()`, no validation); page numbers exceeding `int` range
(`"999999999999"`) silently become `0` (`QString::toInt()`'s documented
overflow behavior); both are inert downstream because `PageView::scrollToPage()`
bounds-checks and no-ops on any out-of-range index. Invalid UTF-8 bytes in
a path become `U+FFFD` replacement characters (Qt's standard lossy
decoding) — the entry survives but can never again match a real file.
Neither needs a fix; recorded because the brief asked about both
specifically.

### Negative results (checked, not just untested)

- **`tomlEscape()`/`tomlUnescape()` round-trip correctly — 0 failures in
  200,020 test cases** (20 targeted adversarial strings, including the
  brief's specific "literal `\n` two-character sequence" case, plus
  200,000 randomized fuzz cases), via a byte-identical verbatim extract of
  the anonymous-namespace source (nothing hand-transcribed) — **and**
  confirmed again through the real compiled `tomlEscape()`/`tomlUnescape()`
  end-to-end, by opening five real files with adversarial bytes (tab,
  quote, a real embedded newline, backslash, apostrophe) actually in their
  filenames and recovering all five with byte-exact length matches after a
  full write-then-reload cycle.
- **No injection or meaningful path-traversal concern.** `readElevated()`'s
  `pkexec` call uses `QProcess::setArguments()` (argv array, no shell), so
  no state-file path content can achieve command injection. `..` and
  absolute-outside-`$HOME` paths behave exactly as they would if the reader
  typed them into the file dialog — a same-user, not-a-privilege-boundary
  situation.
- **`[[portal]]` headers fully reset parser state.** Corruption inside one
  malformed block never contaminates a neighboring well-formed block —
  confirmed with a two-block file where a corrupted first block sits
  directly before a well-formed second block that survives untouched.
- **A valid (non-dangling) symlinked state directory works completely
  transparently**, for both write and read-back, on both ends of the
  symlink — contrasted against the dangling-symlink case, which fails
  silently and gracefully like every other write obstruction tested.
- **`recent.toml`'s `kRecentLimit = 10` cap and de-duplication are both
  correctly enforced at load time**, independent of input size (confirmed
  at up to ~7 million candidate entries in a 500MB file).

### Environment disclosures (read in full before trusting the repo's real state files)

- **`portals.toml`**: this audit's own relative-`XDG_STATE_HOME` test
  (used specifically to probe a path-resolution divergence) caused one real
  write to `~/.local/state/mergen/portals.toml`. **Restored immediately
  from the pre-audit backup, re-verified byte-identical twice** (once right
  after restoring, once again just before writing this summary) —
  confirmed intact as of this report.
- **`recent.toml`**: found modified partway through this audit by what the
  evidence indicates is a **different, concurrent agent session** sharing
  this environment (path naming conventions — `/tmp/mgtest/*`,
  `/tmp/mgaudit/*` — matching neither this audit's own scratch paths nor
  its pre-audit backup; `docs/audit/` here contains numerous sibling audit
  files — `S1-socket.md`, `S3-resources.md`, `S4-ui.md`, `H2-privileged.md`,
  `X3-threat.md`, etc. — confirming a multi-agent coordinated audit sweep
  of this same repo/home directory is underway). **Left as found, not
  restored** — restoring would destroy another session's real, concurrent
  state, the pre-audit backup was itself already contaminated by prior
  agent activity (so it was never a clean baseline to restore *to*), and
  `recent.toml` is explicitly documented (MX.md §7 / MZ.md §8) as a
  self-healing, no-settings, ten-item cache with nothing sensitive in it.
  Pre-audit backup preserved at
  `/tmp/mergen_audit_backup/mergen_real_backup/recent.toml` if wanted.

---

## Progress log

- Read MZ.md §8 and MX.md §7 (source of the claims under test).
- Read full source of target functions in `src/mainwindow.cpp`
  (tomlEscape/tomlUnescape: lines 140-198; loadPortals/savePortals:
  799-868; markPortal/followPortal: 870-934 for context; recentFilePath/
  loadRecent/saveRecent/pushRecent/rebuildRecentMenu: 1774-1891).
- Read `openPath()` (516-573, public), `Document::contentHash()`
  (document.cpp:195), `PageView::scrollToPage()` (pageview.cpp:189,
  bounds-checked) for downstream-effect context.
- Backed up real state dir to /tmp/mergen_audit_backup/mergen_real_backup/.

Findings below, most-severe first (order will be finalized at the end;
appended in discovery order for now, each tagged with a severity so the
final pass can resort).

---
## Finding: Atomicity claim — mechanism differs from MZ.md's literal description, but content durability holds; directory-entry durability does NOT

**Status: REPRODUCED** (empirical, via LD_PRELOAD libc-call interception around a
real `openPath()` → `pushRecent()` → `saveRecent()` write, executed through the
actually-compiled `MainWindow`/`QSaveFile` code — not a replica).

**MZ.md §8 claim:** "Written atomically — temp file, `fsync`, rename." (portals.toml)
**MX.md §7 claim:** "Written atomically — temp file, `fsync`, rename" (recent.toml)

**Method note (methodology correction, kept for the record):** the first pass of
this test used a flawed LD_PRELOAD shim that only forwarded the `mode_t` vararg
to the real `open()`/`open64()` when `O_CREAT` was set. The observed syscall
here is `open64(dir, O_TMPFILE|O_DIRECTORY|O_RDWR|O_CLOEXEC, mode)` —
`O_TMPFILE` does **not** set the `O_CREAT` bit (`O_TMPFILE = O_DIRECTORY |
020000000`, `O_CREAT = 0100`; confirmed no overlap), so the flawed shim silently
substituted `mode=0`, and the resulting file was observed with permissions
`0000`. That was an artifact of the shim, not of MERGEN or Qt — fixed by also
checking `(flags & O_TMPFILE) == O_TMPFILE`, after which the file's true mode
(`0666` requested, `0644` after umask) came through correctly and is reported
below. Flagging this so the corrected numbers are trusted and the transient
`0000` observation is not mistaken for a real finding.

**What actually happens on this system** (confirmed via corrected shim, real
`saveRecent()` call; first run against scratch space on `/tmp`, which is
**tmpfs**, not ext4 -- corrected label, see the follow-up confirmation
below which closes the one gap this left open):

```
mkdir(<state>/mergen) = 0
open64(<state>/mergen, flags=O_TMPFILE|O_DIRECTORY|O_RDWR|O_CLOEXEC, mode=0666) = 7
fdatasync(fd=7) = 0
linkat(olddirfd=AT_FDCWD, "/proc/self/fd/7" -> "<state>/mergen/recent.toml", flags=AT_SYMLINK_FOLLOW) = 0
```

**Follow-up, closing the tmpfs gap:** tmpfs is RAM-backed, so durability
questions are somewhat moot there regardless of what syscalls run -- the
real `~/.local/state/mergen/` lives on `/dev/sda4`, mounted `ext4` (`findmnt
-T /home` confirms `ext4, rw,relatime`), a real block device where this
actually matters. Re-ran the identical shim-instrumented `saveRecent()`
against a scratch directory on that same ext4 filesystem
(`<repo>/docs/audit/.fs-probe`, deleted immediately after,
gitignored path in any case): **identical sequence** --
`mkdir` -> `open64(O_TMPFILE|O_DIRECTORY|O_RDWR|O_CLOEXEC, mode=0666)` ->
`fdatasync` -> `linkat(.../proc/self/fd/N -> recent.toml,
AT_SYMLINK_FOLLOW)`. So this finding is confirmed on the actual filesystem
the real state directory lives on, not just on tmpfs where it would be a
weaker result.

No `rename()`, `renameat()`, or `renameat2()` syscall occurs at all. On a
filesystem that supports `O_TMPFILE` (this one does), Qt's `QSaveFile`/
`QTemporaryFile` backend does not create a *named* temp file and rename it —
it opens an unnamed inode directly inside the target directory via
`O_TMPFILE`, writes to it, `fdatasync`s it, and gives it its final name in one
step via `linkat(AT_FDCWD, "/proc/self/fd/N", targetdirfd, "recent.toml",
AT_SYMLINK_FOLLOW)`. This is a well-known, arguably *stronger* atomic-publish
idiom than named-temp+rename (the file is never visible under any name other
than its final one, so a directory listing mid-write never shows a stray
`.XXXXXX` temp file) — but it is not literally "temp file... rename" as
MZ.md/MX.md describe it.

- **Content fsync claim: CONFIRMED**, mechanism differs. `fdatasync(fd)` is
  called on the temp inode's fd before it is linked into the directory, so
  the file's *content* is flushed to disk before it becomes visible under its
  final name. This satisfies the durability intent behind "fsync" even though
  the specific syscall is `fdatasync` (skips inode metadata not affecting
  file size/content — a reasonable, common substitution) rather than `fsync`.

- **Directory-entry durability: NOT DONE — gap.** After the `linkat()` that
  publishes the file, there is no `fsync`/`fdatasync`/`syncfs` call on the
  containing directory's fd anywhere in the trace, and `grep -n "fsync"
  src/mainwindow.cpp` returns nothing — MERGEN's own code never syncs the
  directory either. Per `fsync(2)`: "Calling fsync() does not necessarily
  ensure that the entry in the directory containing the file has also
  reached disk. For that an explicit fsync() on a file descriptor for the
  directory is also needed." A crash/power-loss in the narrow window after
  `linkat()` returns but before the filesystem's own journal/writeback
  persists the directory update can, on some filesystems/mount options, lose
  the rename even though the file's content was safely fsync'd — i.e. the
  state file could revert to its previous version (or, for a
  previously-absent file, disappear again) after a crash, despite the
  "atomic, fsync'd write" framing. This is a narrow, filesystem-dependent
  window (ext4 with `data=ordered`+journal generally closes it quickly, but
  it is not guaranteed by POSIX) and is a common real-world gap, not unique
  to MERGEN — but it means the literal durability contract implied by
  "temp file, fsync, rename" is not fully met.

**Same code path (`QSaveFile`) is shared by `saveRecent()` and
`savePortals()`** (mainwindow.cpp:853-868 and :1843-1848 both construct a
`QSaveFile`, `write()`, then `commit()`), so this finding applies to both
files identically. Confirmed by reading both call sites; not re-traced
per-file since the mechanism is the same Qt class used the same way.

**Severity:** low. **Fix direction:** if directory-entry durability is
actually wanted, `savePortals()`/`saveRecent()` would need to open the
containing directory themselves and call `fsync` on that fd after
`file.commit()` returns — `QSaveFile` has no built-in option for this. Given
the project's own minimalism bar, the more proportionate fix is to soften
MZ.md/MX.md's wording (e.g. "atomically published via `O_TMPFILE`+`linkat`
or temp-file+rename, content flushed before publish" — without claiming
directory-durability that isn't implemented) rather than adding code for a
crash-window most users will never hit on a session-scoped, easily
regenerated cache file.

---

## recent.toml -- adversarial load matrix (all REPRODUCED via real compiled loadRecent())

Method: real MainWindow constructed (via `probe recentmenu`) with a crafted
recent.toml planted at $XDG_STATE_HOME/mergen/recent.toml, offscreen
platform, 10s timeout per case. rebuildRecentMenu() (mainwindow.cpp:1872,
called from buildToolBar() right after loadRecent()) sets
entry->setToolTip(path) (line 1886) with the exact parsed path, giving a
byte-exact readout of what loadRecent() produced with no private-member
hack needed. Output was made byte-safe (escaped, with explicit
QString::size()/UTF-8-byte-length reporting) after an initial pass using
raw printf("%s", ...) gave misleading results for two cases -- see notes
below; this is a second self-caught methodology fix, kept in the log for
transparency.

Every one of the following completed with exit code 0, no crash, no hang,
under a 10s timeout, confirming MX.md section 7 / MZ.md section 8's "never an error the
reader sees" for ALL of these load-time malformations of recent.toml:

| # | Case | File content (essence) | Result |
|---|---|---|---|
| 1 | baseline | valid one-entry file | loads correctly (control) |
| 2 | empty file (0 bytes) | -- | "No recent files", no crash |
| 3 | no closing ] at all | recent = [ then cut mid-string | text.lastIndexOf(']') returns -1, close < open, early return, empty list |
| 4 | no trailing newline | valid content, file ends right after ] | loads correctly, identical to baseline |
| 5 | CRLF line endings | every line ends CRLF | loads correctly -- the CR lands outside the quoted token so it's inert |
| 6 | embedded NUL inside a quoted path | raw NUL byte, not the two-char escape | preserved as a literal NUL character in the QString -- see dedicated note below |
| 7 | invalid UTF-8 bytes inside a quoted path | 0xff 0xfe 0xfd | QString::fromUtf8 replaces each invalid byte with U+FFFD; entry survives but can never again match a real path (permanently dead, harmless) |
| 8 | no [ or ] at all | recent = "single/string" | indexOf('[') = -1, early return, empty list |
| 9 | [ present, no ] | -- | same as case 3: empty list |
| 10 | 15 quoted entries in the array | -- | exactly 10 kept, in file order, entries 11-15 dropped -- confirms kRecentLimit (mainwindow.cpp:63, checked at mainwindow.cpp:1803 `m_recent.size() < kRecentLimit`) is enforced at LOAD time, not just at push time |
| 11 | same path repeated 3x | -- | exactly 1 entry -- confirms the `!m_recent.contains(path)` de-dup (mainwindow.cpp:1826) |
| 12 | 10x nested brackets around one real entry | [[[[[[[[[[ ... "/tmp/x.pdf" ... ]]]]]]]]]] | still extracts exactly the one real path; extra bracket characters are inert since only the outermost first [ / last ] and quote characters matter |
| 13 | [] (empty array, nothing between) | -- | "No recent files", no crash |

Escape round-trip (unescape direction), case 08_escapes.toml, all CONFIRMED
CORRECT against the real tomlUnescape() invoked from the real loadRecent():

| Input (as written in file) | Recovered QString | Correct? |
|---|---|---|
| backslash-quote | double-quote char | yes |
| backslash-backslash | single backslash | yes |
| backslash-n | actual LF | yes |
| backslash-t | actual TAB | yes |
| backslash-r | actual CR | yes |
| backslash-x (unrecognized escape) | x (backslash dropped, next char kept literally) | yes -- matches the documented fallback in mainwindow.cpp:191-194 ("Unknown escape: ... keep the character and move on"); this is intentional graceful degradation, not a bug |
| raw UTF-8 (unescaped multibyte text) | same text | yes |

Method note (2nd self-caught tooling bug, for the record): the first pass
of this specific sub-test used printf("RECENT_TOOLTIP: %s\n",
tooltip.toUtf8().constData()). Two cases gave misleading raw output:
- Case 6 (embedded NUL) initially appeared truncated at the NUL
  (".../base1" with ".pdf" missing) -- this was printf's %s stopping at
  the first NUL in the C string, not truncation inside Qt. Rebuilt the
  probe to report QString::size() explicitly and hex-escape non-printable
  bytes; the corrected run shows qstring_len=33 vs. the baseline's 32
  (exactly one character longer, i.e. the NUL is retained as a literal
  in-memory character) and the full escaped text shows the ".pdf" suffix
  is NOT lost inside Qt.
- Case 8's CR-containing entry initially rendered with the terminal cursor
  visibly jumping mid-line (a real carriage return doing what carriage
  returns do to a terminal), which could have been misread as corruption.
  The escaped rendering resolves it as an ordinary, correctly-decoded CR
  character.

Both are noted here so the corrected numbers are trusted and nobody
downstream re-derives the wrong conclusion from the first (misleading) pass.

---

## Finding: embedded NUL in a state-file path -- preserved in memory, but safely rejected by every current call site (not a reachable bug; recorded because the underlying Qt behavior is a latent footgun)

Status: REPRODUCED (both halves -- the QString preservation via the real
loader above, and the QFileInfo/QFile split below via a small standalone Qt
program exercising the identical Qt APIs used by Document::openPath and
rebuildRecentMenu).

Following on from case 6 above: since a raw NUL byte can never legally
appear in a real POSIX filename (the kernel forbids it, along with /), the
in-memory QString with an embedded NUL can never correspond to a real file.
The question is what MERGEN's own existence/open calls do with such a
string. A standalone probe (/tmp/mergen_audit/nultest.cpp, linked only
against Qt6Core, not MERGEN) isolates the underlying Qt behavior:

  QString base = "/tmp/mergen_audit/nul_probe_target";      // real 2-byte file "hi"
  QString withNul = base + QChar(0) + "_SHOULD_NOT_EXIST_SUFFIX";
  QFileInfo::exists(withNul)    -> false
  QFileInfo::exists(base)       -> true
  QFile(withNul).open(ReadOnly) -> TRUE (succeeds!)
    contents read: "hi"   (i.e. it silently opened `base`, not `withNul`)

So at the raw Qt/OS layer, QFileInfo::exists() and QFile::open() disagree
on a NUL-embedded path: exists() reports false, but open() silently
succeeds against the path truncated at the NUL and returns that file's
content with no indication the requested path differed from the one
actually opened (the OS syscalls underneath are NUL-terminated C strings
and Qt does not reject the string before handing it to them).

In MERGEN specifically, this is NOT currently reachable, because every call
site that touches a state-file-derived path checks QFileInfo::exists()
first, and consistently gets false for a NUL-embedded path:
- Document::openPath() (document.cpp:22-25): `const QFileInfo info(path); if
  (!info.exists() || !info.isFile()) return LoadStatus::NotFound;` runs
  before Poppler::Document::load() is ever called.
- rebuildRecentMenu() (mainwindow.cpp:1888): `entry->setEnabled(QFileInfo::exists(path))`
  disables the menu entry outright.
- followPortal() (mainwindow.cpp:922): `if (!QFileInfo::exists(to->path))` gates
  before openPath() is called.

So a NUL-embedded path from a corrupted recent.toml/portals.toml is
rejected as "does not exist" everywhere it matters today. This is recorded
as a latent hazard, not a live bug: the safety is incidental (every current
call site happens to check exists() first) rather than structural (there is
no central sanitization of paths coming off disk). Any future code path
that calls QFile::open()/Poppler::Document::load() on a state-file-derived
path without first checking exists() would silently open whatever real file
happens to sit at the truncated prefix instead of failing -- a same-user
path-confusion bug, not a privilege escalation, since the truncated file is
already one the user's own process can read.

Severity: low (not reachable today). Fix direction: none needed
functionally; if defending in depth is wanted, reject/strip any path
containing a NUL at load time in loadRecent()/loadPortals(), the same way a
NUL byte would be rejected by any real filesystem -- cheap and makes the
safety structural instead of incidental.

---

## Finding: recent.toml's array-boundary detection is whole-file and comment-blind -- a quoted string anywhere between the file's first [ and last ] is parsed as a recent-file entry

Status: REPRODUCED (case 13_bracket_comment_injection.toml above).

File content that triggers it:
```
# [ note: also check "/secret/inbox.pdf" for reference
recent = [
    "/home/user/real.pdf",
]
```

mainwindow.cpp:1795-1800:
```cpp
const int open = text.indexOf(QLatin1Char('['));
const int close = text.lastIndexOf(QLatin1Char(']'));
if (open < 0 || close < open) return;
const QString body = text.mid(open + 1, close - open - 1);
```
text.indexOf('[') scans the ENTIRE FILE for the first [, with no regard for
# comments or the `recent = ` key; lastIndexOf(']') likewise scans the
whole file for the last ]. The quote-scanning loop that follows
(1802-1829) then treats every "..."-delimited token inside that span as a
candidate path, with no awareness of comments, the `recent = ` prefix, or
TOML syntax in general.

Result: loading the file above yields TWO recent-file entries --
/secret/inbox.pdf (from inside the # comment) ahead of the real
/home/user/real.pdf -- confirmed via the real compiled loader
(RECENT_TOOLTIP[qstring_len=17,...]: /secret/inbox.pdf then
RECENT_TOOLTIP[qstring_len=19,...]: /home/user/real.pdf).

This can never happen from MERGEN's own writes (saveRecent() only ever
emits one fixed "# MERGEN recent files" comment line containing no quotes),
so it is not reachable through normal atomic-write-then-read use. It is a
real weakness against a hand-edited, corrupted-by-concatenation, or
deliberately adversarial file: the loader does not actually parse "the
recent array" as a structural element, it parses "the first quoted strings
found anywhere between the outermost brackets of the whole file." Also
confirmed harmless-but-illustrative: 10x-nested garbage brackets
([[[[[[[[[[ ... "/tmp/x.pdf" ... ]]]]]]]]]]) still correctly extract the
one real entry, because nesting depth is never tracked -- only the single
first [ and single last ] positions matter.

Severity: low-medium. Same-user file, so this is not a privilege boundary
-- the realistic impact is a corrupted or merged recent-files list picking
up spurious entries (which merely show up, disabled if the fake path
doesn't exist, or openable if it happens to coincide with something real)
rather than any escalation. Fix direction: parse line-by-line requiring a
specific structural anchor (e.g. only consider text after a line matching
`^recent\s*=\s*\[`, and stop at a line that is just `]`), mirroring the
discipline loadPortals() already uses for line-oriented parsing, rather
than doing a whole-file bracket/quote scan.

---

## Assessment: path injection / traversal (recent.toml and portals.toml paths on read-back)

Status: read-only assessment (code read, one code path confirmed by
inspection).

Paths loaded from either file are used, unvalidated, in: QFileInfo::exists(),
Document::openPath() -> Poppler::Document::load(), and -- for files the
user cannot read directly -- readElevated(), which shells out to
`pkexec <helper> <path>`. Checked readElevated() (mainwindow.cpp:674-679):

```cpp
QProcess pkexec;
pkexec.setProgram(QStringLiteral("pkexec"));
pkexec.setArguments({QStringLiteral(MERGEN_HELPER_PATH), path});
pkexec.setProcessChannelMode(QProcess::SeparateChannels);
pkexec.start();
```

setArguments() with an explicit list passes `path` as a single argv element
via execve()-style argument passing -- there is no shell interpretation
anywhere in this call (contrast with QProcess::startCommand or system()),
so no quote, backslash, semicolon, $(), backtick, or newline in a
state-file path can achieve command injection, regardless of how
adversarial the file is. A path containing ".." is handled exactly like any
other path a reader could type into the file dialog -- QFileInfo/Poppler
resolve it normally, with no special traversal risk beyond "opens whatever
file that relative-looking path actually resolves to," which is true of
any PDF viewer and is not a MERGEN-specific issue. An absolute path outside
$HOME is opened exactly like an absolute path inside it -- again, no
special-casing either way, and since MERGEN runs as the same user with no
elevated default privileges, this is a same-user "can open a file you can
already open" situation, not privilege escalation. The one privilege
boundary in the app (pkexec) requires an interactive polkit prompt the
reader must approve, regardless of what path a state file names.

Conclusion: no injection or meaningful traversal concern. The realistic
risk class for both files, as the brief frames it, is corruption/confusion
(wrong or garbage entries shown, as in the finding above), not privilege
escalation -- confirmed rather than assumed.

---

## Finding (headline): loadPortals()'s `ends` counter makes correctness depend on key order within a portal block; several plausible malformations cause silent, total data loss of the affected portal -- CONFIRMED with real compiled code, end to end

Status: REPRODUCED. Method: real MainWindow constructed via `probe
markportal <pathA> <pageA> <pathB> <pageB>`, which (1) constructs
MainWindow (runs the real loadPortals() over a planted adversarial
portals.toml), (2) opens a real PDF, jumps to a page, and fires the real
"Mark portal" QAction (found by matching its `Ctrl+M` QKeySequence via
`QAction::shortcut()` -- a public Qt API, no access-control workaround
needed since QAction::trigger() is public regardless of which private slot
it's connected to), (3) repeats with a second real PDF to complete one new,
known-good portal, which calls the real savePortals(). Because
savePortals() serializes the *entire* `m_portals` vector, the resulting
file is byte-exact evidence of what the real loadPortals() put into memory
from the adversarial input, with the known-good appended portal as a
cross-check that the harness and the save path both worked. Every case
below ran to completion, exit code 0, no crash, no hang.

Method note (3rd self-caught bug, for the record): the first pass of this
test used the *same* PDF file for both marks, relying on scrollToPage() to
change the recorded page between them. That failed silently: scrollToPage()
starts a ~200ms QPropertyAnimation (MZ.md's "Motion" -- mainwindow.cpp calls
through to PageView's animateScrollTo) and returns immediately; `Ctrl+M` was
triggered before any event-loop turn let the animation progress, so
`currentPage()` was still 0 both times. Since both marks then had identical
(hash, page), markPortal()'s own same-place-cancels rule fired
(mainwindow.cpp:884-889) and `savePortals()` was never even called -- the
output files were byte-identical to the input, which could have been
misread as "the corrupted portal round-tripped perfectly." Switched to two
different real PDFs (different content hash) so the cancel path can never
trigger regardless of animation timing; diagnostics (window title,
`currentPage()`, action-enabled state) were added to the probe and used to
confirm the fix before trusting any result below.

### The bug

mainwindow.cpp:804-847:
```cpp
void MainWindow::loadPortals() {
    ...
    Portal current;
    int ends = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line == QLatin1String("[[portal]]")) {
            if (ends == 2) { m_portals.append(current); }
            current = Portal();
            ends = 0;
            continue;
        }
        ...
        PortalEnd &end = ends == 0 ? current.a : current.b;
        if (key == QLatin1String("hash")) { end.hash = value; }
        else if (key == QLatin1String("path")) { end.path = value; }
        else if (key == QLatin1String("page")) { end.page = value.toInt(); ++ends; }
    }
    if (ends == 2) { m_portals.append(current); }
}
```
Which struct field (`current.a` or `current.b`) a `hash`/`path`/`page` line
writes into is decided by `ends`, and `ends` only advances when the literal
key `page` is seen -- so which end a key lands in silently depends on how
many `page` keys have appeared *so far in this block*, not on which end the
key was actually written for. `savePortals()` itself always writes
`hash`/`path`/`page` in that fixed order once per end (mainwindow.cpp:858-864),
so this is only reachable via a hand-edited or corrupted file, not through
MERGEN's own round-trip -- exactly the adversarial-file scenario the brief
asks about.

### Confirmed outcomes, one file content each (all under `[[portal]]`)

| Case | First end as written | Second end as written | Outcome (real, observed) |
|---|---|---|---|
| Baseline (correct order: hash, path, page x2) | `hash=AAAA1111 path=decoyA.pdf page=3` | `hash=BBBB2222 path=decoyB.pdf page=7` | Round-trips perfectly, byte-for-byte |
| **B** -- `page` before `hash`/`path` in end 1 | `page=3` then `hash=AAAA1111` `path=decoyA.pdf` | `hash=BBBB2222 path=decoyB.pdf page=7` | Saved back as end A = `hash="" path="" page=3`, end B = `hash=BBBB2222 path=decoyB.pdf page=7`. **AAAA1111/decoyA.pdf is silently destroyed** -- overwritten in transit into `current.b`, then itself overwritten by the real end-2 data. The portal survives but end A becomes a nameless stub (empty hash+path, page=3 only) |
| **C** -- `page` key missing from end 1 entirely | `hash=AAAA1111 path=decoyA.pdf` (no page) | `hash=BBBB2222 path=decoyB.pdf page=7` | `ends` never leaves 0 until the `page=7` line, so it reaches only 1 by EOF. **Entire portal silently dropped** -- both fully-specified hash/path pairs lost, nothing written back |
| **D** -- duplicate `page` key in end 1 | `hash=AAAA1111 path=decoyA.pdf page=3 page=99` | `hash=BBBB2222 path=decoyB.pdf page=7` | `ends` overshoots to 3 by EOF (each `page` line increments it once, regardless of which end it actually wrote). `if (ends == 2)` is false. **Entire portal silently dropped** |
| **E** -- three complete ends, no second `[[portal]]` header | `hash=AAAA1111/decoyA.pdf/page=1`, `hash=BBBB2222/decoyB.pdf/page=2`, `hash=CCCC3333/decoyC.pdf/page=3` (three full, individually well-formed ends in one block) | -- | `ends` reaches 3 (end 3 silently overwrites `current.b`, clobbering end 2's data). `if (ends==2)` false. **Entire portal, all three ends' worth of well-formed data, silently dropped** |
| **F** -- extra unrecognized key (`label = "..."`) between `path` and `page` in end 1 | `hash=AAAA1111 path=decoyA.pdf label=... page=3` | `hash=BBBB2222 path=decoyB.pdf page=7` | **Round-trips perfectly** -- an unrecognized key matches none of the `if/else if` branches and does not touch `ends`, so it is silently and harmlessly ignored. Confirms the fragility is specific to the `page`-key count/order, not to extra content in general |
| **G** -- negative and overflowing page numbers, correct key order | `hash=AAAA1111 path=decoyA.pdf page=-5` | `hash=BBBB2222 path=decoyB.pdf page=999999999999` | Round-trips end A's `page=-5` **exactly** (negative ints pass through `toInt()` unchanged, no validation). End B's `path`/`hash` preserved correctly but **`page` becomes `0`** -- confirms `QString::toInt()` returns 0 (not clamped, not an error surfaced) when the literal text is out of `int` range |
| **H** -- half-written: only one end, `ends==1` at EOF | `hash=AAAA1111 path=decoyA.pdf page=3` | (nothing) | **Entire portal silently dropped** -- this one is *documented, intended* behavior per the comment at mainwindow.cpp:815-816 ("A portal needs both ends; a half-written one is dropped"); confirmed working as designed |
| **I** -- corrupted block (case B's content) immediately followed by a second, well-formed `[[portal]]` block | (B's corrupted content) | -- | **The second block is completely unaffected** -- `CCCC3333`/`DDDD4444` round-trip perfectly. Confirms `[[portal]]` unconditionally resets `current = Portal(); ends = 0;` (mainwindow.cpp:820-821) regardless of the previous block's `ends` value, so corruption never crosses a portal boundary |

### Confirm / refute / extend, as the brief asked

- **Confirmed:** correctness of a portal block depends on the exact key
  order and exact count of `page` keys within that block; this is
  demonstrably not robust to reordering, omission, or duplication.
- **Confirmed:** a duplicate `page`, a missing `page`, or a third full end
  inside one block causes the *entire* portal (both ends, all fields) to be
  silently discarded -- with two individually well-formed, fully-specified
  ends' worth of data (cases C and E) lost for the cost of one stray or
  missing line.
- **Refuted (the "mixing two portals together" concern):** corruption does
  **not** cross a `[[portal]]` boundary. Every `[[portal]]` line
  unconditionally resets both `current` and `ends`, confirmed empirically
  in case I. Malformed content only ever corrupts or destroys the block it
  appears in.
- **Extended:** the case-F contrast shows the fragility is narrowly about
  the literal key `page` (the only key that increments `ends`) -- any
  amount of *other* unrecognized content inside a block is silently and
  harmlessly ignored, which is a real (if narrow) piece of robustness the
  parser does have.
- The write side does not help a reader recover from this: the very next
  `markPortal()` completion after loading a file that hit any of B/C/D/E
  calls `savePortals()`, which serializes `m_portals` -- whatever
  loadPortals() silently dropped or corrupted is now **permanently gone**,
  overwritten in the one file that held it, with no error ever shown
  (matching MZ.md's "never an error the reader sees" -- but here "never an
  error" also means "never told this happened to their portals" versus a
  stricter parser that could have recovered the well-formed data).

**Severity: high.** This is not a crash or a hang (both confirmed absent in
every case, matching MZ.md's core promise), but it is silent, permanent,
unrecoverable loss of reader-created data -- exactly the thing §8 says a
portal is (something "the reader creates it deliberately... nothing about a
reader's position is ever written without them asking for it") on the
strength of one misplaced or duplicated line in a hand-edited or
corrupted-in-transit file. Given the brief's own framing that portals are
new/unaudited code, and that a single stray line (very plausible from a
manual edit, a bad merge, or a future contributor adding a field and
guessing at the format) destroys reader-authored data with zero signal,
this crosses from "malformed file handled gracefully" into "malformed file
handled destructively but quietly."

**Fix direction:** stop inferring which end a key belongs to from a
same-block counter tied to one specific key. Parse each end as an explicit
unit -- e.g. require and consume exactly three lines (`hash`, `path`,
`page`) per end in a fixed grammar and reject/skip the whole block on any
deviation rather than guessing, or key on an explicit end index if the
format grows a marker (e.g. `[[portal.a]]` / `[[portal.b]]` instead of two
anonymous triples inside `[[portal]]`), or at minimum stop keying the
`current.a`-vs-`current.b` choice off `ends` and instead track which named
fields have already been set for the end currently being filled (start a
new end only when a key repeats one already set on the current end, not
only on seeing `page` specifically). Any of these would turn cases B/C/D/E
from "silently destroy reader data" into "skip exactly the malformed end,"
consistent with the existing, correctly-implemented half-written-portal
policy in case H.

---

## Negative result (good news): tomlEscape()/tomlUnescape() round-trip correctly -- no failures found

Status: REPRODUCED, two independent methods.

**Method 1 -- REPLICA fuzzer.** tomlEscape()/tomlUnescape() have internal
linkage (anonymous namespace in mainwindow.cpp) and cannot be linked from an
external translation unit, so `/tmp/mergen_audit/escape_fuzz.cpp` includes
the function bodies verbatim, extracted at audit time via
`sed -n '140,198p' src/mainwindow.cpp > toml_escape_replica.inc` (md5sum
`e18f70a6e4dcf625826803312d923784`, 59 lines, matches the source read
directly at those line numbers) -- nothing hand-transcribed. Ran 20 targeted
adversarial cases plus 200,000 randomized strings (0-40 chars, drawn from an
alphabet weighted toward backslash/quote/CR/LF/tab plus arbitrary BMP
scalars including other control characters) through `unescape(escape(s)) ==
s`.

**Result: 0 failures out of 200,020 cases.** Every targeted case passed,
including the specific one named in the brief -- a string that already
contains a literal two-character `\n` sequence (backslash followed by the
letter n, not a real newline) survives alongside a real embedded newline in
the same string without either being confused for the other. Also passing:
lone trailing backslash, lone quote character, NUL embedded mid-string, all
0x00-0x1F control characters back to back, astral-plane Unicode (emoji,
combining characters spanning a surrogate pair in UTF-16).

**Method 2 -- REAL end-to-end, both directions, through the actual compiled
code.** Created five real files on disk with adversarial bytes in the
*filename itself* (not just file content) and opened each through the real,
public `MainWindow::openPath()`:

| Filename (raw byte in the name) | Byte length |
|---|---|
| `weird<TAB>tab.pdf` | 36 |
| `weird"quote.pdf` | 38 |
| `weird<LF>newline.pdf` (confirmed via `od -c` to contain a real 0x0a byte, not a two-char escape) | 40 |
| `weird\backslash.pdf` | 42 |
| `weird'apostrophe.pdf` | 44 |

Each `openPath()` call runs the real `pushRecent()` -> `saveRecent()` ->
real `tomlEscape()` -> real `QSaveFile::commit()`. Inspecting the resulting
`recent.toml` with `od -c` confirms every special byte was correctly
escaped in the file on disk (tab -> `\t`, `"` -> `\"`, the embedded real
newline -> `\n` [critically preventing it from becoming a literal
line-break that would have corrupted the file's own line structure],
backslash -> `\\`, apostrophe left unescaped since it needs no escaping in
this format).

Reconstructing `MainWindow` fresh against that exact file (real
`loadRecent()` -> real `tomlUnescape()`) and reading the recent-menu
tooltips back recovers all five filenames with **byte-exact length matches**
against the true on-disk filenames (36/38/40/42/44 recovered vs.
36/38/40/42/44 original, verified independently via `wc -c` on the actual
filenames, not just eyeballed).

**Conclusion:** the escape/unescape functions themselves are solid --
tested here far more thoroughly than the informal "enough for the one thing
MERGEN writes" comment at mainwindow.cpp:139 implies is expected, and they
hold up. This is a real, checked negative result, not an absence of
testing: the weaknesses found in this audit are in the surrounding
line/array-structure parsing (the `ends` counter, the whole-file
bracket/quote scan), not in the character-level escaping. No fix needed
here.

---
## Environment note: real recent.toml changed mid-audit -- not caused by this audit, not restored

Observed partway through this audit (session resumed after an unrelated
session-limit interruption): the real `~/.local/state/mergen/recent.toml`
no longer matches the pre-audit backup taken at
`/tmp/mergen_audit_backup/mergen_real_backup/recent.toml` (different size,
different content, different hash). `portals.toml` is byte-identical to its
backup -- unaffected.

Diagnosed rather than assumed: the new content lists paths like
`/tmp/mgtest/link.pdf`, `/tmp/mgtest/huge.pdf`, `/tmp/mgtest/many.pdf`,
`/tmp/mgaudit/noperm.pdf`, `/tmp/mgaudit/plain.pdf`, `/tmp/mgaudit/slink.pdf`
-- a naming convention (`mgtest`/`mgaudit`, and test names suggesting
symlink/permission/large-file probes) that appears nowhere in this audit's
own work, which used `/tmp/mergen_audit/pdfs/*` and `XDG_STATE_HOME`-scoped
scratch directories exclusively for every `openPath()` call (verified
against this file's own command log above -- every invocation touching a
real MainWindow explicitly set `XDG_STATE_HOME=/tmp/mergen_audit/scratch*`).
Further evidence this is environmental rather than self-inflicted: the
*original* backup, taken before this audit did anything, already contained
paths like `<scratch>/outline.pdf` -- i.e. this
file was already being written by other concurrent agent activity in this
shared environment before this audit started. `recent.toml` is a rolling,
frequently-mutated cache with no path validation tying it to "this session
only," so any other process opening real PDFs through the real `mergen`
binary (unsandboxed, no `XDG_STATE_HOME` override) legitimately overwrites
it -- consistent with everything else this audit found about how the file
behaves.

**Action taken: none.** Restoring the pre-audit backup now would overwrite
what appears to be another session's legitimate concurrent state, and the
backup itself was not a clean pre-existing baseline to begin with (it
already carried other agents' fixture paths). Per MX.md section 7 / MZ.md
section 8, `recent.toml` holds nothing sensitive (ten most-recent paths, no
settings, no credentials) and is explicitly self-healing (`pushRecent()`
prunes and rewrites it on every open), so leaving it as found carries no
real risk. The pre-audit backup remains available at
`/tmp/mergen_audit_backup/mergen_real_backup/recent.toml` if the user wants
it. `portals.toml`, which does hold reader-authored data, is unaffected and
was not touched.

Noted here for transparency and so this observation is not confused with
this audit's own findings above.

---
## recent.toml and portals.toml under large/adversarial-scale input -- REPRODUCED, all real compiled code

Method: real `MainWindow` construction (`probe construct`/`probe recentmenu`)
against generated files, `QElapsedTimer` for wall time inside the process
and `/proc/self/status` `VmHWM`/`VmRSS` for peak/final memory (no
`/usr/bin/time -v` or `strace` package available on this system -- these
are read directly from the kernel via `/proc`, equally authoritative).
Files written with Python for speed; `/tmp` here is tmpfs (3.5G total),
so file sizes were kept within that budget, generating and deleting one
large file at a time.

### recent.toml at scale (memory is TRANSIENT -- freed after load, since only 10 survive)

| Input | Wall time (`probe`'s own timer) | Peak RSS (VmHWM) | Final RSS | Entries kept |
|---|---|---|---|---|
| 1 MB (14,444 valid entries) | 123 ms | 56 MB | 56 MB | 10 |
| 50 MB (705,790 valid entries) | 319 ms | 247 MB | 55 MB | 10 |
| 500 MB (6,959,876 valid entries) | 3.3 s | **1.96 GB** | 56 MB | 10 |
| 100M-character single quoted "path" value (~100 MB file, one entry) | 3.65 s | 815 MB | 251 MB | 1 (the whole 100,000,000-char string, confirmed **stored and returned intact** -- tooltip UTF-8 byte count = 100,000,004, exactly matching 100,000,000 `A`s + `.pdf`) |

No crash, no hang, in any case (all well inside a 60-120s timeout). Peak
memory scales roughly 4x the file size (`file.readAll()` QByteArray +
`QString::fromUtf8` UTF-16 conversion of the whole file + the `body`
substring copy via `text.mid()`, mainwindow.cpp:1789-1800, all coexist
briefly) and is **released back to baseline (~55 MB) once `loadRecent()`
returns**, since only `kRecentLimit` (10) entries are ever retained in
`m_recent`. `kRecentLimit` is enforced correctly regardless of input size --
confirmed already in the adversarial matrix above (15-entry case) and here
again at 500 MB / ~7 million candidate entries. There is **no length cap on
an individual entry** -- the 100-million-character single path is accepted
whole, with no truncation, no rejection, and no validation; only the entry
*count* is bounded.

### portals.toml at scale (memory is PERSISTENT -- no cap exists, everything loaded stays resident)

Code search first, to settle whether any cap exists at all:
```
grep -n "kPortal\|MAX_PORTAL\|portalLimit\|m_portals.size()\|m_portals.count()" src/mainwindow.cpp src/mainwindow.h
```
returns nothing except the unrelated `markPortal` symbol -- there is no
`kRecentLimit`-equivalent constant for portals anywhere in the codebase, and
no size check anywhere in `loadPortals()`. Confirmed empirically:

| Input | Wall time | Peak/final RSS | Result |
|---|---|---|---|
| 1,000,000 well-formed `[[portal]]` blocks (124,115,560-byte file) | 6.1 s (construct only) | **381.8 MB, and it stays there** (VmHWM == VmRSS, i.e. not transient -- this is live, permanently-referenced data in `m_portals`, unlike recent.toml's freed spike) | Loads without crash or hang |
| Same file + one real `markPortal()` completion (via `probe markportal`, which also does two real document opens) | 5.7 s total (load 1M + open 2 real PDFs + 2 real `Ctrl+M` triggers + save 1,000,001 portals back to a ~124 MB file) | -- | `grep -c '\[\[portal\]\]' portals.toml` on the output = **exactly 1,000,001** -- all 1,000,000 original portals survived intact (spot-checked: first portal `h0`/`h0_b` unchanged, last portal is the newly-created `base1.pdf`/`base2.pdf` one) plus the new one. **No cap, no silent drop, at any tested scale** |
| 100M-character single value in one portal end's `hash` field (100,000,094-byte file, otherwise well-formed) | 1.86 s | 814 MB | Loads without crash or hang |

**Confirmed: portals have no cap of any kind** -- not a count cap
(unlike `recent.toml`'s explicit, deliberate `kRecentLimit = 10`), not a
per-field length cap, nothing. Every portal ever created (or present in a
corrupted/adversarial file) is loaded into memory and kept there for the
life of the window, and **every subsequent `markPortal()` completion
rewrites the entire list to disk** (`savePortals()` serializes all of
`m_portals` unconditionally, mainwindow.cpp:849-868) -- so both the memory
footprint and the cost of creating *the next* portal grow without bound as
the file grows. `followPortal()`'s matching loop (mainwindow.cpp:904-934) is
also a plain O(n) linear scan over every portal on every `Ctrl+J`, with no
index or early-exit structure.

**Severity: medium.** Not a crash and not reachable at any size a real
reader would organically produce by hand (nobody makes a million portals
one keystroke at a time) -- but a corrupted or adversarially-large file
(the exact threat model this audit was asked to assess) degrades the
application's steady-state memory and every-future-write cost
proportionally, with no ceiling, in deliberate contrast to the cap
`recent.toml` was given. This is a real gap relative to `recent.toml`'s own
design (which clearly *did* consider "how big should this be allowed to
grow" and answered it), not a hypothetical.

**Fix direction:** decide and enforce a reasonable ceiling on portal count
at load time (drop the oldest or refuse to load beyond N, mirroring
`kRecentLimit`'s pattern), or at minimum switch `followPortal()`'s matching
to something that doesn't degrade linearly if large counts are meant to be
supported. Given portals are reader-authored and deliberate (unlike the
passively-accumulated recent list), the more proportionate question is
whether an explicit cap belongs in the spec at all -- but the code today
enforces none, and MZ.md does not mention one either.

---
## Finding (headline, required deliverable): MZ.md's "survives a rename" claim is TRUE only for the end you are standing on -- REFUTED for the far/target end, which is located purely by stale stored path, never by its own hash

Status: REPRODUCED, real compiled code end-to-end (real `MainWindow`, real
`openPath()`, real `Ctrl+M`/`Ctrl+J` `QAction::trigger()`, real file
renames on disk between runs).

MZ.md section 8's exact words: "Documents are identified by a hash of their
bytes rather than their path, so a portal survives the file being moved and
does not silently attach itself to a different document that happens to
occupy the old name."

Read `followPortal()` in full (mainwindow.cpp:897-934) to check what "the
file being moved" actually does to each end:
```cpp
void MainWindow::followPortal() {
    ...
    const QString hash = m_doc->contentHash();   // freshly computed from the CURRENT doc's bytes
    const int page = m_view->currentPage();
    for (const Portal &portal : m_portals) {
        ...
        if (portal.a.hash == hash && portal.a.page == page) { from = &portal.a; to = &portal.b; }
        else if (portal.b.hash == hash && portal.b.page == page) { from = &portal.b; to = &portal.a; }
        if (!from) continue;

        if (to->hash == hash) { m_view->scrollToPage(to->page); return; }  // same-doc case
        if (!QFileInfo::exists(to->path)) {                                 // <-- STALE STORED PATH
            m_view->setNotice(tr("The other end of this portal is in %1, which is not there.")...);
            return;
        }
        ...
        openPath(to->path);   // <-- also the STALE STORED PATH, never to->hash
        ...
    }
}
```
Confirmed by grepping the whole file for every use of `contentHash`/`.hash
==`/`hash ==` (mainwindow.cpp:874, 884, 901, 907, 910, 918 -- the complete
list): **nothing anywhere ever re-resolves a portal end's stored `path` by
searching for a document with a matching `hash`.** The hash is compared
only two ways: (1) against the *currently open* document, freshly computed
from its live bytes, to decide whether you're standing on `a` or `b`
("near-end matching"); (2) `to->hash == hash` as a degenerate check for
"both ends are the same already-open document." The far end's `path` field
is the *only* thing ever used to locate and open it -- exactly the
mechanism MZ.md says the hash exists to avoid depending on.

### Test 1 -- near-end rename (the document you're standing on)

Portal made between `A.pdf` (hash `1c40ef5d...`) page 0 and `B.pdf` (hash
`a09f295d...`) page 0. **Renamed `A.pdf` -> `A_renamed.pdf`** (identical
bytes/hash). Fresh `MainWindow`, opened `A_renamed.pdf`, page 0, triggered
the real `Ctrl+J` action:
```
OPEN_RETURNED   title='MERGEN ///-- A_renamed.pdf --\\\ MEGAS'
FOLLOW_TRIGGERED title='MERGEN ///-- B.pdf --\\\ MEGAS'
```
**Works exactly as claimed.** The title changes to `B.pdf` -- the jump
succeeds, because the near end is matched against the live document's
freshly-computed hash, completely independent of what path it was opened
from.

### Test 2 -- far-end rename (the document you're trying to reach)

Fresh setup, same two files/hashes. This time **renamed `B.pdf` ->
`B_renamed.pdf`** instead (`sha256sum` confirms identical content/hash:
`a09f295d...` both before and after). Left `A.pdf` at its original path.
Fresh `MainWindow`, opened the *unchanged* `A.pdf`, page 0, triggered the
real `Ctrl+J` action:
```
OPEN_RETURNED    title='MERGEN ///-- A.pdf --\\\ MEGAS'
FOLLOW_TRIGGERED title='MERGEN ///-- A.pdf --\\\ MEGAS'
```
**The title does not change.** The follow silently fails: `to->path` is
still the portal's stored `.../B.pdf`, which no longer exists (`B.pdf` was
renamed), so `QFileInfo::exists(to->path)` is false and `followPortal()`
takes the "kept, not deleted... which is not there" branch and returns --
even though `B_renamed.pdf`, with the exact hash the portal is already
holding, sits right next to it on disk. The reader sees "The other end of
this portal is in B.pdf, which is not there" -- true of the literal old
name, but the phrasing (and the code comment above it, "the file may simply
not be mounted today") frames this as a *removable-media-not-present*
scenario, not a *the file was renamed on this very machine* scenario --
which is precisely the case the hash-based design was introduced to solve,
per MZ.md's own sentence.

### Confirm / refute, precisely

- **Confirmed:** a portal survives its *near* end (the document you invoke
  `Ctrl+J` from) being renamed. Content hashing does exactly what's claimed
  for this direction.
- **Refuted:** a portal does *not* survive its *far* end (the document
  `Ctrl+J` is trying to reach) being renamed. The stored hash for that end
  is carried in the file and even correctly matches the renamed file's
  actual bytes -- it is simply never *used* for that end; only the stale
  path is. "Survives the file being moved" is true for one participant in
  the portal and false for the other, which is a meaningfully narrower
  guarantee than the spec's sentence states, since every portal has both a
  near and a far end depending on which side you approach from -- the same
  portal that survives a rename approached from `A` silently breaks when
  approached from `B` after `B` itself (not `A`) is the one renamed.
- The `path` field is not dead weight -- it is exactly what lets MERGEN
  avoid a filesystem-wide hash search to open the far document (opening a
  document requires a real path; poppler/`Document::openPath` cannot open
  "the file with this hash," only a path). The gap is narrower than "the
  hash does nothing": it is that **nothing ever refreshes a stale `path`
  once a document with the matching hash is seen again at a new location.**

**Severity: high.** This directly contradicts a specific, testable claim in
the project's own constitution about the entire reason portals hash
content instead of storing paths, for exactly the scenario (`mv`/rename)
the sentence names. A reader who renames the far side of a portal --
plausible: renaming a downloaded revision, a file-manager rename, a sync
client normalizing a name -- gets a portal that looks intact (both ends
still listed, `Ctrl+M` would even recreate it as a *second*, working,
duplicate portal without removing the broken one) but silently fails with
a message that reads like the file is unavailable rather than renamed.

**Fix direction:** the natural place to close this gap is exactly the
event MERGEN already has: every successful `openPath()` computes a fresh
`contentHash()` for the newly opened document (needed for `markPortal()`
regardless). Adding "if this hash matches any portal end whose stored path
differs from the one just opened, update that end's stored path and
persist it" during `openPath()` would self-heal every portal end the reader
happens to open normally, and would make `followPortal()`'s existing
`QFileInfo::exists(to->path)` check meaningful again after a rename instead
of chasing a name that's gone. This does not require a filesystem-wide
search -- it only needs to notice renames the reader has already surfaced
by opening the file themselves, which is the realistic way a rename would
be discovered anyway.

---
## Finding (headline): write-side failures are silent everywhere except one path, where an uncaught SIGXFSZ crashes the whole application -- and even on the graceful paths, the reader is unconditionally told "Portal made" whether or not anything was actually saved

Status: REPRODUCED, five distinct real failure conditions, all against the
real compiled `markPortal()` -> `savePortals()` path via `probe markportal`
(real `Ctrl+M` `QAction::trigger()` twice, completing one real portal, the
same harness used for the `ends`-counter and scale findings above).

### Cases 1-3: directory/permission obstructions -- silent failure, no crash, no corruption

| Case | Setup | Result |
|---|---|---|
| Read-only state dir | `mkdir mergen; chmod 0500 mergen` (dir pre-exists, so `QDir().mkpath()` is a no-op that succeeds; the *file* create inside it is what fails) | `exit=0`. Both `Ctrl+M` triggers complete normally (`MARK1_TRIGGERED`, `MARK2_TRIGGERED` both printed). Directory stays empty afterward -- no `portals.toml` ever created |
| Dangling symlink at the `mergen` path (`ln -s /nonexistent/target/dir mergen`) | `QDir().mkpath()` on a path whose last component is a symlink to a nonexistent target fails (can't create a directory where a symlink already occupies the name) | `exit=0`, no crash. Symlink remains exactly as created, still dangling; nothing is written anywhere |
| A regular file occupies the `mergen` path (blocks `mkpath` entirely -- can't create a directory where a plain file exists) | `echo "I am a file..." > mergen` | `exit=0`, no crash. The blocking file is completely untouched afterward (`cat` shows the original text, unmodified) -- confirms `savePortals()`/`saveRecent()` never attempt to force past an obstruction by deleting/overwriting a non-directory in their way |

All three: **exactly matches MZ.md's "never an error the reader sees"** at
the crash/hang/visible-dialog level -- the application does not notice or
report the failure to the reader in any way, it just silently fails to
persist.

### Case 4: RLIMIT_FSIZE exceeded during the write -- CRASHES THE WHOLE APPLICATION (uncaught SIGXFSZ)

`(ulimit -f 0; ... ./probe markportal ...)` -- caps the process's maximum
file size at 0 blocks, so any real write to any file immediately exceeds
the limit. Result:
```
/usr/bin/bash: ... File size limit exceeded (core dumped)
raw exit status: 153   (= 128 + 25; kill -l XFSZ = 25, confirmed)
```
**The process is killed outright by an uncaught `SIGXFSZ`, with a core
dump** -- not a graceful `QSaveFile::open()`/`write()` failure return. Qt
does not install a `SIGXFSZ` handler, and neither does MERGEN
(`grep -rn "SIGXFSZ\|sigaction\|signal(" src/` -- nothing), so the
kernel's default disposition applies: terminate. This happens the instant
`write()` on the growing temp file would exceed the limit, which for a
real reader corresponds to hitting a **process-level `RLIMIT_FSIZE`**
(settable via `ulimit`, ` systemd` unit `LimitFSIZE=`, or a distro/container
default) -- not the same condition as disk-full, but a real, POSIX-standard
way for a single `write()` to fail that this codebase does not handle any
differently from disk-full, and the outcome is categorically worse: instead
of "portal silently not saved," the reader's **entire MERGEN session ends**,
with the open document, any unsaved portal, and anything else in memory
gone. This is a direct, reproducible counterexample to "never an error the
reader sees" -- a crash is the most visible error there is.

### Case 5: genuine disk-full (ENOSPC) -- reproduced via an unprivileged mount namespace, 4KB tmpfs -- graceful, atomicity holds, but the false-success message still applies

No root/mount privileges were available or used for any other part of this
audit; this one case needed a true full filesystem to test ENOSPC
specifically (as distinct from case 4's RLIMIT_FSIZE), obtained via
`unshare --map-root-user --mount --user` (unprivileged user+mount
namespaces, confirmed enabled on this kernel) to mount a **4 KiB tmpfs**
at a scratch path with no other privilege elevation, then ran the same
`probe markportal` inside that namespace with `XDG_STATE_HOME` pointed at
it. Sequence of writes that actually happen during the harness (`openPath`
x2 each trigger `pushRecent`->`saveRecent`; the second `Ctrl+M` triggers
`savePortals`):

```
exit=0   (no crash)
recent.toml:  "# MERGEN recent files\nrecent = [\n    \"/tmp/.../base1.pdf\",\n]\n"   (75 bytes)
portals.toml: <does not exist>
```
The **first** `saveRecent()` (after opening `base1.pdf` alone, 75 bytes)
fit in the 4 KiB budget and succeeded. The **second** `saveRecent()` (after
opening `base2.pdf`, which should have produced a 2-entry, ~115-byte file)
ran out of space and failed -- and critically, **the file was left at its
previous successful 1-entry state, not corrupted, truncated, or emptied**.
The final `savePortals()` (which needed room for a new anonymous
`O_TMPFILE` inode on top of what `recent.toml` already occupied) failed
completely, and **no `portals.toml` was ever created** -- not a partial or
corrupt one, none at all.

This is exactly the atomicity property earning its keep: because
`QSaveFile` never touches the destination path until the temp
inode is fully written and `linkat`-published, a write that fails partway
through **cannot** leave a truncated or corrupted file at the real
filename -- worst case is "still has the last version that did fit," which
is graceful and matches "never an error the reader sees" at the
crash/corruption level. This is a genuine, confirmed strength of the
`QSaveFile`-based design, worth crediting explicitly -- it is the reason
case 5 degrades gracefully while case 4 (a different failure mechanism
entirely -- a signal, not a return value) does not.

### The common thread: "Portal made" is shown unconditionally, success or not

`markPortal()` (mainwindow.cpp:870-895) contains no branch on whether the
save worked:
```cpp
m_portals.append({*m_pendingEnd, here});
m_pendingEnd.reset();
savePortals();                                          // <-- return value is void; commit()'s
                                                          //     success/failure is discarded
m_view->setNotice(tr("Portal made. Ctrl+J follows it.")); // <-- shown regardless
```
Every one of cases 1, 2, 3 and 5 above completes `markPortal()` normally
(confirmed by `MARK2_TRIGGERED` printing, meaning the function returned
without throwing/crashing) with the write silently having failed --
meaning **the reader is told "Portal made" in every one of those four
scenarios, and the portal exists only in that session's memory**: if the
reader closes MERGEN (or it crashes for any unrelated reason) before any
*later*, successful `savePortals()` call, the portal they were just told
was made is gone, permanently, with no error ever shown at the time it
mattered. `saveRecent()`/`pushRecent()` have the identical structural gap,
though the user-visible stakes are lower there (no explicit "saved" message
is shown for the recent list at all -- the open itself is the visible
action -- so there's no *false claim*, just silent non-persistence).

### Severity and fix direction

**Case 4 (RLIMIT_FSIZE / SIGXFSZ crash): high.** A same-user, non-adversarial,
realistic condition (a `systemd` unit or container with a
file-size limit, or a reader's own restrictive `ulimit`) takes down the
whole application instead of failing one save. **Fix direction:** either
have MERGEN install a no-op/ignoring handler for `SIGXFSZ` (`signal(SIGXFSZ,
SIG_IGN)`, standard practice for exactly this situation -- the subsequent
`write()` then correctly returns `-1`/`EFBIG` instead of raising the signal),
or -- more simply, since this is a one-line fix with no downside -- do this
once at startup and let `QSaveFile`'s existing error path handle the rest.

**Cases 1, 2, 3, 5 (silent write failure, no crash): medium**, specifically
because of the false "Portal made" message -- not because silent recovery
is wrong (that is exactly the documented design intent), but because
*telling the reader a specific, false success fact* is a stronger claim
than "stayed quiet about a background failure." **Fix direction:** check
`QSaveFile::commit()`'s boolean return in both `savePortals()` and
`saveRecent()`, and have `markPortal()` show a different, honest notice
("Portal made, but could not be saved") when it returns false -- a small
change that preserves the no-dialogs, no-nagging philosophy (still just the
existing notice bar, still dismissed the same way) while not asserting
something that didn't happen.

---
## Finding: recentFilePath() and portalFilePath() resolve differently under a relative XDG_STATE_HOME -- confirmed, but at the cost of a real backup-and-restore incident (disclosed in full)

Status: REPRODUCED. **Methodology incident, disclosed:** this specific test
caused a real write to the actual `~/.local/state/mergen/portals.toml` --
restored immediately from the pre-audit backup, confirmed byte-identical
afterward (`md5sum` match, `diff` clean). Recorded here in full rather than
quietly fixed, per this audit's own read-only mandate. No other test in
this audit touched the real files (all others used absolute
`/tmp/mergen_audit/scratch*` paths throughout, as shown in every command
above); this is the one exception, and it happened precisely *because* the
test's purpose was to probe what happens when `XDG_STATE_HOME` is
non-absolute -- the failure mode under test was the same one that caused
the incident.

**The code.** The two loaders resolve their file's directory two different
ways:
```cpp
// recentFilePath(), mainwindow.cpp:1774-1780 -- hand-rolled, no validation
QString MainWindow::recentFilePath() {
    QString base = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/state");
    }
    return base + QStringLiteral("/mergen/recent.toml");
}

// portalFilePath(), mainwindow.cpp:799-802 -- delegates to Qt
QString MainWindow::portalFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
           QStringLiteral("/mergen/portals.toml");
}
```
The XDG Base Directory Specification requires `XDG_STATE_HOME` to be an
absolute path and says a relative value "should be considered the same as
if it was not set" -- `QStandardPaths` on Linux follows this and ignores a
non-absolute value, falling back to the compiled-in default
(`~/.local/state`). `recentFilePath()`'s hand-rolled version has no such
check: `qgetenv("XDG_STATE_HOME")` is used exactly as given as long as it's
non-empty, with no `QDir::isAbsolutePath()` guard.

**Confirmed empirically** (against `/tmp/mergen_audit/scratch_relative2`,
`cd`'d into first, with `XDG_STATE_HOME=relsub` -- deliberately relative):
- `recentFilePath()` resolved to `./relsub/mergen/recent.toml`, relative to
  the process's **current working directory** -- confirmed present there
  after a real `openPath()`-triggered `saveRecent()`.
- `portalFilePath()` did **not** honor `relsub` at all. The subsequent real
  `markPortal()` write landed at the actual default
  `~/.local/state/mergen/portals.toml` -- confirmed by exact `mtime` match
  between the write and the corrupted file's timestamp, and by the file's
  new content matching what the test would produce.

So under a relative `XDG_STATE_HOME`, the two files a reader might expect
to travel together **silently split**: `recent.toml` goes to a
cwd-relative, likely-nonsensical location (and would need `mkpath` to
succeed there, which it did in the test since the cwd was writable), while
`portals.toml` goes to the reader's *real* state directory as if the
variable had never been set. A reader (or a launcher script, container
entrypoint, or systemd unit) that sets `XDG_STATE_HOME` to a relative path
by mistake gets silently inconsistent, surprising behavior rather than a
clear failure -- worse, if they intended to *sandbox* MERGEN (e.g. testing,
or deliberately isolating state), `recent.toml` would honor the sandbox
while `portals.toml` would quietly escape it into the real home directory.

**Severity: low.** A relative `XDG_STATE_HOME` is a degenerate,
spec-violating configuration a reader is unlikely to set up by accident in
normal desktop use -- this is not a routine failure mode. But it is a real,
confirmed inconsistency between two functions that exist to do the same
job for sibling files, and the failure direction (silently escaping a
sandbox into the real home directory) is the less safe one to default to
silently. **Fix direction:** make `recentFilePath()` use
`QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)`
the same way `portalFilePath()` already does, instead of re-implementing
XDG resolution by hand -- removes the divergence entirely and deletes code
rather than adding it.

---
## Contrast check: a VALID (non-dangling) symlinked state directory works completely transparently

Status: REPRODUCED. Companion to Case 2 in the failure-injection finding
above (which covered a *dangling* symlink at the `mergen` path). Here,
`/tmp/mergen_audit/scratch_symlink/mergen` was a real symlink pointing to
`/tmp/mergen_audit/real_target_elsewhere` (a real, writable, empty
directory). A full `probe markportal` run (real `openPath()` x2,
real `Ctrl+M` x2 completing a portal) through `XDG_STATE_HOME` pointed at
the symlinked path: `exit=0`, both `portals.toml` (261 bytes, correct
content) and `recent.toml` (115 bytes) were written **into the symlink's
target directory**, not beside the symlink itself -- confirmed by listing
`real_target_elsewhere/` directly. A second, fresh `probe construct` run
against the same symlinked path read them back with no issue
(`CONSTRUCT_OK`, normal memory/timing).

This is unsurprising (symlink-to-directory resolution is transparent at the
POSIX level, and `QDir::mkpath()`/`QSaveFile`'s `O_TMPFILE`+`linkat` both
follow symlinks normally like any other filesystem call) but worth
confirming rather than assuming, since the brief asked specifically about
this case. No fix needed -- this is exactly the behavior a reader would
want if, say, `~/.local/state` itself were a symlink into a different
partition or a dotfiles-synced location.

---
