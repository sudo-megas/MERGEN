# RESUME — where MERGEN v2.0 stands

Written as insurance against a session or quota limit. Everything here is
tracked in the repository under `build/docs/`, alongside the constitution.

Paths in these notes appear as `<repo>`, `<home>` and `<scratch>` — they were
absolute when written and are rewritten here rather than published.

Last updated: after Z11j. The audit is over and its findings are fixed,
ruled or rejected — see `build/docs/audit/DISPOSITION.md`, which is the one file to read.

---

## 1. The short version

v2.0 is **built, tested and committed locally**. Nothing is pushed.
The hardening audit ran, found 51 real findings, and they are now dealt with:
every CRITICAL and HIGH is fixed or ruled. Ten hardening commits, Z11a–Z11j, plus one that records them in MZ.md.

- 21 commits ahead of `origin/ata` — Z1–Z10, then Z11a–Z11j and the record commit
- Tags `v1.1.0` … `v2.0.0` exist locally; only `v1.0.0` exists on the remote
- The working tree is clean apart from untracked test PDFs and `docs/`

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

New source files since v1.0: `overlay.{h,cpp}`, `iconset.{h,cpp}`,
`control.{h,cpp}`, `redact.{h,cpp}`. New non-source: `scripts/audit-*.sh`,
`.github/workflows/audit.yml`, `packaging/PKGBUILD` (moved from repo root).

MZ.md carries dated `[!NOTE] Amended at Zn` blocks recording four places where
reality differed from the plan — annotations already rendered, zoom cannot be
animated, there is no reduced-motion hint to honour, and redaction's first
implementation silently did nothing. Those amendments are the honest record and
should not be tidied away.

## 3. How to verify it still works

Acceptance suites for every milestone are in `build/docs/tests/`. `build/docs/tests/BUILD.md` has the exact build and run
commands, the test-document table, and the sanitizer recipe.

Quick version, from `docs/tests/` with the test PDFs present:

    B=<repo>/build; S=<repo>/src
    OBJS=$(find $B/CMakeFiles/mergen.dir -name '*.o' ! -name 'main.cpp.o')
    g++ -std=c++20 -O2 -DNDEBUG -fPIC -o z1check z1check.cpp $OBJS -I$S -I$B \
      -I$B/mergen_autogen/include -DMERGEN_VERSION='"1.0.0"' \
      -DMERGEN_HELPER_PATH='"/x"' -DMERGEN_RELEASE_DATE='"2026-08-24"' \
      $(pkg-config --cflags --libs Qt6Widgets Qt6PrintSupport Qt6Network poppler-qt6 libqpdf)
    QT_QPA_PLATFORM=offscreen ./z1check

All ten passed at the point this was written, and nine of ten were clean under
ASan+UBSan. Also: `bash scripts/audit-attribution.sh`, `audit-deps.sh`,
`audit-colours.sh` — all three green.

The test PDFs (`test.pdf`, `outline.pdf`, `outline-v2.pdf`, `annot.pdf`,
`colour.pdf`, `evil.pdf`) sit in the repo root, untracked. Careful with
`git add -A`.

## 4. The confirmed defect — FIXED at Z11a

`src/redact.cpp:224` — heap-use-after-free, found by ASan.

    QPDFWriter writer(pdf, destination.toLocal8Bit().constData());
    //                     ^ temporary QByteArray, freed at the ';'
    writer.write();   // :227 — qpdf dereferences the freed pointer here

`QPDFWriter` stores the `char*` and uses it during `write()`. Line 202 uses the
same idiom safely because `processFile()` consumes it synchronously.

Fix: hold the `QByteArray` in a named local that outlives the writer.

Fixed at Z11a with named `sourcePath`/`destinationPath` locals. Left in this
document because the reasoning — why it was deliberately *not* fixed while H1
and X3 were still auditing that file — is worth keeping.

## 5. AUDIT RESULT (as reported) — superseded by §5a

The freeze audit ran and answered the question decisively: **no, this is not
ready to freeze.** Eleven agents; a session limit killed seven mid-work, but all
eleven journaled continuously so ~6,600 lines of findings survived.

Confirmed or credibly reported, in `docs/audit/VERIFIED.md`:

- **Redaction fails open by four independent routes** (H1, reproduced). It will
  report success with the selected text fully extractable. Short secrets are
  never verified at all, because the check skips tokens under 3 characters.
- **Redaction can destroy the source.** A symlink defeats the guard; the
  original is overwritten. Reproduced.
- **SIGSEGV** from a socket command during the password dialog. Reproduced
  independently by three parties.
- **Privileged plaintext reaches disk** in the coredump that crash produces.
- **readElevated accepts a truncated privileged read as success.**
- **The v2.0.0 package would ship a binary reporting version 1.0.0.**
- **The PKGBUILD claims integrity from a signed tag that is not signed.**
- **The three CI audit scripts are largely ineffective** and pass vacuously
  outside a git repo.

Nothing had been fixed at the time that was written. That was deliberate —
verify first, then fix once.

## 5a. WHERE IT STANDS NOW — after Z11a–Z11i

Every headline above is dealt with:

- **Redaction** — **withdrawn**, not shipped broken. Unreachable from the
  command overlay; code retained; MZ.md §13 records what returning it needs.
  The source-destruction route was fixed anyway.
- **The SIGSEGV** — fixed, three ways (async socket, re-entrancy guard,
  retired-widget holding instead of `deleteLater` through a nested loop).
- **Privileged plaintext** — wiped, undumpable, and no longer printable to a
  file. The clipboard is deliberately still allowed; §13 says why.
- **The truncated privileged read** — the helper now checks what it sent
  against what it promised, and the reader checks the magic.
- **The version** — the binary carries `2.0.0`, verified with `strings -e l`.
- **The PKGBUILD signature claim** — removed rather than faked.
- **The audit scripts** — 17/17 planted colour violations (was 5/17), anchored
  dependency matching, and they no longer pass vacuously outside a git repo.

**The freeze question:** yes, on the code. The one step not done is a
clean-chroot package build (`extra-x86_64-build`), which needs root with a TTY.

Read `build/docs/audit/DISPOSITION.md` for all 51 findings and what happened to each.

## 6. The audit in flight (historical)

Eleven agents, each journaling continuously to its own file in `docs/audit/`
so nothing is lost to a session limit:

| file | scope | model |
|---|---|---|
| `00-baseline.md` | my own ASan/UBSan + cppcheck + clazy pass | — |
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
| `X3-threat.md` | end-to-end threat model + independent redact review | opus |

Already banked in those files: S4 proved `rotateRect`/`unrotateRect` are exact
algebraic inverses (a negative result that matters, since redaction depends on
it targeting the right region).

**The plan after they finish:** verify every finding independently before
touching code — agents produce confident false positives, and a fix to correct
code is a fresh bug. Then fix what survives, add a regression check per real
defect, re-run all ten suites plus the three CI audits, commit as `Z11:
hardening`, and re-tag `v2.0.0` onto it.

## 7. What has NOT been done

- **Nothing pushed.** `git push origin ata && git push --tags` is the publish
  step; the tag push triggers `.github/workflows/release.yml` to build the
  package and create the GitHub Release. The user wants to review first.
- **No clean-chroot package test.** `extra-x86_64-build` needs root; `sudo`
  has no TTY in the agent shell. Ask the user to run it. This is the single
  remaining unverified step.
- **No README.** MZ.md §11 defers it deliberately. R5 found the family template
  (banner, shields.io badges, bilingual subtitle, numbered ALL-CAPS sections,
  "Built with Reason and Passion"), and it must carry the line
  `xdg-mime default mergen.desktop application/pdf`.

## 8. Standing instructions from the user

- **No AI attribution anywhere** — repo, commits, code comments, About text.
  This is MZ.md §5/§11 and is enforced by `scripts/audit-attribution.sh`, which
  walks the tree, every commit message, and the author list. The family audits
  for it. Do not add trailers.
- All commits from `sudo-megas` only.
- If an agent dies to an API error, **drop it, do not retry**, until the user
  says otherwise.
- MZ.md is the single source of truth for what this application is. The other
  files under `build/docs/` are the working record of how it got there —
  research, decisions, the freeze audit, the acceptance suites.

## 9. Other durable notes in this directory

- `research1.md` — the five research reports v2.0 was designed from
- `Decisions.md` — the 18 questions that produced the design decisions
- `tests/` — acceptance suites + `BUILD.md`
- `audit/` — the freeze audit, in progress
