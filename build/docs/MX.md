# MERGEN — Specification and Milestones

A minimal PDF viewer for Arch Linux. Qt6 + poppler-qt6, no KDE Frameworks, no
desktop-environment dependencies.

This is the single source of truth for the project. Spec and milestones live in
this one file; changes are made by editing the section they belong to, not by
appending new documents.

- **Repo:** `github.com/sudo-megas/MERGEN`
- **Binary:** `mergen`
- **Licence:** GPL-3.0-only
- **Platform:** Arch Linux only, Wayland only
- **Default branch:** `ata`
- **Docs path:** `build/docs/MX.md`

---

## Table of contents

1. [Why MERGEN exists](#1-why-mergen-exists)
2. [Environment and dependencies](#2-environment-and-dependencies)
3. [Feature set — v1.0](#3-feature-set--v10)
4. [DO-NOT list](#4-do-not-list)
5. [Interface](#5-interface)
6. [Keybindings](#6-keybindings)
7. [State on disk](#7-state-on-disk)
8. [Architecture](#8-architecture)
9. [Milestones](#9-milestones)
10. [Release procedure](#10-release-procedure)
11. [Working agreement — unattended builds](#11-working-agreement--unattended-builds)

---

## 1. Why MERGEN exists

Every GUI PDF viewer available falls into one of two groups. The good one —
Okular — is excellent but is structurally a KDE application: it is a thin shell
hosting a KPart, its menus come from KXmlGui, its config from KConfig, its file
opening from KIO, its render threading from ThreadWeaver. Installing it means
installing roughly twenty KF6 modules plus Phonon, qt6-speech, libkexiv2 and
libspectre, almost none of which do anything for the act of looking at a page.

The others are either vim-keybound (zathura, sioyek), or drag GTK/glib in, or
feel unfinished.

MERGEN takes the one part of Okular that is actually doing the work — poppler —
and puts a plain Qt6 widget shell around it. Nothing else.

The dependency objection here is the usual one for this family: not the count,
but deps that earn nothing. `poppler-qt6` renders and searches. `qt6-base`
paints, scrolls and handles input. Both earn their place.

---

## 2. Environment and dependencies

Written for and tested on: Arch Linux, Wayland, Niri, `prefer-no-csd` enabled.

| Dependency | Why it is here |
|---|---|
| `qt6-base` | Widgets, painting, scroll view, input handling, file dialog, printing |
| `poppler-qt6` | PDF parsing, page rendering to `QImage`, text extraction, search |
| `ttf-cascadia-code-nerd` | Toolbar icon glyphs — see §5 |

Build-time only: `cmake`, `ninja`, `gcc`.

Nothing else. No KF6, no GTK, no glib, no network library, no database.

CUPS arrives indirectly through `qt6-base`'s print support and is accepted
deliberately — printing PDFs is a real and frequent task here, so the dependency
earns its place.

> [!NOTE]
> Qt has no desktop-portal theme hint on a bare Wayland session, so MERGEN
> renders in Qt's light palette unless `QT_QPA_PLATFORMTHEME` is set. Likewise
> `QFileDialog` only routes through xdg-desktop-portal when that variable is set
> to `xdgdesktopportal`; otherwise Qt draws its own dialog. Both are Qt
> behaviour outside a full DE, not MERGEN bugs, and MERGEN does not set the
> variable itself.

---

## 3. Feature set — v1.0

Seven features plus printing. This list is closed for v1.0.

| Feature | Notes |
|---|---|
| Open a PDF | `argv[1]`, Ctrl+O, toolbar Open button, recent-files dropdown |
| Continuous vertical scroll | All pages in one scrolling column, viewport-culled, smooth pixel scrolling |
| Zoom | 10% steps, 10%–1000%, plus fit-width and fit-page |
| Page jump | By typing into the toolbar page counter |
| Text search | Ctrl+F, next/previous hit, hits highlighted in the page, progress shown and cancellable |
| Text selection and copy | Mouse drag selects, Ctrl+C copies to clipboard |
| Rotate | Clockwise and counter-clockwise, applies to the whole document |
| Print | `QPrintDialog`, page ranges honoured |

---

## 4. DO-NOT list

Authored by the user. Nothing is added to this list without a ruling.

**Never, at any version:**

- Form filling
- Digital signatures
- Page thumbnails panel
- Tabs or multiple documents in one window
- Any format other than PDF — no DjVu, no PostScript, no EPUB, no comic archives
- Bookmarks
- Outline / table-of-contents sidebar
- Remember last page per file
- Any sidebar or side panel at all
- Network access of any kind — no update check, no telemetry, no analytics, no
  crash reporting
- Opening a URL — About page addresses are selectable text, never clickable
- Tray icon
- File-manager context menu integration
- Multiple themes or a theme switcher
- Hardcoded window geometry — the compositor decides
- Storing passwords
- AI attribution anywhere in the repo, commits, or About
- X11 support
- Debian, Windows or macOS packaging
- First-run prompts, onboarding, or nagging dialogs

**Deliberately left open for a later version:**

- Annotations and highlighting — not banned, but out of scope for v1.0

---

## 5. Interface

One window. A top toolbar, and beneath it the page view. No sidebar, no status
bar, no menu bar.

**Toolbar, left to right:**

`Open` (with a dropdown arrow for the recent-files list) · `Zoom out` ·
`Zoom in` · `Fit width` · `Fit page` · `Rotate` · `Search` · `Print`

**Toolbar, right-aligned:**

Page counter, in the form `12 / 340`. The left number is an editable field —
typing a page number and pressing Enter jumps to that page. There is no separate
go-to-page dialog.

Buttons are **icon glyphs with text labels**. Icons are Font Awesome 5 glyphs
from the CaskaydiaCove Nerd Font patch, set as the action text in a glyph-font
label — no SVG assets, no `.qrc`, and no icon-theme dependency such as
`kiconthemes`.

**Window title,** one string, since compositor-drawn decorations have no zones:

```text
MERGEN ///— document.pdf —\\\ MEGAS
```

With no document open:

```text
MERGEN ///— —\\\ MEGAS
```

**Geometry.** No default size, no minimum size, no saved geometry. Niri places
and sizes the window.

**Colours.** Qt6 default style following the system palette. The area around the
pages uses `QPalette::Base`. Text selection uses `QPalette::Highlight`. Search
hits are derived at runtime from the highlight colour — hue rotated 180°, alpha
~40% — so they always contrast with both the selection and the page without a
hardcoded value. There is no theming code and no palette switcher.

**Decorations** are drawn by the compositor. MERGEN draws none.

**Empty state.** Launched with no argument, MERGEN shows an empty page view with
the toolbar active. Error messages for unreadable or non-PDF files are drawn as
centred text in this same empty view — never as a dialog box.

**File changed on disk.** A `QFileSystemWatcher` on the open document. When it
fires, a small inline notice appears in the page view; clicking it reloads.
MERGEN never reloads on its own.

**Password-protected PDFs.** A `QInputDialog` prompts for the password, with a
retry loop on failure and cancel returning to the empty state. The password
lives in memory for the session only — never written to disk, never placed in
`recent.toml`, wiped on close.

---

## 6. Keybindings

Standard desktop conventions. No vim-style bindings anywhere.

| Key | Action |
|---|---|
| <kbd>↑</kbd> / <kbd>↓</kbd> | Scroll by line |
| <kbd>PgUp</kbd> / <kbd>PgDn</kbd> | Scroll by page |
| <kbd>Home</kbd> / <kbd>End</kbd> | Jump to document start / end |
| <kbd>←</kbd> / <kbd>→</kbd> | Pan horizontally when zoomed past viewport width |
| Mouse wheel | Scroll |
| <kbd>Ctrl</kbd> + wheel | Zoom |
| <kbd>Ctrl</kbd>+<kbd>+</kbd> / <kbd>Ctrl</kbd>+<kbd>-</kbd> | Zoom in / out by 10% |
| <kbd>Ctrl</kbd>+<kbd>0</kbd> | Reset zoom to 100% |
| <kbd>F</kbd> | Fit width |
| <kbd>Shift</kbd>+<kbd>F</kbd> | Fit page |
| <kbd>Ctrl</kbd>+<kbd>F</kbd> | Open search |
| <kbd>Enter</kbd> / <kbd>Shift</kbd>+<kbd>Enter</kbd> | Next / previous search hit |
| <kbd>Esc</kbd> | Close search |
| <kbd>Ctrl</kbd>+<kbd>R</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>R</kbd> | Rotate clockwise / counter-clockwise |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Copy selected text |
| <kbd>Ctrl</kbd>+<kbd>O</kbd> | Open file |
| <kbd>Ctrl</kbd>+<kbd>P</kbd> | Print |
| <kbd>Ctrl</kbd>+<kbd>Q</kbd> | Quit |

---

## 7. State on disk

MERGEN writes exactly one file:

```text
~/.local/state/mergen/recent.toml
```

It contains the last ten opened file paths, most recent first, and nothing else.
No page numbers, no window geometry, no zoom level, no passwords, no settings.

The list is a rolling ten: pushing an eleventh entry drops the oldest. There is
no "clear recent files" command — the list clears itself through use.

Written atomically — temp file, `fsync`, rename — on each successful open.

If the file is missing, unreadable or malformed, MERGEN starts with an empty
recent list and overwrites it on the next open. A missing state file is never an
error the user sees.

Paths that no longer exist are shown greyed in the dropdown and dropped from the
file the next time it is written.

---

## 8. Architecture

Four translation units. No plugin system, no abstraction layer over poppler, no
generator indirection.

| File | Responsibility |
|---|---|
| `main.cpp` | `QApplication`, parse `argv[1]`, construct the window |
| `mainwindow.cpp` | Toolbar, actions, keybindings, file dialog, print, recent list, password prompt, file watcher |
| `pageview.cpp` | The scrolling page column: layout, culling, painting, zoom, selection, search highlighting, empty-state and error text |
| `document.cpp` | Thin wrapper over `Poppler::Document` — page count, page size, render, `search()`, `textList()` |

**Rendering model.** Pages are laid out in a single vertical column with a fixed
gap. Only pages intersecting the viewport (plus one page of margin above and
below) are rendered. Rendered pages are cached as `QImage` keyed by page index
and zoom level; the cache is cleared on zoom or rotation change. Scrolling is
smooth and pixel-based, not stepped.

**Zoom.** Zoom steps snap onto the 10% grid rather than compounding, so
stepping up from a fit factor of 87% reaches 90% and not 97%. Both fit modes
size against the largest page in the document rather than the page currently on
screen, so the factor does not shift as the reader scrolls between pages of
different sizes, and a fit mode is a standing instruction: it is recomputed on
every resize until the reader picks an explicit zoom. Fit-width measures against
the viewport width after the scrollbar has taken its share.

**Selection.** `Poppler::Page::textList()` gives per-word bounding boxes. A
mouse drag maps viewport coordinates back to page coordinates, selects the words
whose boxes fall between anchor and cursor, and paints a translucent highlight
over them. Ctrl+C joins the selected words' text.

**Search.** `Poppler::Page::search()` per page, run over the whole document on
submit, results stored as a list of (page, rect). Next/previous scrolls to the
hit and paints a highlight in the derived contrast colour.

Search runs off the main thread so the UI never blocks on a large document. The
worker walks pages in order and emits each hit as it is found, plus a page-index
progress signal. The search bar shows a thin progress indicator and a cancel
button while a pass is running; Esc cancels as well. Hits appear and become
navigable as they arrive rather than only at the end, so a hit on page 3 of a
1,000-page document is usable immediately. Cancelling stops the worker and keeps
whatever hits were already found. Starting a new search cancels any pass still
in flight.

Poppler document objects are not thread-safe, so the worker opens its own
`Poppler::Document` handle on the same path rather than sharing the view's.

**Printing.** `QPrintDialog` supplies the page range; MERGEN honours it and
renders each selected page at the printer's resolution rather than reusing the
screen cache.

**Threading.** v1.0 renders synchronously on the main thread. If large pages
feel sluggish in practice, move rendering to a `QThreadPool` — but only after
measuring, not pre-emptively.

---

## 9. Milestones

Each milestone ends with a working build and a local commit and tag. Nothing is
pushed and nothing is built into a package until M7 — this keeps the token cost
of milestones low and defers all release machinery to one run.

Version scheme: `v0.1.0` at M1, incrementing the patch per milestone, becoming
`v1.0.0` at M7.

### M1 — Skeleton and first page — `v0.1.0`

Build system, window, and a page on screen. Nothing interactive.

1. Create the repo layout: `src/`, `build/docs/MX.md`, `CMakeLists.txt`,
   `LICENSE` (GPL-3.0-only), `.gitignore`, `.clang-format` (LLVM base, 4-space
   indent, 100-column limit).
2. `CMakeLists.txt` targeting C++20, finding `Qt6::Widgets` and `poppler-qt6`
   via `pkg-config`.
3. `document.cpp` — open a PDF path, expose page count, page size, and render a
   page index to `QImage` at a given scale.
4. `mainwindow.cpp` — a `QMainWindow` with an empty toolbar and a central widget.
5. `pageview.cpp` — render page 0 only, centred, at 100%.
6. `main.cpp` — read `argv[1]`; if absent, start empty.
7. Initialise the repo on branch `ata`.
8. Verify: `cmake -B build -G Ninja && cmake --build build && ./build/mergen some.pdf`
   shows page one.
9. Commit and tag:

```bash
git add -A
git commit -m "M1: project skeleton, poppler document wrapper, single-page render"
git tag v0.1.0
```

### M2 — Continuous scroll — `v0.1.1`

1. Lay all pages out in one vertical column with a fixed inter-page gap.
2. Compute total document height; drive a `QScrollArea` or a manual scrollbar.
3. Cull to the viewport plus one page of margin above and below.
4. Cache rendered pages as `QImage` keyed by `(index, zoom)`.
5. Smooth pixel-based scrolling.
6. Wire ↑ ↓, PgUp, PgDn, Home, End, and the mouse wheel.
7. Verify: a 300-page PDF scrolls smoothly end to end and memory does not climb
   with page count.
8. Commit and tag:

```bash
git add -A
git commit -m "M2: continuous scroll, viewport culling, page cache"
git tag v0.1.1
```

### M3 — Zoom and rotate — `v0.1.2`

1. Zoom in/out in 10% steps, bounded 10%–1000%; reset to 100%; Ctrl+wheel.
2. Fit-width and fit-page, recomputed on window resize. Fit-width is the default
   on opening a document.
3. Rotation in 90° steps, applied document-wide.
4. Clear the page cache on any zoom or rotation change.
5. Keep the scroll anchor stable across zoom — the page under the viewport
   centre stays under the viewport centre.
6. Verify: zooming does not jump position; fit-width survives a window resize.
7. Commit and tag:

```bash
git add -A
git commit -m "M3: zoom steps, fit modes, rotation, anchored zoom"
git tag v0.1.2
```

### M4 — Toolbar and file opening — `v0.1.3`

1. Build the toolbar with Nerd Font glyph icons plus text labels.
2. Window title string, updated on open and on close.
3. Open button and Ctrl+O via `QFileDialog::getOpenFileName`, filtered to `*.pdf`.
4. Editable page counter, right-aligned; typing a number and pressing Enter jumps.
   Counter updates as the user scrolls.
5. `recent.toml` read on start, written atomically on each open; rolling ten;
   dropdown on the Open button, missing paths greyed.
6. Error text in the empty page view for unreadable or non-PDF files.
7. Password prompt via `QInputDialog` with retry loop; password never persisted.
8. `QFileSystemWatcher` on the open document with an inline reload notice.
9. Verify: opening from the dialog, from the dropdown, and from `argv[1]` all
   land on the same code path.
10. Commit and tag:

```bash
git add -A
git commit -m "M4: toolbar, file dialog, page jump, recent files, password prompt, file watcher"
git tag v0.1.3
```

### M5 — Text selection and search — `v0.1.4`

The most delicate milestone. Expect this one to take longer than the others.

1. `textList()` per page, cached alongside the rendered image.
2. Map viewport coordinates to page coordinates accounting for zoom and rotation.
3. Drag-select across words and across page boundaries; paint a translucent
   selection highlight using `QPalette::Highlight`.
4. Ctrl+C copies the selected text, joined with spaces and newlines at line ends.
5. Search bar on Ctrl+F, `Poppler::Page::search()` across all pages, hit list
   stored as (page, rect).
6. Run the search on a worker thread with its own `Poppler::Document` handle.
   Emit hits as they are found and a page-progress signal as it walks.
7. Progress indicator and a cancel button in the search bar; Esc cancels a
   running pass and closes the bar. Cancelling keeps the hits already found.
   Starting a new search cancels any pass still in flight.
8. Enter and Shift+Enter scroll to next and previous hit; hits painted in the
   derived contrast colour; Esc closes and clears highlights.
9. Verify: selection coordinates stay correct at 250% zoom and rotated 90°.
10. Verify: searching a 1,000-page PDF leaves the UI responsive, shows progress,
    and cancels immediately when asked.
8. Commit and tag:

```bash
git add -A
git commit -m "M5: text selection, clipboard copy, document search"
git tag v0.1.4
```

### M6 — Printing, About, desktop integration — `v0.1.5`

1. Ctrl+P and toolbar Print via `QPrintDialog` and `QPrinter`; honour the page
   range; render at printer resolution rather than reusing the screen cache.
2. About dialog — maker, version, release date, source address, full licence
   text. Addresses are selectable and **not** clickable; the app opens no browser.
3. `mergen.desktop` with `Exec=mergen %f`, `MimeType=application/pdf;`, and the
   app icon.
4. Application icon installed to the hicolor theme path.
5. Verify: after running `xdg-mime default mergen.desktop application/pdf`,
   `xdg-mime query default application/pdf` returns `mergen.desktop`.
6. Commit and tag:

```bash
git add -A
git commit -m "M6: printing, About dialog, desktop entry and MIME association"
git tag v0.1.5
```

### M7 — Packaging, CI, release — `v1.0.0`

The only milestone that pushes, builds a package, or publishes anything.

1. Write `PKGBUILD` — `pkgname=mergen`,
   `depends=('qt6-base' 'poppler' 'ttf-cascadia-code-nerd')`,
   `makedepends=('cmake' 'ninja' 'gcc')`, `arch=('x86_64')`, `license=('GPL3')`,
   standard `build()` and `package()` functions.
2. Test the package locally in a clean chroot:

```bash
extra-x86_64-build
```

3. Install and smoke-test the built package:

```bash
sudo pacman -U mergen-1.0.0-1-x86_64.pkg.tar.zst
mergen ~/some.pdf
```

4. Write `.github/workflows/release.yml` — a single job on an
   `archlinux:base-devel` container, triggered by a tag matching `v*`, that
   builds the package and uploads the `.pkg.tar.zst` to a GitHub Release.
5. Commit everything:

```bash
git add -A
git commit -m "M7: PKGBUILD and release workflow"
```

6. Create the repo on GitHub under the `sudo-megas` account, then push:

```bash
git remote add origin git@github.com:sudo-megas/MERGEN.git
git push -u origin ata
git push --tags
```

7. Tag and push the release tag, which triggers the workflow:

```bash
git tag v1.0.0
git push origin v1.0.0
```

8. Verify the Actions run succeeded and the release page carries the
   `.pkg.tar.zst` artefact.
9. Rollback if the workflow misfires — delete the tag locally and remotely, fix,
   re-tag:

```bash
git tag -d v1.0.0
git push origin :refs/tags/v1.0.0
```

---

## 10. Release procedure

Only M7 releases. The rule for this project, replacing the per-milestone
build-and-tag protocol used on TRITIUM:

- **M1 through M6:** commit locally, tag locally. No push, no build, no release.
- **M7:** commit, push, tag, push the tag, let Actions build and publish.

All commits come from the `sudo-megas` account. No AI attribution appears in any
commit message, code comment, or About text.

The README is deliberately deferred to an unscheduled later session and is not
part of any milestone. When it is written it follows the family convention, and
it must carry this line:

```bash
xdg-mime default mergen.desktop application/pdf
```

---

## 11. Working agreement — unattended builds

The build runs unattended. The user is not watching and will not be available to
answer mid-run. When something comes up, take the recommended action and keep
going; do not stop and wait.

**Environment that can be assumed:**

- `gh` is authenticated as `sudo-megas` and the token has repo scope. Creating
  the repo, pushing, and publishing the release need no interactive login.
- Arch Linux, Wayland, Niri. `base-devel`, `cmake`, `ninja`, `git`, `qt6-base`,
  `poppler-qt6` and a Nerd Font are present or installable with `yay -S`.
- Clean-chroot building via `extra-x86_64-build` is available.
- Package signing is the user's own step and happens outside this run.

**How to decide when unattended:**

- Anything already ruled in this document is settled. Re-read the relevant
  section rather than re-deciding it.
- A choice this document does not cover: pick the option most consistent with
  what it does say — fewest dependencies, least state, no dialogs, no network —
  then record the choice in the section it belongs to as part of the same
  commit. The document stays the source of truth.
- A choice that would breach §4: do not take it. Stop at that milestone, leave
  the working tree buildable, and leave a note in the commit body explaining
  what was blocked and why.
- A dependency that seems necessary but is not in §2: prefer writing the code by
  hand. Add a dependency only if hand-writing it would be unreasonable, and say
  so in §2 with the reasoning, in the same voice as the existing entries.
- Build failures, API mismatches, and poppler signature differences: fix them and
  carry on. Do not report back for permission.
- If a milestone's verification step cannot pass, fix it before tagging. Never
  tag a milestone whose verification failed.

**What never happens unattended, regardless:**

- No push and no release before M7, per §10.
- No AI attribution in any commit message, code comment, or About text.
- No commits from any account other than `sudo-megas`.
- Nothing on the §4 list gets built, however convenient it seems at the time.
