# MERGEN — Specification and Milestones, v2.0

A minimal PDF viewer for Arch Linux. Qt6 + poppler-qt6, no KDE Frameworks, no
desktop-environment dependencies.

This is the single source of truth for the project. It supersedes `MX.md`,
which described the closed v1.0 specification and is kept in the repository,
unedited, as the sealed record of how v1.0 was built and why. Where this
document and `MX.md` disagree, this document wins. Where this document is
silent and `MX.md` is not, `MX.md` still describes the shipped behaviour and
remains the explanation for it.

Spec and milestones live in this one file; changes are made by editing the
section they belong to, not by appending new documents.

- **Repo:** `github.com/sudo-megas/MERGEN`
- **Binary:** `mergen`
- **Licence:** GPL-3.0-only
- **Platform:** Arch Linux only, Wayland only
- **Default branch:** `ata`
- **Docs path:** `build/docs/MZ.md`
- **Supersedes:** `build/docs/MX.md` (v1.0, sealed)
- **Programme:** Z1–Z10, `v1.1.0` → `v2.0.0`

---

## Table of contents

1. [Why v2.0 exists](#1-why-v20-exists)
2. [What changed from MX.md](#2-what-changed-from-mxmd)
3. [Environment and dependencies](#3-environment-and-dependencies)
4. [Feature set — v2.0](#4-feature-set--v20)
5. [DO-NOT list](#5-do-not-list)
6. [Interface](#6-interface)
7. [Keybindings](#7-keybindings)
8. [State on disk](#8-state-on-disk)
9. [Architecture](#9-architecture)
10. [Milestones](#10-milestones)
11. [Release procedure](#11-release-procedure)
12. [Working agreement — unattended builds](#12-working-agreement--unattended-builds)
13. [Open rulings](#13-open-rulings)

---

## 1. Why v2.0 exists

v1.0 answered a narrow question well: take poppler, put a plain Qt6 widget
shell around it, and refuse everything else. That shipped, and the refusal was
most of what made it good.

v2.0 does not abandon that. It asks a second question the first one never
tested: **which of v1.0's refusals were principles, and which were merely
convenient?**

Some were principles. No network, no telemetry, no theme switcher, no
onboarding, no tray icon — these are what the application is, and they are
untouched below. Some were conveniences that hardened into rules because
nothing pushed on them. "No sidebar" was a rule against permanent chrome
eating the window; it was never a rule against showing the reader a table of
contents. v1.0 could not tell those apart because it never had to.

v2.0 tells them apart. The rule that survives is stated as what it actually
protects, and the feature it was accidentally forbidding gets built in a form
that does not violate it. A table of contents summoned by a keystroke and
gone on `Esc` is not a sidebar. A preview of a link target under a held cursor
is not a thumbnails panel. The chrome budget of the window is unchanged —
still one toolbar and the page — and that is the actual invariant.

The second thing v2.0 does is stop being purely a renderer of other people's
pages. Documents arrive with annotations already in them; v1.0 ignored them,
which means a page someone else highlighted rendered as though they never had.
That is not minimalism, it is an omission. Reading a PDF faithfully includes
reading what is written on it.

The third thing is the part with nothing to compare against. MERGEN's readers
run Arch on a tiling compositor and script everything else they own; a viewer
they cannot drive from a keybinding is a viewer that does not belong to their
desk. A viewer that cannot show them what changed between two revisions of the
same paper makes them open two windows and squint. A viewer that lets them
draw a black box over a name and call it redacted is lying to them. These are
not features borrowed from Acrobat's menu; three of the four have no equivalent
in this class of viewer at all, and that is the reason to build them.

The dependency objection remains the usual one for this family: not the count,
but deps that earn nothing. Everything added below is argued for in §3 or it
does not enter.

---

## 2. What changed from MX.md

Recorded here so the difference is legible in one place rather than inferred by
reading two documents against each other.

| Area | v1.0 (`MX.md`) | v2.0 (this document) |
|---|---|---|
| Feature list | Seven features plus printing; "closed for v1.0" | Reopened; §4 |
| Sidebars/panels | "Any sidebar or side panel at all" banned | Ban narrowed to *persistent* chrome; transient overlays permitted — §5 |
| Outline / TOC | Banned as a sidebar | Permitted as a transient overlay — Z3 |
| Thumbnails | Panel banned | Panel still banned; hold-to-peek of one target permitted — Z3 |
| Annotations | "Out of scope for v1.0," not banned | Rendering in scope (Z6); creation is an open ruling — §13 |
| Page colour | System palette only | Night mode as a render-time transform — Z5 |
| Toolbar icons | Glyph as `QAction` text | Glyph rasterised into a real multi-state `QIcon` — Z1 |
| Motion | None; every transition snaps | Tweened zoom/jump/scroll, momentum scrolling — Z7 |
| Writing files | `recent.toml` only | Adds `portals.toml`; redaction writes a new PDF on request — §8 |
| Scripting | None | Unix-socket control surface — Z8 |
| Milestones | M1–M7, patch bumps, `v0.1.0`→`v1.0.0` | Z1–Z10, minor bumps, `v1.1.0`→`v2.0.0` |
| Constitution | `MX.md` | This document; `MX.md` sealed |

Everything not in this table is unchanged and still governed by the reasoning
in `MX.md`.

---

## 3. Environment and dependencies

Written for and tested on: Arch Linux, Wayland, Niri, `prefer-no-csd` enabled.

| Dependency | Why it is here |
|---|---|
| `qt6-base` | Widgets, painting, scroll view, input handling, file dialog, printing. Also supplies QtNetwork, which provides the local socket used in §9 — a module of a package already present, not a new package |
| `poppler-qt6` | PDF parsing, page rendering to `QImage`, text extraction, search, annotation and outline access |
| `ttf-cascadia-code-nerd` | Toolbar icon glyphs — see §6. Verified against 3.5.0-1 (Nerd Fonts 3.5.0, font revision 2407.024): all nine code points MERGEN uses are present. Qt6 does not fall back to another installed font for a missing glyph the way Qt5 did (QTBUG-110502), so this is checked rather than assumed — and `IconSet` refuses to draw a glyph the font does not carry, leaving the label alone rather than a tofu box |
| `polkit` | `pkexec`, for opening PDFs the reader may not read |
| `qpdf` | Content-level redaction — see below. **Build-time only since 2.0.1** |

Build-time only: `cmake`, `ninja`, `gcc`, `qpdf`.

> [!NOTE]
> **Amended at 2.0.1.** `qpdf` moved from `depends` to `makedepends`. Redaction
> was withdrawn at Z11 (§13) and its code, though still compiled, is called from
> nowhere; makepkg builds with LTO, so the optimiser drops those functions as
> unreachable, the references to `libqpdf` go with them, and `--as-needed` then
> leaves no `NEEDED` entry at all. Verified against the published 2.0.0 binary:
> no `libqpdf` in `readelf -d`, no qpdf symbols, no qpdf strings. Every reader
> was installing a library their copy never loads.
>
> It stays a build dependency because the source still has to compile and link
> against it before LTO removes it. **If redaction is ever reachable again this
> moves back** — the thing to check is the binary, not the version number.

**`qpdf` is the one new package, and it is here for exactly one job.** Redaction
that draws an opaque rectangle over a name and saves the file is not redaction;
the text sits underneath, selectable, and every tool that ships it that way is
handing the reader a false guarantee. Removing the content requires rewriting
the page's content stream, which poppler-qt6 does not do — it is a renderer,
and asking it to become a writer would be asking the wrong library. `qpdf` is
the standard tool for exactly this transformation, it is a single C++ library
with no desktop stack behind it, and the alternative is either shipping the lie
or hand-writing a PDF content-stream rewriter, which is unreasonable. It earns
its place. It is linked only by the redaction path; nothing else in MERGEN
calls it.

The control surface in Z8 is a Unix domain socket via `QLocalServer`, **not**
D-Bus. Both are available inside `qt6-base` at no new package cost, so the
choice is not about dependencies but about what has to be running: a session
bus is desktop-environment infrastructure, and this project has refused to
require any since v1.0. A socket in `$XDG_RUNTIME_DIR` needs nothing to exist
beyond the kernel, works identically under a bare compositor with no session
manager, and is drivable from a shell one-liner without a client library. That
is the correct shape for readers who script their window manager.

Nothing else. No KF6, no GTK, no glib, no network library, no database.

CUPS arrives indirectly through `qt6-base`'s print support and is accepted
deliberately, as in v1.0.

> [!NOTE]
> Qt has no desktop-portal theme hint on a bare Wayland session, so MERGEN
> renders in Qt's light palette unless `QT_QPA_PLATFORMTHEME` is set. This is
> Qt behaviour outside a full DE, not a MERGEN bug, and MERGEN does not set the
> variable itself. Since Qt 6.5 the system palette is loaded and updated live
> where the platform reports one, so no in-application colour-scheme listener
> is needed or wanted — a reader whose dark mode does not follow should
> configure `qt6ct` or their portal, which are the correct place for it. This
> is unchanged from v1.0 and is restated because it is the most common thing
> mistaken for a missing feature.

---

## 4. Feature set — v2.0

Everything in v1.0 §3 remains, unchanged, and is not restated. Added:

| Feature | Notes |
|---|---|
| Annotation rendering | Annotations already present in a document are drawn. Creation is not in this list — see §13 |
| Night mode | Luminance-only inversion of the rendered page. Hue and saturation survive, so a red chart stays red and a photograph does not become a negative. Session-scoped, no persistence |
| Outline overlay | The document's own table of contents, summoned by keystroke, jump to section, gone on `Esc` |
| Command overlay | One surface for search, page jump and every action. `Ctrl+K` |
| Hold-to-peek | Holding the pointer over an internal link shows its target in place, without navigating |
| Document properties | Version, producer, fonts, permissions, and a plain warning when a file embeds JavaScript, forms or attachments |
| Presentation mode | Fullscreen, one page at a time, chrome withdrawn, letterboxed |
| Control socket | `open`, `goto`, `search`, `next`, `prev`, `quit` over a Unix socket in `$XDG_RUNTIME_DIR` |
| Compare | Two revisions of a document side by side, differing regions marked |
| Portals | A reader-made two-way link between locations, within a document or across two |
| ~~Redaction~~ | **Withdrawn at Z11.** Built, audited, and found to fail open by four routes. The code remains; the feature is not offered — see §13 |
| Motion | Zoom, page jump and search navigation are tweened; scrolling carries momentum |

---

## 5. DO-NOT list

Authored by the user. Nothing is added to or removed from this list without a
ruling. This section supersedes `MX.md` §4.

> [!NOTE]
> **Amended at Z12, 24/08/2026.** Three entries below are struck rather than
> deleted, on the author's ruling: *"dont bother about forbiddances. i allow
> you."* They were the rules against a persistent side panel, against a page
> thumbnails panel, and against a status bar.
>
> The reasoning §1 gives still holds — the rule was against *permanent chrome
> eating the window*, not against showing the reader what is in their document.
> What shipped is held to that: the previews take a fifth of the row and fold
> away with `Ctrl+B`; they step aside entirely for presentation and for a
> comparison; the status bar answers two questions a reader actually asks of a
> file (where is it, and may I write to it) in two lines and no more.
>
> Struck through rather than removed, because a rule that was once thought
> necessary is worth being able to see.

**Never, at any version:**

- Form filling
- Digital signatures
- Any format other than PDF — no DjVu, no PostScript, no EPUB, no comic archives
- Bookmarks
- Tabs, or a multi-document workspace
- Remember last page per file
- ~~Any *persistent* sidebar, side panel, dock or status bar~~ — **struck at Z12**
- ~~A page thumbnails *panel*~~ — **struck at Z12**
- Network access of any kind — no update check, no telemetry, no analytics, no
  crash reporting
- Opening a URL — addresses are selectable text, never clickable
- Tray icon
- File-manager context menu integration
- Multiple themes or a theme switcher
- Hardcoded window geometry — the compositor decides
- Storing passwords
- AI attribution anywhere in the repo, commits, or About
- X11 support
- Debian, Windows or macOS packaging
- First-run prompts, onboarding, or nagging dialogs
- Editing a PDF in place — every write produces a new file
- Redaction that only draws over content without removing it

**Rulings of this document.** Four v1.0 bans are restated above in narrower
form. The narrowing is deliberate and is recorded here rather than left to be
noticed:

1. **"Any sidebar or side panel at all"** protected the window from permanent
   chrome. It now says *persistent*, because that is what it was defending. A
   surface summoned by a keystroke, drawn over the page, and dismissed on `Esc`
   costs the window nothing when it is not there. The toolbar remains the only
   permanent chrome, and no dockable panel is ever added.
2. **"Outline / table-of-contents sidebar"** banned the sidebar, and the
   sidebar is still banned. The table of contents is not.
3. **"Page thumbnails panel"** banned the panel, and the panel is still banned.
   Showing one link target under a held cursor is not a panel and does not
   become one.
4. **"Tabs or multiple documents in one window"** protected against a
   multi-document workspace with per-document state to keep in sync. Compare
   mode holds two documents for the length of a comparison and offers no
   tab strip, no document switcher, and no second independent view state. It
   is a mode, not a workspace, and it exits back to one document.

**Portals and the "remember last page per file" ban.** That ban is about the
application quietly recording where the reader was. A portal is the opposite:
the reader creates it deliberately, at a place they chose, and it exists
because they said so. Nothing about a reader's position is ever written
without them asking for it.

**Deliberately left open for a later version:**

- Annotation *creation* — see §13

---

## 6. Interface

One window. A top toolbar, and beneath it the page view. No sidebar, no status
bar, no menu bar. Overlays are drawn over the page view and belong to no
permanent layout.

**Toolbar, left to right:**

`Open` (with a dropdown arrow for the recent-files list) · `Zoom out` ·
`Zoom in` · `Fit width` · `Fit page` · `Rotate` · `Search` · `Print`

Unchanged from v1.0, with three additions to how it is drawn:

**Icons are real icons.** The glyph is rasterised through `QPainter` into a
cached pixmap tinted from the live palette and attached with
`QAction::setIcon()`; `QAction::setText()` carries a human-readable label. In
v1.0 the glyph *was* the text, which meant an icon-only toolbar would have
hidden it, assistive technology read a private-use codepoint aloud, and
disabled and hovered states relied on whatever dimming the text happened to
get. All three follow from the same mistake and are fixed by the same change.
Every action additionally carries an accessible name and a tooltip naming its
shortcut — with no menu bar, the tooltip is the only place a shortcut is
discoverable, which makes it load-bearing here in a way it is not elsewhere.

**The toolbar is grouped.** Separators divide it into open · zoom and fit ·
rotate, search and print. It is the whole of MERGEN's chrome; the reading is
worth the two separators.

**Hover, press and disabled are drawn deliberately.** v1.0 left them to
whichever `QStyle` the reader had, which meant the most-touched surface in the
application looked different, and sometimes looked like nothing, depending on
their configuration. A narrow paint path draws a palette-derived highlight for
hover and press and a palette-derived dimming for disabled. No stylesheet is
used and no colour is hardcoded — see §9.

**Overlays.** One reusable surface, used by the outline, the command overlay,
document properties and hold-to-peek. It is a frameless child of the page view,
horizontally centred and offset from the top, sized to its content within a
fraction of the viewport, painted in `QPalette::Window` over a dimmed page with
a soft shadow — the one place in the application where elevation is honest,
because it is the one surface genuinely floating above another. It takes focus
when shown, returns it when hidden, and closes on `Esc`, on focus loss, and on
choosing an entry. It is never docked, never resizable, and never persists
across documents.

**Night mode** inverts lightness only. The page's own colours keep their hue
and saturation, so figures remain readable and photographs remain
photographs. The toolbar, the overlays and every dialog follow the system
palette exactly as before, because MERGEN does not theme itself.

> [!NOTE]
> **Amended at Z5.** This section first said night mode applies to the
> rendered page "and to nothing else". It also inverts the canvas the pages
> sit on. That canvas is not chrome — it is the surround of the page itself,
> and on a light system theme a dark page on a bright surround leaves most of
> the glare the mode was turned on to remove. It is inverted by the same
> transform, from `QPalette::Base`, so it stays derived from the reader's
> palette rather than named. Nothing else moves.

**Presentation mode** takes the window fullscreen, withdraws the toolbar,
fills the surround with black rather than `QPalette::Base`, and shows one page
at a time fitted to the screen. `Esc` leaves. Nothing is configurable about it.

**Compare mode** shows two documents side by side, scroll-locked, with
differing regions marked in the derived contrast colour already used for search
hits. It is entered explicitly, exits to the left-hand document, and adds no
persistent chrome.

**Window title,** one string, since compositor-drawn decorations have no zones:

```text
MERGEN ///— document.pdf —\\\ MEGAS
```

**Empty state.** Launched with no argument, MERGEN shows the page view with the
toolbar active and the whole window as a drop target. Behind the centred text
sits a faint watermark drawn from the application icon's own curled-corner
motif — the icon is real illustration work and it otherwise stops at the
desktop entry. The text is one line naming the two ways in, shown whenever
there is no document and nothing else to say. It is not onboarding: it never
appears over a document, is dismissed by using the application rather than by
being acknowledged, and says the same thing on the thousandth launch as on the
first. Error messages for unreadable or non-PDF files replace that line in this
same view, never as a dialog box.

Only one PDF may be dropped. MERGEN holds one document, and a folder or a
second file is not a smaller version of that request, so it is refused at the
door rather than half-honoured.

**Motion.** Zoom changes, page jumps and search navigation are tweened over
roughly 150–250 ms with an ease-out curve rather than snapping, and scrolling
carries momentum. Motion exists to keep the reader's place, not to decorate:
nothing bounces, nothing overshoots, and every animation is skipped entirely
when the platform reports that the reader has asked for reduced motion.

**Password-protected PDFs, root-owned files, file-change watching, and
decorations** are unchanged from `MX.md` §5.

---

## 7. Keybindings

Standard desktop conventions. No vim-style bindings anywhere. Everything in
`MX.md` §6 stands unchanged; added:

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Command overlay |
| <kbd>Ctrl</kbd>+<kbd>T</kbd> | Outline overlay |
| <kbd>Ctrl</kbd>+<kbd>I</kbd> | Document properties |
| <kbd>Ctrl</kbd>+<kbd>N</kbd> | Night mode on / off |
| <kbd>F5</kbd> | Presentation mode |
| <kbd>Ctrl</kbd>+<kbd>D</kbd> | Compare with another document / leave compare |
| <kbd>Ctrl</kbd>+<kbd>M</kbd> | Mark one end of a portal, or complete it |
| <kbd>Ctrl</kbd>+<kbd>J</kbd> | Follow the portal on this page |
| <kbd>Esc</kbd> | Close the frontmost overlay, or leave presentation mode |

`Esc` already closed the search bar in v1.0 and now closes whatever transient
surface is frontmost, innermost first. It never closes the document.

---

## 8. State on disk

MERGEN writes two files, both under `~/.local/state/mergen/`:

```text
recent.toml     the last ten opened paths, unchanged from v1.0
portals.toml    reader-created portals
```

`recent.toml` is exactly as specified in `MX.md` §7 and is not revisited.

`portals.toml` holds only what the reader explicitly made: for each portal, the
two endpoints as (document identity, page, point). Documents are identified by
a hash of their bytes rather than their path, so a portal survives the file
being moved and does not silently attach itself to a different document that
happens to occupy the old name. Written atomically — temp file, `fsync`,
rename. If it is missing, unreadable or malformed, MERGEN starts with no
portals and overwrites it on the next one made; a bad state file is never an
error the reader sees. A portal whose document cannot be found is kept, not
deleted — the file may simply not be mounted today.

Nothing else is persisted. No window geometry, no zoom level, no page number,
no night-mode preference, no passwords, no settings.

Redaction writes a new PDF to a path the reader chooses. It never modifies the
open document, and the open document is never the destination.

---

## 9. Architecture

The v1.0 architecture stands: four translation units in the viewer plus the
one-file privileged helper, no plugin system, no abstraction layer over
poppler, no generator indirection. Everything in `MX.md` §8 — the rendering
model, zoom and fit behaviour, selection, search threading, elevation,
printing — remains correct and is not restated. v2.0 adds three pieces of
shared machinery and then builds on them.

| File | Responsibility |
|---|---|
| `main.cpp` | `QApplication`, parse `argv[1]`, construct the window |
| `mainwindow.cpp` | Toolbar, actions, keybindings, file dialog, print, recent list, password prompt, file watcher, control socket |
| `pageview.cpp` | The scrolling page column, and the surface overlays are drawn over |
| `document.cpp` | Thin wrapper over `Poppler::Document` |
| `overlay.cpp` | The one transient floating surface, and its four users |
| `iconset.cpp` | Glyph rasterisation and the palette-derived icon cache |
| `mergen-open.cpp` | The privileged reader, unchanged |

**The three foundations, built first and deliberately.** Each is built once and
paid for by several features, which is why §10 orders them ahead of everything
they serve.

*The icon set.* A glyph, a size and a palette role in; a cached `QIcon` out,
carrying Normal, Disabled and Active pixmaps generated from the same glyph at
different palette colours. Keyed by all three inputs and cleared whenever the
palette changes, since a cached pixmap tinted for the old palette is wrong the
moment the reader's theme changes. The font is loaded once; if it is absent the
labels stand alone, as in v1.0 — a missing font costs the glyphs, not the
toolbar.

*The overlay surface.* One frameless widget over the page view that manages
its own show, focus, dismissal and shadow, and knows nothing about its
contents. Its four users supply a model and a title and handle a chosen row.
Building this once is what makes the outline, the command overlay, properties
and hold-to-peek four small features instead of four medium ones, and it is
what keeps them behaving identically — a reader who learns `Esc` learns it once.

*The render hook.* A transform stage between poppler's decode and the page
cache. Night mode is the first user and is one pass over the decoded image;
annotation compositing is the second. A page rendered inverted and a page
rendered plainly are not the same image, and v1.0's cache key would have
happily returned one for the other.

> [!NOTE]
> **Amended at Z5.** This section first said the cache key gains the transform
> state. The cache is cleared when the mode changes instead, which is what
> zoom and rotation already do and answers the same correctness worry without
> holding two copies of every visible page for a mode the reader is not
> looking at. Toggling is rare; the pages re-render on the next paint.

The inversion itself is worth stating, because the obvious implementation is
wrong. Straight RGB inversion is one call and turns every photograph into a
negative and every red chart cyan, which is why so many tools offering a dark
PDF mode are unusable on anything but plain text. Inverting lightness while
holding hue and saturation has a closed form: in HSL the chroma
`C = (1 - |2L-1|) · S` is unchanged by `L → 1-L`, so only the offset
`m = L - C/2` moves, and the whole transform collapses to adding
`255 - (max + min)` to every channel. It is its own inverse, so toggling twice
returns the original image exactly.

**Annotation rendering.** Poppler composites annotations into the page inside
`renderToImage()`, in the geometry and at the zoom and rotation it was asked
for. MERGEN has therefore always drawn them.

> [!NOTE]
> **Amended at Z6.** This section first described a compositing layer in
> MERGEN: read the annotations, transform their geometry, paint them over the
> rendered page, keeping the rects unrotated the way search hits are. None of
> that is needed, and it was written on an assumption that was never checked.
> Poppler draws annotations itself unless asked not to, and measured against
> `pdftoppm` on the same file MERGEN's output already matched it exactly —
> highlight, underline and square all present, correct at 250% and rotated.
> Building the layer would have duplicated poppler and risked drawing markup
> twice.
>
> What was actually wrong is narrower and worth fixing: MERGEN drew
> annotations *by default rather than by decision*. `HideAnnotations` is a
> render hint poppler simply had switched off; nothing in MERGEN said it
> wanted annotations, so a change of default would have silently taken them
> away. The hint is now set explicitly. The behaviour is unchanged and the
> intent is on the record.

**Night mode.** Lightness is inverted; hue and saturation are left alone.
Straight RGB inversion is one call and would have been cheaper, but it turns
every photograph into a negative and every red chart cyan, which is why so many
tools that offer a dark PDF mode are unusable on anything but plain text. The
transform runs once per page render, into the cache, not per paint.

**The control socket.** `QLocalServer` on `$XDG_RUNTIME_DIR/mergen-$UID.sock`,
one line per command, replying `ok` or `err: <reason>`. Commands are `open
<path>`, `goto <page>`, `search <text>`, `next`, `prev`, `quit`, and nothing
else. It accepts local connections only, by construction — a Unix socket has no
network surface, which is what keeps this consistent with the standing ban.
Every command is one the reader could already have issued from the keyboard;
the socket adds reach, not capability. A second instance finding a live socket
hands its argument to the first and exits, which is how `mergen file.pdf` from
a keybinding raises the window already open rather than starting a second one.

**Compare.** Two `Document` handles, two page columns, one scroll position.
Differences are found by rendering both pages at the same scale and comparing
them in bands, marking the bands that differ. This is a visual diff, not a
semantic one: it reports that a region changed, not what it means, and it says
so plainly rather than implying more precision than it has.

**Portals.** A portal is two endpoints and nothing else. Following one is a
jump to the other; if the far document is not open, MERGEN opens it first.
They are made explicitly, listed in the command overlay, and stored per §8.

**Redaction.** The reader selects text and asks for it to be removed. The open
document is untouched, the destination is never the source, and the result is
verified by searching the output for the removed text before the reader is told
it succeeded — a redaction tool that cannot demonstrate the text is gone is the
kind that ships the lie this project added a dependency specifically to avoid.

A show-text operator carries the text and not its position, so where it lands
has to be tracked through the graphics and text matrices that preceded it: `q`
and `Q`, `cm`, `BT`, `Tm`, `Td`, `TD`, `T*`, `TL`. Operators whose origin falls
inside the selected area are dropped, and a black rectangle is drawn over the
gap so the page shows that something was taken rather than quietly closing over
it.

> [!NOTE]
> **Amended at Z10.** `QPDFPageObjectHelper::filterContents` runs a filter
> through a pipeline and leaves the page untouched; only `addContentTokenFilter`
> rewrites the content stream, and it does so when the document is written
> rather than when it is asked. The first implementation used the former,
> reported success, and changed nothing — caught because the output was read
> back with `pdftotext` rather than trusted.

Two things it declines rather than half-does. A selection spanning two pages
has no single page to redact on, and removing part of it would be worse than
saying so. And a document that arrived through the elevation helper exists here
only as bytes, with no readable path for `qpdf` to work from.

The verification is not a formality. Given a request whose named text survives
the rewrite, the written file is deleted and the reader is told nothing was
saved — tested by asking for an area that holds one line while naming another,
and confirming the output is discarded rather than handed over as redacted.

**Motion.** `QPropertyAnimation` on the scrollbar's `value`, eased out over
200 ms, for page jumps and search navigation. Animations are interruptible and
always start from the value the bar actually holds rather than from where the
previous one intended to land, which is what stops a second jump mid-flight
from snapping. A relayout cancels any jump in flight, because it was aimed at
coordinates that have moved.

> [!NOTE]
> **Amended at Z7 — zoom is not animated.** This section first called for
> tweened zoom. Zoom changes the rendered size, so animating it means
> re-rendering every frame. Measured on this machine against a trivial page:
> 2.0 ms at 100%, 9.7 ms at 250%, 23.4 ms at 400% — already past the 16.7 ms a
> 60 Hz frame allows, on a page with nothing on it but five rectangles. A real
> document would be far worse, so tweened zoom would have made the application
> feel worse rather than better, which is the opposite of the point. Doing it
> properly means scaling the cached pixmap during the gesture and re-rendering
> sharp at the end; that is a genuine feature and it is not this milestone.
> §9 already says to move work off the main thread only after measuring — the
> same rule applied here, and the measurement said no.

> [!NOTE]
> **Amended at Z7 — there is no reduced-motion hint to honour.** This section
> first said every animation is checked against the platform's reduced-motion
> hint. Qt exposes none on this platform. The nearest thing,
> `QApplication::isEffectEnabled(Qt::UI_General)`, reads **false** under the
> wayland, minimal, offscreen and vnc plugins alike whenever no desktop
> environment has supplied `UiEffects` — which is exactly a bare Niri session,
> MERGEN's own target. Gating on it would have shipped motion that never once
> ran for the reader it was built for, and "the platform declined" cannot be
> told apart from "nobody was asked". Motion is therefore unconditional, and
> the decision lives in one function so a ruling has somewhere to land — see
> §13.

`QScroller` is not used. Qt's own wheel handling already delivers the
high-resolution pixel deltas a trackpad sends through libinput, which is where
smooth scrolling on this platform actually comes from; `QScroller` would add a
touch-gesture grab that competes with text selection for the left button and
buys nothing a Wayland trackpad does not already provide.

**Threading.** Rendering remains synchronous on the main thread, as in v1.0.
Night mode adds a per-page pass and compare adds a per-page comparison; if
either is measurably slow on real documents, move it to a `QThreadPool` — after
measuring, not before.

---

## 10. Milestones

Ten milestones, `Z1` through `Z10`. Each ends with a working build, a local
commit and a local tag. Versions bump the minor per milestone, and the tenth is
`v2.0.0` rather than `v1.10.0` — the tenth version of a series is `.0` of the
next.

Nothing is pushed and nothing is built into a package until `Z10`, exactly as
`M1`–`M7` behaved.

The first three milestones are foundations and deliberately ship little the
reader can see. They are first because everything after them is smaller for
their existing.

### Z1 — Icons, toolbar authorship, accessibility — `v1.1.0`

1. `iconset.cpp` — glyph rasterisation into palette-tinted, multi-state
   `QIcon`s, cached and invalidated on palette change.
2. Convert every toolbar action: `setIcon()` for the glyph, `setText()` for a
   real label.
3. Accessible name and a tooltip naming the shortcut on every action, the page
   counter and the recent-files dropdown.
4. Toolbar separators dividing open · zoom and fit · rotate, search and print.
5. Palette-derived hover, press and disabled painting.
6. Glyph optical alignment pass; verify each codepoint renders in the pinned
   font version and record that version.
7. Verify: the application icon is legible at 16, 22 and 24 px; no action is
   announced by a private-use codepoint; `Zoom in` visibly disables at 1000%.
8. Commit and tag:

```bash
git add -A
git commit -m "Z1: icon set, toolbar authorship, accessibility"
git tag v1.1.0
```

### Z2 — The overlay surface, and document properties — `v1.2.0`

1. `overlay.cpp` — frameless surface over the page view: show, focus, dismiss
   on `Esc` and focus loss, soft shadow, page dimming, content-sized within a
   fraction of the viewport.
2. Its first user: document properties on `Ctrl+I` — version, producer, page
   count, page size, fonts, permissions, and a plain warning when the file
   embeds JavaScript, forms or attachments.
3. Verify: the overlay never survives a document close, never takes focus it
   does not return, and a file with embedded JavaScript says so.
4. Commit and tag:

```bash
git add -A
git commit -m "Z2: transient overlay surface, document properties"
git tag v1.2.0
```

### Z3 — Outline overlay and hold-to-peek — `v1.3.0`

1. Outline on `Ctrl+T` from `Poppler::Document::toc()`, nested, jumping to a
   section on selection.
2. A document with no outline says so in the overlay rather than opening empty.
3. Hold-to-peek: holding the pointer over an internal link renders its target
   into the overlay without navigating; releasing dismisses it.
4. Verify: outline jumps land on the right page in a rotated, zoomed document.
5. Commit and tag:

```bash
git add -A
git commit -m "Z3: outline overlay, hold-to-peek link preview"
git tag v1.3.0
```

### Z4 — Command overlay — `v1.4.0`

1. `Ctrl+K` — one surface accepting a page number, a search term, or an action
   by name, with the intent disambiguated from what is typed.
2. Every toolbar action reachable by name; portals listed here once Z9 lands.
3. Verify: `Ctrl+K` then a number jumps; then text searches; then an action
   name runs it. The existing search bar and page counter still work exactly as
   before — this adds a route, it does not replace one.
4. Commit and tag:

```bash
git add -A
git commit -m "Z4: command overlay"
git tag v1.4.0
```

### Z5 — Render hook and night mode — `v1.5.0`

1. Transform stage between decode and cache; cache key extended with the
   transform state.
2. Luminance-only inversion, `Ctrl+N`, session-scoped.
3. Verify: a page with a colour figure keeps its hues inverted-lightness, not
   negated; toggling twice returns the original image; the cache never returns
   an inverted page for a normal one or the reverse.
4. Commit and tag:

```bash
git add -A
git commit -m "Z5: render transform stage, night mode"
git tag v1.5.0
```

### Z6 — Annotation rendering — `v1.6.0`

Smaller than planned, because the premise was wrong — see the amendment in §9.

1. Ask for annotations explicitly rather than inheriting poppler's default.
2. Report in the properties overlay how much markup a document carries, so a
   reader knows the yellow is someone else's and not part of the page. Links
   and form widgets are not counted: they are structure, not markup.
3. Verify: a document annotated in another viewer shows that markup in MERGEN,
   correct at 250% zoom, rotated 90° and 270°, and still legible under night
   mode.
4. Commit and tag:

```bash
git add -A
git commit -m "Z6: annotation rendering"
git tag v1.6.0
```

### Z7 — Motion, presentation mode, empty state — `v1.7.0`

1. Tweened zoom, page jump and search navigation; `QScroller` momentum;
   reduced-motion hint respected; animations interruptible from their current
   value.
2. Presentation mode on `F5` — fullscreen, toolbar withdrawn, black surround,
   one page fitted, `Esc` leaves.
3. Empty state: whole-window drop target, centred text, icon-motif watermark.
4. Verify: zooming twice quickly does not jump; setting the platform's
   reduced-motion preference removes all animation; dropping a PDF anywhere in
   the empty window opens it.
5. Commit and tag:

```bash
git add -A
git commit -m "Z7: motion, presentation mode, empty state"
git tag v1.7.0
```

### Z8 — Control socket — `v1.8.0`

1. `QLocalServer` on `$XDG_RUNTIME_DIR/mergen-$UID.sock`; `open`, `goto`,
   `search`, `next`, `prev`, `quit`; one line in, `ok` or `err: <reason>` out.
2. A second instance finding a live socket hands over its argument and exits.
3. A stale socket from a crashed instance is detected and replaced, not
   inherited.
4. Verify: driving MERGEN from a shell one-liner works with no session bus
   running; `mergen file.pdf` twice raises one window, not two.
5. Commit and tag:

```bash
git add -A
git commit -m "Z8: control socket"
git tag v1.8.0
```

### Z9 — Compare and portals — `v1.9.0`

1. Compare: two documents, scroll-locked, differing bands marked in the derived
   contrast colour. Entered explicitly, exits to one document.
2. Portals: make, follow, list in the command overlay, persist to
   `portals.toml` per §8, identified by content hash.
3. Verify: comparing a document with itself marks nothing; a portal survives
   its document being renamed; a portal to a missing document is kept and says
   so rather than vanishing.
4. Commit and tag:

```bash
git add -A
git commit -m "Z9: compare mode, portals"
git tag v1.9.0
```

### Z10 — Redaction, audits, packaging, release — `v2.0.0`

The only milestone that pushes, builds a package, or publishes anything.

1. Redaction: selection to content-stream extents, removal via `qpdf`, written
   to a new file chosen by the reader, **verified** by searching the output for
   the removed text before reporting success.
2. CI audit scripts, in the family pattern: attribution (no AI trailers or
   tooling credit anywhere in tree, commits or About), dependencies (nothing
   linked beyond §3), and strings (no hardcoded colours outside the derived
   palette paths). Gated on every push.
3. Reconcile packaging against the family: move `PKGBUILD` under `packaging/`.
   The licence stays `GPL-3.0-only`, which is what every source file in the tree
   already declares; it differs from TRITIUM's `-or-later` deliberately, since a
   licence is not something to widen for the sake of matching a sibling.
4. Update `PKGBUILD` for the `qpdf` dependency; test in a clean chroot with
   `extra-x86_64-build`; install and smoke-test the result.
5. Verify the release workflow still builds from a tag.
6. Commit, push, tag, push the tag, let Actions publish:

```bash
git add -A
git commit -m "Z10: redaction, CI audits, packaging"
git push origin ata
git tag v2.0.0
git push origin v2.0.0
```

7. Verify the Actions run succeeded and the release carries the
   `.pkg.tar.zst`.
8. Rollback if the workflow misfires — delete the tag locally and remotely,
   fix, re-tag.

---

### Z11 — The freeze audit, and what it cost — `v2.0.0`

Not planned. Added because the user asked whether v2.0 could be frozen, and the
honest way to answer was to look rather than to assert.

Eleven agents audited the tree in parallel — three Opus on redaction, the
privileged path and object lifetimes; four Sonnet on the socket, the state
files, resource growth and UI correctness; one Fable on build and packaging;
three more Opus on malformed input, the print path and an end-to-end threat
model. A session limit killed seven of them mid-work, but every agent journalled
continuously, so about 11,000 lines of findings survived rather than being lost
with the agents.

They produced **51 findings, numbered to V52 with no V42, 19 of them
CRITICAL**. Every one was independently
verified before a line was changed — an agent's confident false positive
"fixed" is a fresh bug, and three of these turned out to be exactly that.

The answer to the freeze question was **no**. Nine commits later it is yes.

1. **Z11a** — the two crashes and the guards that were not guarding.
   `isOpen()` returned true for a locked document, so every accessor
   dereferenced a null catalog: 295 of 295 encrypted mutants crashed. And
   poppler returns a **non-null 1×1 image** when it refuses an allocation,
   which made every `isNull()` check in the tree inert — black printed pages,
   documents reported "identical". Plus the privileged wipe, `PR_SET_DUMPABLE`,
   absolute `pkexec`, SIGXFSZ, and the outline recursion bound.
2. **Z11b** — withdraw redaction; make the audits audit. See §13.
3. **Z11c** — the control socket stops blocking, and stops re-entering.
4. **Z11d** — rotation-aware compare marks, one reset per document.
5. **Z11e** — portals as records, honest print range, cached properties.
6. **Z11f** — stop deleting through nested event loops; compare without the wait.
7. **Z11g** — bound the caches, unclamp the fit modes, verify a portal's far end.
8. **Z11h** — a long print can be watched, and stopped.
9. **Z11i** — closing during a search waits, rather than aborting.
10. **Z11j** — and that print loop is not a way back into the document.

Measured, before and after: entering compare on a 1000-page pair 9,100 ms →
12 ms; document properties 54 ms → 0 ms; five idle socket clients 5.0 s →
0.000 s; a 1000-page print 346 s frozen → progress bar with a working Cancel.

> [!NOTE]
> **Amended at Z11.** Two findings were *rejected* after measurement, and the
> rejections are part of the record. Selection line breaks were reported wrong
> at every non-zero rotation; they are not, because `setRotation` already clears
> the word cache and the selection. Cache growth was reported at 73.6 MB over a
> thousand pages; it measures 1.2 MB, because scrolling never fills the word
> cache — only selecting does. The prune was kept anyway, as a bound rather than
> a repair, and its commit message says so.

The full disposition of all 51 — fixed with a commit, ruled, rejected
with the evidence, or partial with the reason — is in `docs/audit/DISPOSITION.md`.
That directory is scratch and is not tracked; this paragraph is the tracked
record that it existed.

### Z12 — What the reader asked for, after looking at it — `v2.0.0`

Also not planned. The author ran the built application, looked at it properly,
and reported what was wrong with it. Everything here comes from that.

**The rendering.** Pages looked as though nothing had ever heard of
antialiasing. poppler's antialiasing was on the whole time; `PageView` had no
device-pixel-ratio handling at all, so on a 2x display every page was rasterised
at half the pixels it occupied and then scaled up by the compositor. Rendered at
real density now, with `setDevicePixelRatio` so every other coordinate stays
logical and nothing else had to change. The peek preview and the new previews
strip get the same treatment.

**The title said MERGEN twice.** Not this application's string: `main.cpp` set
an application *display name*, and Qt appends " — <display name>" to every
window title at the platform layer.

**A click could not clear a selection.** `positionAt` snaps to the nearest word,
so pressing empty space selected that word. Now measured by drag distance
against `startDragDistance()`, which keeps deliberately dragging across a single
word working — comparing the two ends would not have.

**The paper floats.** Page gap 12px to 22px, a margin at each side so all four
edges of a page are always visible, and each page carries a hairline edge and a
three-step shadow. Both colours come from palette roles, so they follow the
reader's theme and night mode with no second set of colour rules.

**A strip of page previews**, about a fifth of the row, `Ctrl+B`. Built the way
`PageView` is built rather than as a list of icons: a thousand-page document
renders a dozen thumbnails, not a thousand. It steps aside for presentation and
for a comparison.

**Zoom shows its value**, with a logarithmic slider whose middle is life size
rather than five times it. The readout shows the true zoom even where a fit mode
legitimately goes below the slider's floor.

**A status bar** — full path, creation and modification dates centred, octal and
symbolic permissions. `statx` rather than `stat`, because `stat` has no creation
time to give: `st_ctime` is the inode's *change* time and is routinely mistaken
for this one. Where the filesystem records no birth time it says so rather than
showing a different fact under this label.

**Two pointer modes**, `Ctrl+H`: select text, or grab the page and move it in
any direction. A middle-drag pans in either mode.

**An About page in the family layout** — the mark, the wordmark, a bilingual
subtitle, version, release date, maker, source, the licence in full, and the
sign-off. Addresses selectable, never clickable. The application icon existed
but was only findable after installation and `setWindowIcon` was never called at
all, so it is embedded as a Qt resource now and the installed icon theme is
preferred when there is one.

> [!NOTE]
> **Amended at Z12.** Three defects here were found by the author looking at the
> application, not by any test, and two of them had been shipping unnoticed
> since the milestone that introduced them.
>
> `GlyphButton::paintEvent` washed the button for `State_Sunken` and
> `State_MouseOver` and never looked at `State_On`, so a *checked* toggle
> painted exactly like an unchecked one. The previews toggle had therefore been
> invisible from the moment it was added.
>
> The status bar's permissions were set bold and rendered plain: this system
> resolves `monospace` to Andale Mono, which ships no bold face, and Qt did not
> synthesise one — weight 700 produced glyphs identical to weight 400, to the
> same advance width. The family is now chosen by whether it can do what is
> asked of it.
>
> The dates were centred with `addPermanentWidget`, which is laid out after the
> stretch is shared, so the space either side was never equal and they sat 74px
> left of centre. The test that was supposed to catch this used a tolerance of
> one twelfth of the window, and passed.

## 11. Release procedure

- **Z1 through Z9:** commit locally, tag locally. No push, no build, no
  release.
- **Z10:** commit, push, tag, push the tag, let Actions build and publish.

All commits come from the `sudo-megas` account. No AI attribution appears in
any commit message, code comment, or About text. From Z10 this is enforced by
an audit script on every push rather than by intention alone.

The README remains deferred, as it was in v1.0, and is not part of any
milestone here. When it is written it follows the family convention and must
carry:

```bash
xdg-mime default mergen.desktop application/pdf
```

---

## 12. Working agreement — unattended builds

The build runs unattended. The user is not watching and will not be available
to answer mid-run. When something comes up, take the recommended action and
keep going; do not stop and wait.

**Environment that can be assumed:**

- `gh` is authenticated as `sudo-megas` with repo scope.
- Arch Linux, Wayland, Niri. `base-devel`, `cmake`, `ninja`, `git`, `qt6-base`,
  `poppler-qt6`, `qpdf` and a Nerd Font are present or installable with
  `yay -S`.
- Clean-chroot building via `extra-x86_64-build` is available.
- Package signing is the user's own step, outside this run.

**How to decide when unattended:**

- Anything already ruled in this document is settled. Re-read the relevant
  section rather than re-deciding it.
- A choice this document does not cover: pick the option most consistent with
  what it does say — fewest dependencies, least state, no dialogs, no network —
  then record the choice in the section it belongs to as part of the same
  commit. This document stays the source of truth.
- A choice that would breach §5: do not take it. Stop at that milestone, leave
  the working tree buildable, and leave a note in the commit body explaining
  what was blocked and why.
- A dependency that seems necessary but is not in §3: prefer writing the code
  by hand. Add one only if hand-writing it would be unreasonable, and say so in
  §3 with the reasoning, in the same voice as the existing entries.
- Build failures, API mismatches, and poppler signature differences: fix them
  and carry on. Do not report back for permission.
- If a milestone's verification cannot pass, fix it before tagging. Never tag a
  milestone whose verification failed.

**What never happens unattended, regardless:**

- No push and no release before Z10, per §11.
- No AI attribution in any commit message, code comment, or About text.
- No commits from any account other than `sudo-megas`.
- Nothing on the §5 list gets built, however convenient it seems at the time.

---

## 13. Open rulings

Decisions deliberately not made yet, recorded so they are not made by accident.

**Annotation creation.** v2.0 renders annotations; it does not create them.
The ruling was deferred until Z6 ships and there is real code in hand, because
the choice depends on how the rendering path actually turns out. The three
candidates, with what each costs:

- *Sidecar file.* The PDF is never modified and never at risk, and it works on
  documents the reader cannot write — including the root-owned files the
  `pkexec` path opens. But the highlights are invisible in every other viewer
  and do not travel with the file, and it adds a third state file.
- *Written into the PDF.* Portable, visible everywhere, what readers expect —
  and it makes MERGEN a writer of the documents it opens, needing save, dirty
  and undo semantics, and impossible on exactly the read-only files the
  elevation path exists to serve.
- *Sidecar, with an explicit export.* Both, and the most work.

Nothing is built toward any of them before the ruling.

**Where an elevated document may go.** A file the reader could only open by
authenticating to `pkexec` is in memory as plaintext that their own account has
no right to. Three ways out of the process were considered at Z11, and they are
not ruled the same way:

- *Redaction* — refused. It writes a new file derived from the original, and
  the refusal predates the withdrawal below.
- *Printing to a file* — refused, at Z11. Printing to a *printer* is allowed:
  the reader authenticated, and the bytes go to paper. Printing to a file leaves
  an unprivileged copy on disk that outlives the authorisation entirely, which
  is the same objection as redaction with none of the difficulty.
- *The clipboard* — **allowed**, deliberately. This is the inconsistent-looking
  one, so the reasoning is written down rather than left to be rediscovered. A
  selection is text the reader has already read on screen; the clipboard is
  volatile, holds what fits in a selection rather than the document, and is the
  only way to act on what they authenticated to see. Refusing it would make the
  elevation path nearly useless while stopping nobody who can retype a line.
  The guarantee MERGEN makes is that it will not *write a file*; it is not that
  an authenticated reader cannot use what they were shown.

If that trade is ever revisited, the thing to change is the clipboard, not the
other two.

**An opt-out for motion.** Motion is unconditional because Qt offers nothing on
this platform to condition it on — see the amendment in §9. A reader who wants
none has no way to say so. The options are an environment variable read at
startup, a keybinding, or waiting for Qt to grow a real hint. All three are
rulings, not choices to make mid-programme, so none was taken.

**Redaction, and whether it returns.** Withdrawn at Z11 rather than shipped.
The audit found four independent routes by which it reported success while the
selected text remained extractable, confirmed by two reviewers working
separately on two different PDF engines, and a fifth by which a symlinked
destination destroyed the source. The last is fixed; the first four are not,
because they are one defect: the filter approximates text layout instead of
modelling it. It tests a show operator's origin rather than its painted extent,
never advances the text matrix across glyphs, and drops the line advance carried
by `'` and `"` — which slides the following line under the black bar, still
readable, manufacturing exactly the false guarantee §3 cites as the reason for
taking on `qpdf`.

Returning it means tracking the full text state (`Tf`, `Tc`, `Tw`, `Tz`, `Ts`,
`TL`), advancing per glyph by font width, testing the painted extent, and
decomposing `'`/`"` into their advance and their show so only the show is
suppressed — plus converting display space to user space with the CropBox
origin. That is a milestone, not a patch, and it should be verified against
documents from real producers rather than hand-built ones: on the valgrind
manual, the speex manual and this project's own `test.pdf`, the current
implementation simply declines.

Until then the honest position is that MERGEN does not redact.

**Hiding annotations.** MERGEN draws the markup a document carries and offers
no way to turn it off. A reader who wants the clean page underneath someone
else's highlighting has a real want, and every large viewer has a switch for
it. It is not built because nothing ruled it, and §12 does not permit inventing
features mid-programme — recorded here so the question is asked rather than
quietly answered by its absence.

**Night-mode persistence.** Night mode is session-scoped, because §5 bans
settings and §8 keeps state to what the reader explicitly made. A reader who
works at night will toggle it every launch, which is friction the ban did not
intend to create but does. Left as the conservative reading until ruled
otherwise; the counter-argument is real and is recorded here rather than
quietly resolved in code.

**Search-highlight derivation.** The hue rotation that derives the search
colour works, but rotating in HSL does not guarantee the result stays legible
against every possible palette. Rotating in a perceptually uniform space with
an explicit contrast floor would. Not scheduled; it is a robustness
improvement to working code, not a fix for a known failure.

**Focus/typewriter reading mode** and a **vim-style modal command bar** were
raised and are not scheduled. The modal bar collides directly with §7's "no
vim-style bindings anywhere," which is a standing position and not something
to erode by increment; if it is ever wanted, it needs a ruling on §7 first, not
a milestone.
