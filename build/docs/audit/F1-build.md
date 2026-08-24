# F1 — Build / packaging / CI supply-chain audit

> [!NOTE]
> Tool and vendor names in this file are written as `<assistant>` and
> `<vendor>`. They appear here only as examples of the shape the
> attribution audit looks for, never as credit — and writing them out in
> full would trip the very check being described. The substitution is
> mechanical and changes no finding.

Auditor scope: CMakeLists.txt, packaging/PKGBUILD, .github/workflows/{release,audit}.yml,
scripts/audit-{attribution,deps,colours}.sh. Read-only; adversarial tests run on a scratch
clone in /tmp/mergen-audit. Constitution: build/docs/MZ.md (§3 deps, §5 DO-NOT, §10 milestones,
§11 release). Written incrementally; sections marked VERIFIED were executed, ASSESSED were read.

## Status log

- [x] Read MZ.md, all 5 target files, src/ listing
- [x] Baseline: all three audit scripts PASS on the clean tree (VERIFIED, exit 0 each)
- [x] readelf -d on build/mergen and build/mergen-open; pacman -Qo mapping (VERIFIED)
- [x] shellcheck on all three scripts (VERIFIED)
- [x] Adversarial colour matrix
- [x] Adversarial attribution matrix
- [x] Adversarial deps matrix
- [x] Vacuous-pass demo (non-repo dir)
- [x] Clean out-of-tree build, warning check
- [x] Local makepkg end-to-end (file:// source override), namcap
- [x] Workflow assessment writeup

## Confirmed findings so far (evidence below, severity ordering in final section)

### F-01 (HIGH) Version mismatch: binary says 1.0.0, package says 2.0.0
- CMakeLists.txt:8 `project(mergen VERSION 1.0.0 ...)`; packaging/PKGBUILD:4 `pkgver=2.0.0`.
- MERGEN_VERSION feeds QApplication::setApplicationVersion (src/main.cpp:16) and the About
  dialog (src/mainwindow.cpp:1553).
- VERIFIED: `strings -e l build/mergen` (QStringLiteral = UTF-16) contains `1.0.0`.
- MZ.md §10 bumps a version per milestone Z1..Z10 up to v2.0.0; the CMake project version was
  never bumped once. The v2.0.0 package will ship an About dialog and --version claiming 1.0.0.
- Fix direction: set project VERSION 2.0.0 (or derive from git describe at configure time).

### F-02 (HIGH) PKGBUILD claims integrity "from the signed tag"; the tag is unsigned and nothing verifies it
- packaging/PKGBUILD:18-19 comment: "Integrity comes from the signed tag ... why SKIP is correct".
- VERIFIED: `git cat-file -t v2.0.0` -> `commit` (lightweight tag, cannot be signed);
  `git tag -v v2.0.0` -> "cannot verify a non-tag object of type commit".
- Even if the tag were signed, makepkg only verifies git tag signatures when the source
  fragment carries `?signed` and `validpgpkeys=()` is set; neither is present.
- MZ.md §10 Z10 step 6 uses plain `git tag v2.0.0` — so the procedure itself never creates a
  signed tag. With sha256sums=('SKIP') the source has NO integrity check at all: whoever
  controls the GitHub repo (or a MITM on the transport, mitigated only by TLS) controls the build.
- Fix direction: annotated+signed tag, `#tag=v$pkgver?signed`, validpgpkeys with the maintainer key.

### F-03 (VERIFIED baseline) The three audit scripts pass on the clean tree
- `bash scripts/audit-attribution.sh` exit 0; `bash scripts/audit-deps.sh build/mergen` exit 0;
  `bash scripts/audit-colours.sh` exit 0. No false positives on the real tree.

### F-04 (VERIFIED) Binary link map — depends[] cross-check
readelf -d build/mergen NEEDED -> owner package:
- libQt6{PrintSupport,Network,Widgets,Gui,Core}.so.6 -> qt6-base
- libGLX.so.0, libOpenGL.so.0 -> libglvnd   <-- NOT in depends; see F-05
- libpoppler-qt6.so.3 -> poppler-qt6
- libqpdf.so.30 -> qpdf (confirms qpdf in depends is genuinely needed)
- libstdc++/libgcc_s -> gcc-libs family, libm/libc -> glibc (base, never listed)
build/mergen-open NEEDED: libstdc++, libm, libgcc_s, libc only.
- qt6-base, poppler-qt6, qpdf, polkit (pkexec runtime), ttf-cascadia-code-nerd (glyphs, MZ §3),
  hicolor-icon-theme (icon dirs) — all justified. Nothing unnecessary in depends.

### F-05 (LOW-MEDIUM) libglvnd is a direct DT_NEEDED but absent from depends
- mergen's own NEEDED lists libGLX.so.0 and libOpenGL.so.0 (owned by libglvnd), pulled in by
  Qt6::Gui's public link interface (VERIFIED: /usr/lib/libQt6Gui.so.6 NEEDED carries
  libEGL/libGLX/libOpenGL). Satisfied today only transitively via qt6-base's dep tree.
  Arch policy is to list packages owning direct link deps. namcap output to follow.
- Fix direction: add libglvnd to depends (harmless, accurate).

### F-06 (VERIFIED, design-confirming) A transitive/ldd-based deps check is impossible here
- `ldd build/mergen` transitively pulls libglib-2.0, libQt6DBus, libdbus-1, libgio-2.0 (via
  qt6-base) and libcurl.so.4 (via poppler). The blessed §3 stack itself carries every "banned"
  name transitively, so audit-deps.sh's direct-NEEDED-only scope is the only workable reading
  of "nothing linked beyond §3" — this matches the script's own comment and is NOT a bug.

### F-07 (MEDIUM) Release workflow satisfies the hicolor-icon-theme dependency only by accident
- .github/workflows/release.yml:19-22 installs git cmake ninja gcc github-cli qt6-base
  poppler-qt6 qpdf polkit ttf-cascadia-code-nerd — but PKGBUILD depends includes
  hicolor-icon-theme, and makepkg (run without -s/-d) hard-fails on missing depends.
- VERIFIED via `pactree -s -l` over the sync DB: of the ten packages installed by the workflow,
  exactly one — cmake — happens to drag hicolor-icon-theme into its transitive closure. The
  release currently builds only because of an unrelated package's dep tree; if cmake ever drops
  it the release job dies at "Missing dependencies".
- Fix direction: add hicolor-icon-theme (and libglvnd if F-05 accepted) to the pacman -S list.

### F-08 (LOW) shellcheck findings on all three scripts
- SC2164 (audit-attribution.sh:5, audit-deps.sh:4, audit-colours.sh:5): `cd "$(dirname "$0")/.."`
  unguarded; with `set -uo pipefail` but no `-e`, a failed cd leaves the script scanning the
  WRONG directory. Same failure class as the vacuous-pass problem (F-2x below).
- SC2001 style x2 in audit-deps.sh. VERIFIED by running shellcheck 0.11.0.

## ADVERSARIAL MATRIX (all VERIFIED by execution in /tmp/mergen-audit clone)

### F-09 (HIGH) audit-colours.sh: 5 of 17 planted violations caught, 12 evade
Planted a 17-case block into tracked src/main.cpp (worktree; git grep scans tracked worktree
files) and ran `bash scripts/audit-colours.sh` -> exit 1, but only these were flagged:

| # | Construct | Result |
|---|-----------|--------|
| T01 | `auto x = QColor(255, 0, 0);` (temporary) | CAUGHT |
| T02 | `QColor x(255, 0, 0);` (named-variable declaration) | **MISSED** |
| T03 | `auto x = QColor(0xFF0000);` | CAUGHT |
| T04 | `QColor x(0xFF0000);` | **MISSED** |
| T05 | `"#ff0000"` six-digit hex string | CAUGHT |
| T06 | `"#f00"` three-digit hex string | **MISSED** |
| T07 | bare `Qt::red` | CAUGHT |
| T08 | `Qt::darkRed` | **MISSED** (dark* family absent from list, only darkGray present) |
| T09 | `Qt::black` outside presentation | **MISSED** (see below) |
| T10 | `QColor(QStringLiteral("red"))` colour-by-name | **MISSED** |
| T11 | `QColor::fromRgb(0xFF0000)` | **MISSED** (`::` breaks `QColor\s*\(`) |
| T12 | `c.setRgb(255, 0, 0)` | **MISSED** |
| T13 | `qRgb(255, 0, 0)` | **MISSED** |
| T14 | `QColor{255, 0, 0}` brace init | **MISSED** (paren-only regex) |
| T15 | `int r=255...; QColor x(r, g, b);` ints via variables | **MISSED** |
| T16 | `QColor::fromHsv(0, 255, 255)` | **MISSED** |
| T17 | `QColor(Qt::red)` | CAUGHT (by the named-colour grep) |

- T02/T04 are the sharpest: the ordinary C++ declaration spelling of exactly the violation the
  script exists for evades, because `QColor\s*\(` requires the paren to follow the type name
  directly. scripts/audit-colours.sh:9.
- T09: the header comment (line 16) says "Qt::black is allowed only in the presentation
  surround" but black is simply absent from the grep list, so Qt::black ANYWHERE passes — the
  "only" is not enforced at all. (Legit use exists: src/pageview.cpp:311 presentation surround,
  argued in MZ.md §6 — that use is fine; unrestricted black elsewhere is not.)
- Qt dark* names other than darkGray (darkRed, darkGreen, darkBlue, darkCyan, darkMagenta,
  darkYellow) are all absent from the list. So are `transparent`, `color0`, `color1`.
- Fix direction: match the constructor forms `QColor[[:space:]]*[A-Za-z_0-9]*[({]`, add
  `fromRgb|fromRgba|fromHsv|fromHsl|fromCmyk|setRgb|setNamedColor|fromString`, extend the
  named list to the full Qt::GlobalColor enum minus a single annotated allowance for the
  presentation-surround line, and catch 3/4/8-digit hex. qRgb cannot be banned wholesale —
  src/pageview.cpp:53 uses it legitimately in the night-mode transform math — so allow it by
  annotation or path. T15 (variables) is fundamentally beyond grep; note as known limit.
### F-10 (HIGH) audit-attribution.sh: catches the exact <assistant>-Code shapes, misses everything adjacent
Two-phase test in the clone (author identity set to sudo-megas so tests do not cross-contaminate).

Phase A — six planted evasions, script exits 0, ALL MISSED:

| Planted | Where | Result |
|---|---|---|
| "This patch was written by <an assistant> over a weekend." | commit body | **MISSED** — pattern only matches hyphenated `AI-(generated|assisted|written)` |
| "Reviewed with <vendor-c-model> before merging." | commit body | **MISSED** — only the chat-product name is listed; the bare vendor-c model names absent |
| "Thanks to <vendor-b> for spotting the race." | commit body | **MISSED** — <vendor-b> (and every non-<vendor-c>/<vendor> vendor) absent |
| author `sudo-megas <evil@attacker.example>` | commit author | **MISSED** — check greps `^sudo-megas ` against %an only; email never validated |
| committer `<assistant> <noreply@vendor.example>`, author sudo-megas | commit committer | **MISSED** — format is `%an <%ae>`; committer identity is never examined |
| "Assisted‑by: Cursor and <vendor-c-cli>" | commit body | **MISSED** — trailer name and both tools absent from pattern |

Phase B — catches work, but two tree holes proven (script exit 1, only partial reporting):

| Planted | Result |
|---|---|
| `// Co-Authored‑By: <assistant> <noreply@vendor.example>` in tracked src/overlay.cpp | CAUGHT (tree scan) |
| commit body `Co-Authored‑By: <assistant> <noreply@vendor.example>` | CAUGHT (history scan) |
| commit authored by `<assistant> <noreply@vendor.example>` | CAUGHT (author scan) |
| tracked `build/docs/CREDITS.md` containing "Produced with <assistant-cli>." | **MISSED** — pathspec `:!build/docs/*` (scripts/audit-attribution.sh:13) excludes the whole directory, not just the two constitution files; MZ.md §5 says "AI attribution anywhere in the repo" |
| tracked `attribution.png` with `Co-Authored‑By: <assistant>` appended | **MISSED** — `git grep -I` skips binary files; confirmed the string IS in the tracked blob (`git grep -c` finds it) while the audit reports clean for it |

- Fix direction: broaden pattern (`\bAI\b`, `\b<vendor-c-model>\b`, <vendor-b>, cursor, <vendor-c-cli>, aider,
  devin, <vendor-d> already present; `assisted[- ]by`); check `%an <%ae>%n%cn <%ce>` and pin the
  email; exclude only `build/docs/MX.md` and `build/docs/MZ.md` by name; drop -I or scan binary
  files' strings separately.
- Note: `set -uo pipefail` without `-e` plus `if git ...; then FAIL; else clean` means a git
  FAILURE (exit 128) also lands in the "clean" branch — see F-12 vacuous-pass demo.
### F-11 (HIGH) audit-deps.sh: unanchored `libm` admits any library containing that substring; dlopen invisible; DBus ban entry has a case typo
Built six one-line C binaries in /tmp/adv and ran `bash scripts/audit-deps.sh <bin>` on each:

| Binary links | NEEDED shows | Verdict | Meaning |
|---|---|---|---|
| libmagic (`-lmagic`) | libmagic.so.1 | **exit 0 PASS** | **HOLE**: allowlist token `libm` (scripts/audit-deps.sh:11) is an unanchored substring — "lib**m**agic" matches |
| libmount (`-lmount`) | libmount.so.1 | **exit 0 PASS** | **HOLE**: same — also libmd, libmvec, libmpfr, libmenuw... |
| `dlopen("libcurl.so.4")` | libc.so.6 only | **exit 0 PASS** | **HOLE by design**: runtime loading invisible to `readelf -d`; the test binary demonstrably loads libcurl when run. Mitigating: `grep -rn 'dlopen\|QLibrary\|QPluginLoader' src/` finds nothing — unexploited today, unenforced tomorrow |
| Qt6DBus (`-lQt6DBus`) | libQt6DBus.so.6 | exit 1 FAIL | Caught — but by the ALLOWLIST only. The banned regex says `libQt6Dbus` (line 12, lowercase b) and never matches the real soname `libQt6DBus.so.6`; the "banned" section printed `none` |
| sqlite3 | libsqlite3.so.0 | exit 1 FAIL | Correctly caught by both banned and unexpected |
| zstd | libzstd.so.1 | exit 1 FAIL | Correctly caught as unexpected — honest new direct deps DO fail |

- Fix direction: compare full sonames anchored (`grep -x` against a list like `libm\.so\.6`),
  fix `libQt6Dbus`->`libQt6DBus`, and add a source-level `dlopen|QLibrary|QPluginLoader` grep.

### F-12 (HIGH) Vacuous pass: in a broken environment every audit reports "clean" and exits 0
- Copied audit-attribution.sh and audit-colours.sh into a NON-git directory (/tmp/novcs)
  containing a planted `// Co-Authored‑By: <assistant>` file. Both scripts printed
  `fatal: not a git repository` for every check, then printed `clean`, then **exited 0**.
- Cause: `set -uo pipefail` without `-e`, and `if git grep ...; then FAIL; else clean` — git's
  exit 128 (error) is indistinguishable from exit 1 (no matches). shellcheck's SC2164 on the
  unguarded `cd` is the same class.
- Why it matters in CI: git inside the archlinux container refuses to read a workspace owned by
  another user unless `safe.directory` is set — audit.yml sets it in the *Build* step
  (.github/workflows/audit.yml:30). If that step is ever reordered, removed, or the build is
  skipped, all three audit steps go green while checking nothing.
- Fix direction: distinguish exit codes (`git grep ...; case $? in 0) fail;; 1) clean;; *) exit 2;;`)
  or at minimum a preflight `git rev-parse --git-dir` + a self-test that plants a violation and
  asserts the script fails.

### F-13 (MEDIUM) audit-deps.sh never examines mergen-open
- audit.yml:38 runs `bash scripts/audit-deps.sh build/mergen` only. The privileged helper
  build/mergen-open is a second installed ELF; a banned library linked into it would pass CI.
- VERIFIED current state is clean (readelf: libstdc++/libm/libgcc_s/libc only) — the gap is
  enforcement, not present-day linkage. Fix: run the script on both binaries.
## PKGBUILD verification (packaging/PKGBUILD)

### F-14 (VERIFIED) End-to-end makepkg works with the PKGBUILD standing alone in a directory
- Copied packaging/PKGBUILD alone into /tmp/pkgtest (mimicking release.yml's
  `cp checkout/packaging/PKGBUILD .`), changed ONLY the source URL to
  `git+file://<repo>#tag=v$pkgver` (the v2.0.0 tag is not on GitHub yet, so the
  real URL cannot be cloned — that one substitution is the only deviation), ran plain `makepkg
  --noconfirm`: exit 0, produced mergen-2.0.0-1-x86_64.pkg.tar.zst.
- `-S "$pkgname"` resolves because makepkg checks the git source out at `$srcdir/mergen` and
  runs build() with cwd=$srcdir; the PKGBUILD's own location is irrelevant. The packaging/ move
  is sound. Zero compiler warnings in the makepkg build log.
- Package contents verified with bsdtar -tvf: usr/bin/mergen; usr/lib/mergen/mergen-open at
  mode -rwxr-xr-x root:root — NOT setuid, matching CMakeLists.txt:36-37's stated design
  ("polkit decides, not the filesystem"); desktop entry (MimeType=application/pdf present);
  polkit policy at usr/share/polkit-1/actions; 9 hicolor PNG sizes + SVG; LICENSE at
  usr/share/licenses/mergen. .PKGINFO depends/license match the PKGBUILD.
- license=('GPL-3.0-only') matches: VERIFIED all 17 files in src/ carry
  `SPDX-License-Identifier: GPL-3.0-only` (grep), as does CMakeLists.txt.
- qpdf in depends is genuinely needed: libqpdf.so.30 is in mergen's DT_NEEDED (F-04) and
  src/redact.cpp uses it (MZ §3's argued single purpose).

### F-15 (VERIFIED) namcap: PKGBUILD clean; package warnings all explainable
- `namcap packaging/PKGBUILD`: no output, exit 0.
- `namcap mergen-2.0.0-1-x86_64.pkg.tar.zst`: W-level only —
  libgcc/glibc/libstdc++ "implicitly satisfied" (toolchain, never listed on Arch — fine);
  "polkit may not be needed" and "ttf-cascadia-code-nerd may not be needed" are FALSE
  positives: namcap sees only ELF links, these are runtime exec (pkexec) and font deps argued
  in MZ §3. No error-level findings. namcap raises nothing about libGLX/libOpenGL because
  qt6-base's `libgl` virtual dependency (provided by libglvnd — verified via pactree/pacman -Si)
  guarantees them; F-05 downgraded to LOW accordingly.

### F-16 (VERIFIED) The version mismatch ships: the 2.0.0 package's binary says 1.0.0
- Extracted usr/bin/mergen from the built mergen-2.0.0-1 package: `strings -e l` contains
  `1.0.0` (the QStringLiteral MERGEN_VERSION) and `/usr/lib/mergen/mergen-open` (helper path
  correct for the packaged layout). F-01 confirmed end-to-end in the release artefact.

## Release workflow (.github/workflows/release.yml) — ASSESSED by reading, plus local facts

### F-17 (MEDIUM) No integrity binding: release clones "whatever the tag points to right now", twice
- release.yml:27 clones by tag NAME (`--branch "$GITHUB_REF_NAME"`), not by `$GITHUB_SHA`;
  then makepkg clones the repo AGAIN (PKGBUILD source array) — the artefact content is whatever
  the tag points to at each clone moment. A tag force-re-pointed between trigger and clone (or
  between the two clones) silently changes the shipped artefact. Combined with F-02 (SKIP, no
  signed tag) there is no integrity chain at all beyond GitHub TLS.
- Fix direction: after clone, assert `git rev-parse HEAD` == `$GITHUB_SHA`; verify the same in
  makepkg's checkout or pass the commit through.
- Related (LOW): both clones are unauthenticated — the workflow only works while the repo is
  public; a later switch to private breaks the release with no obvious cause.

### F-18 (assessment) git-clone-instead-of-checkout is defensible, and good for token hygiene
- release.yml uses NO third-party or first-party actions at all, so there is nothing to pin —
  and, notably, actions/checkout persists the GITHUB_TOKEN into .git/config by default, which
  plain anonymous clone avoids: the token exists only in the final publish step's env
  (release.yml:41), so the compiler/makepkg stage cannot read it. `permissions: contents: write`
  (release.yml:8-9) is the documented minimum for `gh release create/upload`; no other scopes
  granted. This is the tight part of the workflow. Trade-off is F-17's lost SHA binding.
- ASSESSED (not executable here): the `useradd builder` + `sudo -u builder makepkg` dance works in
  the archlinux:base-devel image because the base-devel meta-package includes sudo and fakeroot;
  this is the one release-build piece the local makepkg test did not exercise by proxy.

### F-19 (MEDIUM) Release build is unreproducible and the artefact carries no verification data
- Container `archlinux:base-devel` is a floating tag and `pacman -Syu --noconfirm`
  (release.yml:15-20) installs whatever is current — two runs of the same tag can produce
  different binaries (different Qt/poppler/qpdf sonames), and a rebuild months later may not
  even link. .BUILDINFO does record the installed versions (verified in the local package), so
  post-hoc reconstruction is possible, but nothing pins them up front.
- The uploaded .pkg.tar.zst has no signature, no checksum listing, no provenance attestation.
  MZ §12 says "Package signing is the user's own step, outside this run" — so absence of a
  SIGNATURE is a documented choice, not a bug; but a checksum in the release notes or a GitHub
  build-provenance attestation is not signing and would cost nothing. As shipped, a user has no
  way to verify the asset beyond trusting the GitHub release page.
- Also (LOW): the container set installs hicolor-icon-theme only via cmake's transitive closure
  — see F-07.

### F-20 (MEDIUM) The audits do not gate the release
- MZ §10 Z10.2 says the audit scripts are "Gated on every push". audit.yml `on: push` does fire
  for the tag push too, but release.yml runs INDEPENDENTLY — a failing audit turns the commit
  red while the release job still builds and publishes the package. Nothing sequences release
  after audit (no `needs:`, separate workflows, no required-check binding on tags).
- Fix direction: run the three scripts inside release.yml before makepkg, or make release a job
  that `needs:` the audit job in one workflow.

## Audit workflow (.github/workflows/audit.yml)

### F-21 (VERIFIED mechanics / ASSESSED context) Exit-code propagation is sound; the pieces around it are weaker
- Each audit step is `run: bash scripts/<script>` (audit.yml:35, 38, 41); a step fails the job on
  non-zero, and the adversarial runs above prove the scripts do exit 1 on a catch. Propagation
  is correct. `fetch-depth: 0` (audit.yml:24) is present and is exactly what the history-walking
  attribution audit needs (a shallow clone would scan one commit and vacuously pass the rest).
- `git config --global --add safe.directory "$GITHUB_WORKSPACE"` (audit.yml:30) works in the
  container (runs as root, writes root's ~/.gitconfig, $GITHUB_WORKSPACE is the mounted path) —
  and recent actions/checkout also registers safe.directory itself, so it is belt-and-braces.
  BUT it lives inside the *Build* step: remove/reorder that step and every audit goes green
  vacuously per F-12.
- actions/checkout@v4 (audit.yml:22) is pinned to a floating MAJOR tag, not a commit SHA. A
  compromised or re-pointed v4 tag executes arbitrary code in a job that (see next line) has
  the default token. audit.yml has NO `permissions:` block at all, so the job gets the
  repository's default token permissions (potentially read-write on older default settings).
  Severity MEDIUM. Fix: pin to the full commit SHA and add `permissions: contents: read`.
- The Build step compiles the tree (needed by audit-deps.sh, which gets build/mergen —
  audit.yml:38) but never scans build/mergen-open (F-13), and builds WITHOUT -Werror, so the
  zero-warning state (F-23) is unenforced in CI.
## CMakeLists.txt

### F-22 (VERIFIED) Everything checked works; zero warnings on a clean build
- Fresh out-of-tree configure+build in /tmp/mergen-clean (Ninja, Release): configure exit 0,
  build exit 0, full 15-step log contains ZERO occurrences of "warning" or "error"
  (`grep -icE 'warning|error'` = 0). The makepkg build log is also warning-free. The
  `-Wall -Wextra -Wpedantic` flags (CMakeLists.txt:65,77) are real and the tree is clean under
  them — though CI never enforces this (no -Werror, no log grep), so it can regress silently.
- Source list complete: all 16 tracked src/ files except mergen-open.cpp (own target, line 76)
  and license.h.in (configured, line 32) are in add_executable(mergen) — verified against ls.
- MERGEN_HELPER_PATH: `${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open` (line 38) matches
  install(TARGETS mergen-open ... DESTINATION ${CMAKE_INSTALL_LIBDIR}/mergen) (line 83);
  verified end-to-end: the packaged binary embeds /usr/lib/mergen/mergen-open (UTF-16 string)
  and the package places the helper exactly there. Used at src/mainwindow.cpp:679 via pkexec.
- qpdf: `pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)` (line 27) +
  `PkgConfig::QPDF` (line 74) — resolves and links (libqpdf.so.30 in NEEDED).
- Helper is NOT setuid anywhere: plain install(TARGETS), packaged mode -rwxr-xr-x (F-14).
- (NIT) MERGEN_HELPER_PATH is a CACHE STRING whose default derives from the install prefix at
  FIRST configure; re-configuring an existing build dir with a different prefix keeps the stale
  cached path. Fresh-configure flows (makepkg, CI) are unaffected.
- (NIT) CMakeLists.txt:8 `VERSION 1.0.0` is the root cause of F-01/F-16.

## Reproducibility and hygiene

### F-23 (VERIFIED) No timestamp/hostname/path embedding from the project itself
- MERGEN_RELEASE_DATE is a fixed configure-time string ("2026-08-24"), not __DATE__ — good.
- `strings` (ASCII and UTF-16) over the packaged binary: no /home paths, no hostname; only
  /usr/lib/mergen/mergen-open and state-file fragments. license.h is generated from the tracked
  LICENSE deterministically (raw-string, @ONLY).
- Non-reproducibility that remains is environmental: makepkg's .BUILDINFO embeds builddate and
  the exact installed package set (verified in the local package), and the CI container floats
  (F-19). That is standard Arch practice, not a MERGEN defect.

### F-24 (LOW) .gitignore is correct; the scratch PDFs plus the §10 `git add -A` recipe are one command from shipping
- VERIFIED: build/CMakeCache.txt ignored by `/build/*`; build/docs/{MX.md,MZ.md} tracked (the
  `!/build/docs/` re-include works — parent dir itself is not excluded, only its contents, so
  the negation is effective); docs/ ignored; compile_commands.json ignored. Nothing sensitive
  or large is trackable from the build tree.
- BUT six scratch PDFs sit untracked at the repo root (annot/colour/evil/outline/outline-v2/
  test.pdf), `git check-ignore test.pdf` exits 1 (NOT ignored), and every MZ §10 milestone
  recipe is `git add -A`. They escaped the v2.0.0 tag (verified: no .pdf in `git ls-tree -r
  v2.0.0`), but the next `git add -A` sweeps them in. Fix: add `/*.pdf` to .gitignore.

### F-25 (INFO, claims spot-checked) MZ claims that hold
- "No stylesheet is used" (§6): `grep -rn setStyleSheet src/` — nothing. Holds (unenforced).
- Qt::black only in the presentation surround: src/pageview.cpp:311 is the only occurrence.
  Holds today; unenforced (F-09 T09).
- Depends exactly cover §3's list plus hicolor-icon-theme (icon dirs; standard) — nothing
  banned, nothing missing except the F-05 libglvnd nicety.

## SEVERITY-ORDERED SUMMARY

| Sev | Finding | Where |
|---|---|---|
| HIGH | F-09 colours audit: 12 of 17 planted violations evade, incl. plain `QColor x(255,0,0)` declarations, fromRgb/setRgb/qRgb, colour-name strings, Qt dark* names, unrestricted Qt::black | scripts/audit-colours.sh:9,17 |
| HIGH | F-10 attribution audit: misses "written by <an assistant>", <vendor-c-model>/<vendor-b>/Cursor/<vendor-c-cli>, spoofed author email, <assistant>-as-committer, anything in build/docs/, anything in binary files | scripts/audit-attribution.sh:8,13,29 |
| HIGH | F-11 deps audit: unanchored `libm` allowlist token admits libmagic/libmount/etc (proven exit 0); dlopen invisible; `libQt6Dbus` ban entry can never match real soname | scripts/audit-deps.sh:11-12 |
| HIGH | F-12 all audits report "clean"/exit 0 in a broken or non-repo environment (proven); safe.directory dependency sits in a different CI step | scripts/*:4-5, .github/workflows/audit.yml:30 |
| HIGH | F-01/F-16 version skew: project VERSION 1.0.0 vs pkgver 2.0.0; the built 2.0.0 package provably ships an About/--version of 1.0.0 | CMakeLists.txt:8, packaging/PKGBUILD:4 |
| HIGH | F-02 "Integrity comes from the signed tag" is false: v2.0.0 is a lightweight unsigned tag, no ?signed/validpgpkeys, sha256sums SKIP = no integrity at all | packaging/PKGBUILD:18-21 |
| MED | F-20 audits do not gate the release: separate workflows, no needs:, tag builds+publishes even if audits fail — weaker than MZ §10 "Gated on every push" | .github/workflows/{audit,release}.yml |
| MED | F-17 release clones by tag name (twice), never pins $GITHUB_SHA; re-pointed tag silently changes the artefact | .github/workflows/release.yml:27, PKGBUILD:20 |
| MED | F-19 floating container + unpinned pacman -Syu → unreproducible release; uploaded .pkg.tar.zst carries no checksum/attestation (signature itself is documented as the user's own step) | .github/workflows/release.yml:15-20,43-49 |
| MED | F-21 actions/checkout@v4 floating tag, and audit.yml has no permissions: block (default token perms) | .github/workflows/audit.yml:22 |
| MED | F-07 release container lacks hicolor-icon-theme; makepkg dep check passes only because cmake happens to drag it in | .github/workflows/release.yml:19-22 |
| MED | F-13 deps audit never scans the second installed ELF, mergen-open | .github/workflows/audit.yml:38 |
| LOW | F-05 libglvnd is a direct DT_NEEDED (libGLX/libOpenGL) but not in depends; guaranteed today via qt6-base's libgl virtual dep | packaging/PKGBUILD:10 |
| LOW | F-08 shellcheck SC2164 x3 (unguarded cd, same class as F-12) | scripts/*.sh |
| LOW | F-24 unignored scratch PDFs + `git add -A` milestone recipe | .gitignore, MZ.md §10 |
| NIT | F-22 MERGEN_HELPER_PATH cache staleness on re-configure with changed prefix | CMakeLists.txt:38 |
| NIT | CI never enforces the zero-warning state (no -Werror / log check) | .github/workflows/audit.yml:31-32 |

Verified-positive: baseline audits pass on the clean tree (F-03); depends[] exactly matches the
real link map (F-04); direct-NEEDED-only scope is the only workable deps-check design — the
blessed stack itself transitively carries libcurl/glib/dbus (F-06); PKGBUILD works from
packaging/ standalone, package contents complete and correct, helper not setuid (F-14); namcap
clean (F-15); zero compiler warnings (F-22); no embedded paths/timestamps from the project
(F-23); .gitignore's build/docs carve-out works exactly as intended (F-24); release.yml uses no
unpinned third-party actions and keeps the token out of the build stage (F-18).
