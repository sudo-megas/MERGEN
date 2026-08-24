# MERGEN — Decisions to Discuss (Pass 1)

Uncommitted working document, prepared from `research1.md` for an
interactive discussion — not a proposal, not a to-do list, not anything
pre-decided. Each item below is a real question with a tradeoff, not a
recommendation dressed up as one. Nothing here overrides `build/docs/MX.md`
unless and until the maintainer rules on it, the same way everything in
MX.md itself got there.

18 questions, grouped into 8 areas. No particular order is implied — happy
to go straight to whichever one is most interesting, skip any, or take them
in sequence. Where two or more research agents independently landed on the
same idea from different angles, that's noted — it's a signal worth
weighing, not a vote that decides anything.

**Audits, separately:** both Opus audit agents (A1: UI/concurrency/rendering
logic, A2: security/build/packaging) failed with a transient
`API Error: 529 Overloaded` and were dropped rather than retried, per
standing instruction. Nothing below depends on them. Say the word whenever
you want them relaunched — same scope as originally planned — and
`audit1.md` gets created once they actually report.

---

## A. Motion & interaction polish

**Q1.** Add short (~150–250ms, eased) animation to the three places MERGEN
currently snaps instantly — zoom-level changes, page-jumps, and the
scroll-to-search-hit jump (`pageview.cpp:499/501`, a bare `setValue()`
today) — plus turn on `QScroller` for trackpad-momentum scrolling?
Near-zero risk: `QVariantAnimation`/`QPropertyAnimation`/`QScroller` are
all built into Qt already, no new dependency, and it should respect the OS
reduce-motion setting. **Two independent agents (R1, R4) each flagged this
as the single highest-leverage, lowest-risk change on the table.**
*(R1 rec #1, R4 punch-list #1)*

---

## B. Toolbar visual authorship & accessibility

**Q2.** Rework the Nerd-Font glyph toolbar buttons from raw
`QAction::text()` into real multi-state `QIcon`s — rasterize each glyph via
`QPainter` into a cached, palette-colored pixmap, keep a separate
human-readable string on `QAction::setText()`? This is the "QtAwesome
technique." It fixes three real, currently-latent bugs in one move: icons
silently vanish if `toolButtonStyle` is ever `IconOnly` (since the glyph
*is* the text today), screen readers announce an unlabeled Private-Use-Area
codepoint instead of a name, and disabled/hover states aren't generated
correctly. Same font, same look, zero new dependencies. *(R2 rec #3)*

**Q3.** Independent of Q2 — audit every toolbar `QAction` for a real
`setAccessibleName()`/`setToolTip()`/`setStatusTip()` distinct from the
glyph itself? Cheapest, highest-value accessibility fix available, doesn't
require the icon rework first. **Three agents (R1, R2, R4) each flagged
some version of this independently.** *(R1 rec #2, R2 rec #4, R4
punch-list #6)*

**Q4.** Author explicit hover/press visual feedback for toolbar buttons?
Right now it's pure ambient `QStyle` via `setAutoRaise(true)` — meaning the
toolbar looks different, sometimes barely-there, depending on whether the
user's Qt style is Breeze, Fusion, Kvantum, or Adwaita-Qt. The sanctioned
way to do this without violating "no theming code" is a small
custom-painted `QToolButton` or a narrow `QProxyStyle` (plain QSS can't
read live `QPalette` roles) drawing a palette-derived highlight on
hover/press. *(R2 rec #5, R4 punch-list #2)*

**Q5.** Group the 8 toolbar actions into visually separated clusters —
Open · Zoom/Fit · Rotate/Search/Print — via `addSeparator()` or slim
spacers? Cheap, matches GNOME's "only a small number of controls visible"
toolbar rule, and gets ahead of clutter before the action list potentially
grows from anything in section F. *(R1 rec #4, R4 punch-list #4)*

**Q6.** A batch of smaller, low-risk toolbar items with no real tradeoff to
weigh — more a checklist than a decision, listed together so they don't
each need separate discussion: glyph optical-alignment/weight-consistency
pass; consistent disabled-state treatment (Zoom-in at the 1000% cap, Print
with nothing open); cursor-shape discipline audit in the page view;
app-icon legibility check at 16–24px switcher size; tooltip completeness
pass (the *only* shortcut-discovery mechanism, since there's no menu bar).
Worth just doing, or anything here you'd rather skip? *(R4 punch-list #3,
#5, #6, #7, #8)*

---

## C. Empty state & icon motif

**Q7.** Design a real empty state for "no document open" — a full-window
drag-and-drop target, with a faint watermark echoing the app icon's
brushed-metal/curled-page-corner motif behind the centered text? Right now
it's plain centered text on `QPalette::Base`. **Two agents (R1, R4)
independently flagged this as the biggest structural empty-state gap** and
the cheapest place to extend the icon's real design investment into the
app itself rather than stopping at the `.desktop` icon. *(R1 rec #3, R4
punch-list #10)*

---

## D. Search-highlight color derivation

**Q8.** Upgrade the search-hit hue-rotation from raw HSL to a
perceptually-uniform space (Oklch/HCT) with an explicit contrast check, so
the derived highlight color is guaranteed legible against the page
background regardless of the user's accent color? Self-contained change to
one function (`PageView::searchColor`); today's HSL rotation already works
in most cases, this is a robustness upgrade rather than a fix for a known
failure. *(R1 rec #5)*

---

## E. Maintenance / risk items

**Q9.** Worth a one-time regression check pinning the exact CaskaydiaCove
Nerd Font version MERGEN's glyph codepoints depend on? Qt6 no longer
silently falls back to another installed font for a missing glyph the way
Qt5 did (`QTBUG-110502`) — a future font update that renumbers or drops a
codepoint MERGEN uses would produce a visible tofu box with no graceful
degrade. Pure risk-mitigation, no feature value, cheap to do now. *(R2 rec
#6)*

**Q10.** Two small inconsistencies R5 turned up against the rest of the
family: MERGEN's `PKGBUILD` pins `license=('GPL-3.0-only')` while TRITIUM's
README states `GPL-3.0-or-later`, and MERGEN keeps `PKGBUILD` at repo root
where TRITIUM/SAAT/PARACHRON nest theirs under `packaging/`. Worth
reconciling to match the family pattern, or are these deliberate
MERGEN-specific choices worth a one-line note in MX.md instead? *(R5)*

---

## F. Feature roadmap

**Q11.** R3's research converged hard on two features as the clear
value-per-effort leaders, and its own answer to "if MERGEN could only do
one or two things next": **(a) annotations/highlighting** — medium effort,
the single most-requested feature across every competitor surveyed
(zathura's oldest open issue, the reason people pick sioyek over zathura,
the headline of every Xodo/PDF Expert review) — and it's the *one* item
`MX.md` already leaves open for later, not something that needs a ruling
change. **(b) Page color inversion / night-reading mode** — low effort,
very high demand (whole dedicated apps exist for just this), and
explicitly **not** the same thing as the banned "theme switcher" since
it's a content-render toggle on the decoded page image, not app-chrome
theming. Pursue either, both, or neither as a near-term follow-up? *(R3 —
full ranked 13-feature table in `research1.md`)*

**Q12.** Two "reinterpretation, not violation" ideas came up, both aimed
at sidebar-like navigational power without an actual persistent sidebar: a
**transient, shortcut-triggered outline/TOC popup** (not docked), and a
**Ctrl+K-style command overlay** for search + page-jump + action
invocation. Both target the DO-NOT list's evident *intent* (no persistent
chrome) rather than testing its letter. Worth exploring, or does that read
as a loophole not worth taking regardless of the letter/intent
distinction? This one seems more like a philosophy question than a
feature question. *(R3 — TOC popup; R1 rec #7 — command overlay)*

**Q13.** Beyond the ranked list, R3 surfaced longer-shot ideas with no
direct competitor equivalent: sioyek-style bidirectional "portals" between
two locations/documents, a visual PDF diff/compare mode (cheap to build on
poppler's existing rasterization), true content-level redaction (via qpdf,
actually strips content rather than papering over it), a vim-style modal
command bar for navigation only, and — flagged as especially on-brand for
MERGEN's actual Arch+Wayland+tiling-WM audience — a **native IPC/scripting
surface** (Unix socket or D-Bus) so window-manager keybindings could drive
MERGEN directly. Any worth a closer look, or better to focus on Q11's
higher-certainty items for now? *(R3 ambitious list)*

**Q14.** Should any currently-banned item (tabs, bookmarks, thumbnails
panel, any sidebar) actually be reconsidered given the "nothing to lose"
framing — or does the research mostly confirm the bans are already
correct? (R3's own note on tabs specifically: "the one that doesn't make
you deal with tabs" is arguably part of MERGEN's identity, not a gap.)
This is the closest thing to "should MX.md's DO-NOT list itself be
amended," so worth asking directly rather than assuming either way. *(R3,
general synthesis)*

---

## G. Theming — mostly validation, one real check-in

**Q15.** R2's research is a strong, independently-sourced validation of
two choices MERGEN already made: **(1)** zero custom QSS + live
`QPalette` as the only color source, and **(2)** no in-app dark-mode/theme
logic — Qt 6.5+ already resolves system color-scheme live, and tools like
`qt6ct`/Kvantum solve "I want a different look" better at the system level
than an app ever could (a user's dark-mode-doesn't-work complaint on niri
is a `qt6ct`/portal config issue, not a MERGEN bug). Given the research,
any appetite to revisit either — or does this just confirm "keep doing
exactly what MX.md already says," worth stating explicitly so it's a
closed question going forward rather than something that quietly gets
re-litigated later? *(R2 rec #1, #2, #7)*

---

## H. Family traditions / process & docs

**Q16.** R5 confirmed the whole "Megas family" (SAAT, JADEITE, PARACHRON,
INDIUM, RESONANCE, TRITIUM, and MERGEN as app 7) shares a README template:
banner, shields.io badges (version/release date/licence/Arch package
size), bilingual subtitle where applicable, numbered ALL-CAPS sections,
and TRITIUM's sign-off line "Built with Reason and Passion." `MX.md`
already says MERGEN's README is deliberately deferred to an unscheduled
later session — does that stay deferred, or is now (post-v1.0, mid-
brainstorm) a reasonable time to write it against the family template?
*(R5)*

**Q17.** Two family conventions MERGEN doesn't currently have: **(a)** a
dated-amendment / deviations-ledger discipline in the spec doc (TRITIUM's
inline "Amendment — DATE" blocks; INDIUM's numbered `DEVIATIONS` section
plus a separate `issues.md`), and **(b)** CI scripts that actively enforce
constitution rules rather than leaving them as prose (JADEITE's
`audit-egress`/`audit-strings`/`audit-colours`/`audit-locale`; INDIUM's
`check-deps.sh` gating dependency rules on every push) — MERGEN's own
zero-AI-attribution and dependency-minimalism rules currently live only as
written policy. Worth adopting either, or is MERGEN's smaller scope a good
reason to deliberately stay lighter than its siblings here? *(R5)*

**Q18.** R5 flagged that `megas-xlr` (the author's own signed pacman repo)
is the stated distribution channel for TRITIUM and INDIUM, both explicitly
"No AUR." Is that also the intended channel for MERGEN, and if so, is it
actually wired up — or does M7's release workflow need a follow-up step to
publish there? *(R5)*

---

*Prepared from `research1.md`. Waiting for "ask your questions now" before
starting the actual discussion.*
