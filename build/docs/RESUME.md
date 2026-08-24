# RESUME — where MERGEN stands

Written as insurance against a session or quota limit. Everything here is
tracked in the repository under `build/docs/`, alongside the constitution.

Paths in these notes appear as `<repo>`, `<home>` and `<scratch>` — they were
absolute when written and are rewritten here rather than published.

Last updated: after **v2.0.6**, released 2026-08-25. Sections 1–7 are the
current state; section 8 is the historical record of the v2.0 freeze audit and
is kept because the reasoning in it is still the reasoning.

---

## 1. The short version

**v2.0.6 is released.** `origin/ata` and the tag `v2.0.6` both point at the same
commit, the package is published, and all three CI jobs are green.

- Everything is pushed. This document previously said "nothing is pushed"; that
  has not been true since v2.0.0.
- Published releases: `v1.0.0`, `v2.0.0`, `v2.0.1`, `v2.0.2`, `v2.0.3`,
  `v2.0.4`, `v2.0.6`, each with a built `.pkg.tar.zst` attached.
- **`v2.0.5` was never tagged.** It exists as a commit; the version was bumped
  again before anything was released, so 2.0.5's changes ship inside 2.0.6.
- **The milestone tags `v1.1.0` … `v1.9.0` are local-only.** They exist in the
  main checkout and were never pushed. The remote carries `v0.1.x`, `v1.0.0`
  and the `v2.0.x` line. Nothing depends on this; it is recorded so their
  absence on the remote is not mistaken for loss.

## 2. What v2.0 is

Ten milestones, Z1–Z10, each its own commit and tag. The constitution is
`build/docs/MZ.md` (tracked), which supersedes `MX.md` (tracked, sealed as the
v1.0 record).

| | milestone | tag |
|---|---|---|
| Z1 | Icon set, toolbar authorship, accessibility | `v1.1.0` |
| Z2 | Transient overlay surface + document properties | `v1.2.0` |
| Z3 | Outline overlay + hold-to-peek | `v1.3.0` |
| Z4 | Command overlay (Ctrl+K) | `v1.4.0` |
| Z5 | Render transform stage + night mode | `v1.5.0` |
| Z6 | Annotation rendering stated rather than inherited | `v1.6.0` |
| Z7 | Motion, presentation mode, empty state | `v1.7.0` |
| Z8 | Control socket | `v1.8.0` |
| Z9 | Compare mode + portals | `v1.9.0` |
| Z10 | Redaction, CI audits, packaging | `v2.0.0` |

Then the point releases, all on 2026-08-24/25 and all but one about the
privileged path — see §4:

| | what it was | 
|---|---|
| 2.0.1 | `qpdf` moved to `makedepends`; the built package never loads it |
| 2.0.2 | A file you may not read is not a file that is missing |
| 2.0.3 | The status bar answers rather than going blank |
| 2.0.4 | The same question, asked properly everywhere it is asked |
| 2.0.5 | One name for the helper, and a check that it stays one (never tagged) |
| 2.0.6 | A comparison may authenticate; two documents may be privileged |

New source files since v1.0: `overlay.{h,cpp}`, `iconset.{h,cpp}`,
`control.{h,cpp}`, `redact.{h,cpp}`. New non-source: `scripts/audit-*.sh`,
`.github/workflows/audit.yml`, `packaging/PKGBUILD` (moved from repo root),
`build/docs/tests/run-all.sh`, `build/docs/tests/make-fixtures.py`.

MZ.md carries dated `[!NOTE] Amended at …` blocks recording every place where
reality differed from the plan. Those amendments are the honest record and
should not be tidied away.

## 3. How to verify it still works

One command, from the repository root, after `cmake --build build`:

    bash build/docs/tests/run-all.sh

It writes the fixtures, compiles all eleven suites against the objects the real
build produced, runs them plus `z8check.sh`, and exits with the number of suites
that failed. About a minute and a quarter. **249 checks.**

> [!IMPORTANT]
> **Not as root**, and the script refuses. Several checks turn on a directory
> the account cannot traverse — that is how an unreadable document is simulated
> without a root-owned file — and root ignores permission bits. As root,
> `stat()` on a mode-000 directory succeeds, `Presence::Unreadable` never
> occurs, and every check guarding the elevation path passes while testing
> nothing. A suite that cannot fail is worse than no suite.

The four audits, all of which CI runs:

    bash scripts/audit-attribution.sh
    bash scripts/audit-deps.sh build/mergen
    bash scripts/audit-colours.sh
    bash scripts/audit-policy.sh build

`audit-policy.sh` is the newest: it requires the helper path compiled into the
binary, the path the polkit action authorises, and the directory `install()`
puts the helper in to be one directory. They drifted once — see §4.

**CI runs the suites now**, in the `suites` job of `.github/workflows/audit.yml`,
as an ordinary user it creates for the purpose. Before v2.0.6 the suites ran
only by hand, and turning them on immediately found three tests that were
measuring the environment rather than the program:

- z2 asserted the page counter was visible while testing something about Esc.
  Whether it is visible depends on whether the toolbar fits, which depends on
  the machine's font metrics. It compares the counter against itself now.
- z8 slept two seconds and asserted the control socket existed. A fixed sleep is
  a test that fails for being early. It waits.
- z8 looked for the socket in the wrong directory. `Control::socketPath` asks Qt
  for the runtime location, and with `XDG_RUNTIME_DIR` unset Qt does **not** fall
  back to `/tmp` — it uses `/tmp/runtime-$USER`. The script guessed `/tmp`, so
  every check failed on any machine without that variable, which is every
  container and no desktop. It sets the variable now rather than guessing.

The fixtures are written by `python3 build/docs/tests/make-fixtures.py` into the
working directory. They are gitignored, along with the compiled suites and their
`.log` files — but still, careful with `git add -A`.

## 4. The privileged-document arc — 2.0.2 to 2.0.6

The most recent work, and the part most likely to be picked up next. All of it
concerns opening a PDF this account may not read.

**How it started.** Opening a root-owned PDF with the installed package reported
"does not exist", and no `pkexec` process was ever spawned. `Document::openPath`
tested `QFileInfo::exists()`, which answers `stat()` and returns false for every
failure alike — so a PDF inside a directory this user cannot traverse (`/root`
at mode 750, the ordinary case) came back `NotFound` and stopped before the
`NoPermission` branch that offers to authenticate. The feature had shipped since
v1.0 and worked only for an unreadable file inside a *traversable* directory.

Eleven audit agents, one dedicated entirely to the privileged path, and fourteen
acceptance suites had all passed over it. None of them ever ran the real binary
against a file it genuinely could not stat. **That is the lesson of this whole
arc**: run the real thing against the real case.

**What each release did.**

- **2.0.2** — `errno` separates the two: `ENOENT` is not there, `EACCES` is not
  permitted to look, and only the second is a question `pkexec` can answer.
- **2.0.3** — the status bar's permissions and dates both `stat()` as this
  account, which fails for exactly these documents. Two empty labels read as a
  broken status bar; it says "needs privilege" now.
- **2.0.4** — 2.0.2 fixed the question inside `openPath` rather than somewhere
  reusable, so four other callers went on asking `QFileInfo::exists()`. The worst
  was `pushRecent`: a root-owned document opened, entered the recent list, and
  was deleted from it by the *next* open. Now a `Presence` type
  (`Present`/`Absent`/`Unreadable`) with `presenceOf()` the single place `errno`
  is read.
- **2.0.5** — `MERGEN_HELPER_PATH` was `set(… CACHE …)` with a computed default,
  which assigns only when the cache lacks the variable. Reconfiguring an existing
  build tree with a different prefix kept the old path while `install()` moved
  the file, so the binary and the policy named a helper that was not there.
  Releases were never affected (`makepkg` uses a fresh directory); developers
  were. Recomputed every configure now, with a configure-time warning and
  `audit-policy.sh` standing over it.
- **2.0.6** — a comparison may authenticate, ruled in MZ.md §13 on the same
  reasoning as the clipboard. Needed three things, two of which are why it was a
  ruling and not a patch: the `m_opening` guard (`readElevated` runs a nested
  event loop and a socket `open` arriving through it would `leaveCompare` the
  document out from under the call), `PR_SET_DUMPABLE` counted rather than
  set/cleared (two elevated documents are possible now, and clearing it on the
  first close would re-enable core dumps while the second held plaintext), and
  `~Document` wiping (a comparison document is released by resetting its owner
  and never passes through `close()`).

**Two older defects found while doing 2.0.6**, both in the privileged path:

- `openData` never cleared `m_hash` or `m_properties`, which `openPath` always
  has — and `openPath` returns `NoPermission` before touching any state. So a
  privileged document arriving through `openData` inherited the *previous*
  file's cached hash and properties: the properties overlay described the wrong
  document, and a portal made in the privileged one was keyed by another file's
  content. An elevated document is deliberately not hashed at all (§8), so the
  correct answer was no hash and the answer given was someone else's.
- `openPath` cleared `m_data` with `QByteArray::clear()` rather than
  `wipeData()`, handing privileged plaintext back to the allocator unerased.

**Verified live**, not merely by test: with a document in a directory this
account cannot traverse — the same `EACCES` `/root` gives, and root can traverse
either — the real binary spawns `/usr/bin/pkexec /usr/lib/mergen/mergen-open`,
polkit raises its prompt, the document opens, it survives in the recent list
across the next open, and `enterCompare` on such a file ends with
`isComparing()` true.

**Testing this without root.** A `mktemp -d` directory at mode 000 produces the
same `EACCES` as `/root` at 750, and root ignores permission bits either way, so
the helper under `pkexec` traverses it exactly as it would traverse `/root`. No
`sudo` needed to set the case up, which matters because the agent shell has no
TTY for it.

## 5. What has NOT been done

- **No clean-chroot package build.** `extra-x86_64-build` needs root with a TTY.
  The release workflow does build the package with `makepkg` in a fresh
  `archlinux:base-devel` container on every tag, which covers most of what the
  chroot build would catch, but it is not the same thing.
- **Redaction is withdrawn**, not fixed. MZ.md §13 records what returning it
  needs: full text state tracking, per-glyph advance, painted-extent testing and
  decomposing `'`/`"`. That is a milestone, not a patch.
- **The open rulings in MZ.md §13** are still open: annotation creation, an
  opt-out for motion, hiding annotations, night-mode persistence, and
  search-highlight derivation in a perceptually uniform space.

## 6. Standing instructions from the user

- **No AI attribution anywhere** — repo, commits, code comments, About text.
  This is MZ.md §5/§11 and is enforced by `scripts/audit-attribution.sh`, which
  walks the tree, every commit message, and the author list, and fails CI. Do
  not add trailers of any kind — including the co-authorship one tooling offers
  by default, which the audit rejects by name. Writing that name into a tracked
  file trips it too, as this line was originally drafted and duly caught.
- All commits from `sudo-megas` only.
- If an agent dies to an API error, **drop it, do not retry**, until the user
  says otherwise.
- MZ.md is the single source of truth for what this application is. The other
  files under `build/docs/` are the working record of how it got there.
- Releasing is the user's call. The publish step is a fast-forward of `ata`
  followed by a tag push; pushing branch and tag **together**
  (`git push origin ata v2.0.x`) avoids the failure mode where an `&&` chain
  breaks and the tag lands without the branch.

## 7. Other durable notes in this directory

- `MZ.md` — the constitution, and §13 the rulings
- `MX.md` — sealed v1.0 record
- `research1.md` — the five research reports v2.0 was designed from
- `Decisions.md` — the 18 questions that produced the design decisions
- `tests/` — acceptance suites, `BUILD.md`, `run-all.sh`, `make-fixtures.py`
- `audit/` — the freeze audit; `DISPOSITION.md` is the one file to read

---

## 8. Historical — the v2.0 freeze audit

Kept because the reasoning is still the reasoning. All of it is resolved; see
`build/docs/audit/DISPOSITION.md` for all 51 findings and what happened to each.

### 8a. The confirmed defect — fixed at Z11a

`src/redact.cpp` — heap-use-after-free, found by ASan.

    QPDFWriter writer(pdf, destination.toLocal8Bit().constData());
    //                     ^ temporary QByteArray, freed at the ';'
    writer.write();   // qpdf dereferences the freed pointer here

`QPDFWriter` stores the `char*` and uses it during `write()`. The nearby
`processFile()` call uses the same idiom safely because it consumes the pointer
synchronously. Fixed with named locals that outlive the writer. Kept here
because the reasoning — why it was deliberately *not* fixed while H1 and X3 were
still auditing that file — is worth keeping.

### 8b. What the audit reported

Eleven agents; a session limit killed seven mid-work, but all eleven journaled
continuously so ~6,600 lines of findings survived. Confirmed or credibly
reported: redaction failing open by four independent routes and able to destroy
its source; a SIGSEGV from a socket command during the password dialog;
privileged plaintext reaching disk in the resulting coredump; `readElevated`
accepting a truncated privileged read; the v2.0.0 package shipping a binary
reporting 1.0.0; the PKGBUILD claiming integrity from an unsigned tag; and three
CI audit scripts that passed vacuously outside a git repo.

Nothing was fixed at the time that was written. That was deliberate — verify
first, then fix once.

### 8c. How each was dealt with, Z11a–Z11j

- **Redaction** — withdrawn, not shipped broken. Unreachable from the command
  overlay; code retained; MZ.md §13 records what returning it needs. The
  source-destruction route was fixed anyway.
- **The SIGSEGV** — fixed three ways: async socket, re-entrancy guard, and
  holding retired widgets rather than `deleteLater` through a nested loop.
- **Privileged plaintext** — wiped, undumpable, and not printable to a file. The
  clipboard is deliberately still allowed; §13 says why.
- **The truncated privileged read** — the helper checks what it sent against what
  it promised, and the reader checks the magic.
- **The version** — verified in the built binary with `strings -e l`, and checked
  against the published package on every release since.
- **The PKGBUILD signature claim** — removed rather than faked.
- **The audit scripts** — 17/17 planted colour violations (was 5/17), anchored
  dependency matching, no longer passing vacuously outside a git repo.

### 8d. The agents, for the record

| file | scope | model |
|---|---|---|
| `00-baseline.md` | ASan/UBSan + cppcheck + clazy pass | — |
| `H1-redact.md` | redact.cpp, adversarially | opus |
| `H2-privileged.md` | mergen-open, polkit, readElevated | opus |
| `H3-lifetimes.md` | object lifetimes and concurrency | opus |
| `S1-socket.md` | control socket security and DoS | sonnet |
| `S2-state.md` | recent.toml / portals.toml parsers | sonnet |
| `S3-resources.md` | cache growth, memory | sonnet |
| `S4-ui.md` | coordinate transforms, keys, focus | sonnet |
| `F1-build.md` | CMake, PKGBUILD, CI, audit scripts | fable |
| `X1-fuzz.md` | malicious/malformed PDFs (+ `corpus/`) | opus |
| `X2-print.md` | print path, exhaustion, hangs | opus |
| `X3-threat.md` | end-to-end threat model + redact review | opus |

S4 proved `rotateRect`/`unrotateRect` are exact algebraic inverses — a negative
result that mattered, since redaction depended on it targeting the right region.

**The standing lesson from all of it**, reinforced by §4: eleven agents and
fourteen suites passed over a defect that surfaced within seconds of opening a
root-owned PDF with the installed package. Verify findings independently before
touching code — agents produce confident false positives, and a fix to correct
code is a fresh bug — and then run the real binary against the real case.
