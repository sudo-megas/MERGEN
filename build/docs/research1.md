# MERGEN — Research Findings, Pass 1

> [!NOTE]
> Tool and vendor names in this file are written as `<assistant>` and
> `<vendor>`. They appear here only as examples of the shape the
> attribution audit looks for, never as credit — and writing them out in
> full would trip the very check being described. The substitution is
> mechanical and changes no finding.

Uncommitted working document. Generated 2026-08-24, post-v1.0.0, as an open
brainstorming pass — nothing here is scoped/approved work, and nothing here
overrides `build/docs/MX.md` unless and until the maintainer says so.

Five Sonnet agents, run in parallel, ~15 minute soft budget each, full web
access (WebSearch/WebFetch), no constraint against the current DO-NOT list.
Two Opus audit agents (correctness/concurrency and security/build) were also
dispatched but both failed with `API Error: 529 Overloaded` and were dropped
per standing instruction rather than retried — see `audit1.md` (not yet
created; will exist once the audits are relaunched and actually report).

## Contents

1. [R1 — Modern desktop app design](#r1--modern-desktop-app-design)
2. [R2 — Theming/design in Qt6 C++ Widgets](#r2--themingdesign-in-qt6-c-widgets)
3. [R3 — PDF feature comparison](#r3--pdf-feature-comparison)
4. [R4 — Eye-candy / professional polish](#r4--eye-candy--professional-polish)
5. [R5 — sudo-megas app family traditions](#r5--sudo-megas-app-family-traditions)

---

## R1 — Modern desktop app design

*(Note from the agent on process: partway through this research a plan-mode
restriction appeared in-session. It required no action — every tool call
made here was read-only web research, and this report itself is the
requested deliverable, not a file, so nothing needed to change.)*

### Part 1 — What Apple, Fluent 2, GNOME, KDE, and Material 3 actually agree on

**Spacing & typography.** Every system uses one small base unit multiplied
consistently rather than arbitrary pixel values: Apple's 8pt grid with 4pt
subdivisions and 44×44pt minimum tap target ([Apple HIG](https://developer.apple.com/design/human-interface-guidelines/typography));
Material 3's 4dp grid; GNOME's long-standing "multiples of a small base
unit" spacing guidance. KDE's **Kirigami Units** is the outlier worth noting
closely: `gridUnit` is derived from `QFontMetricsF::height()` — not a fixed
pixel value — with `smallSpacing = gridUnit/4` and `largeSpacing = gridUnit`
([Kirigami Units docs](https://develop.kde.org/docs/getting-started/kirigami/introduction-listviews/);
[invent.kde.org MR #479](https://invent.kde.org/frameworks/kirigami/-/merge_requests/479)).
Typography-wise, Apple pushed toward bolder, left-aligned system-font text
in 2025 ([WWDC25 "Get to know the new design system"](https://developer.apple.com/videos/play/wwdc2025/356/)).

**Elevation & depth.** Fluent 2 builds this from *Light + Depth*: shadows
from a consistent overhead light source, plus two materials — **Mica**
(opaque, tinted by desktop wallpaper, for persistent surfaces) and
**Acrylic** (translucent frosted glass, reserved for *transient,
light-dismiss* surfaces like flyouts/menus only) ([Fluent 2 Material](https://fluent2.microsoft.design/material);
[Microsoft Learn: Acrylic material](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)).
Material 3 replaced pure shadow-elevation with **tonal elevation** — a
surface gets a semi-transparent overlay tinted with the primary color as it
"rises," contrast-guaranteed by construction ([m3.material.io/styles/elevation](https://m3.material.io/styles/elevation)).
Apple's 2025 "Liquid Glass" is the same idea taken further. GNOME/KDE stay
comparatively flat: depth comes from toolbar/content tone separation plus,
only on compositors that support it (KWin on Plasma), actual
compositor-level blur — not app-drawn shadows.

**Motion.** Fluent 2 specifies purposeful, **150–250ms** transitions that
"reinforce spatial paradigms that support way-finding" ([fluent2.microsoft.design/motion](https://fluent2.microsoft.design/motion)).
Material 3's 2025 "Expressive" refresh moved to a **physics/spring-based
motion system**, validated against 46 user studies and ~18,000 participants,
claiming up to 4× faster element recognition ([Material 3 Expressive](https://supercharge.design/blog/material-3-expressive)).
150–250ms is a near-universal convergence point right now.

**Color.** All five now default to **deriving chrome color from a live
system/user theme** rather than a fixed brand palette — Windows accent
color, Material You's wallpaper-derived dynamic color, Adwaita's system
accent color, Plasma's accent color. Material 3's HCT color space
*guarantees* WCAG-safe contrast by construction rather than hoping a color
transform stays legible.

**Iconography.** Apple's SF Symbols (glyph-based, auto-matched
weight/scale to surrounding text) is the closest industry-standard analog
to MERGEN's Nerd-Font-glyphs-as-icons approach — it validates the technique
in principle. GNOME uses single-color "symbolic" SVGs that recolor with the
foreground/accent color. Real caveat, from an accessibility case study: raw
icon-font glyphs, when the glyph character doubles as accessible text, get
announced by screen readers as "unknown character" or the literal codepoint
— the glyph needs a **separate** accessible label ([24a11y.com case study](https://www.24a11y.com/2017/svg-icon-fonts-accessibility-case-study/);
[Carnegie Museums accessibility guide](http://web-accessibility.carnegiemuseums.org/code/svg/)).

**Empty & loading states.** GNOME's `AdwStatusPage` pattern and Apple's HIG
agree: an empty state is icon/illustration + short headline + (if
actionable) one clear call-to-action — never a blank canvas. Loading-state
consensus: skeleton screens for structured content, spinners for short
indeterminate waits ([onething.design](https://www.onething.design/post/skeleton-screens-vs-loading-spinners);
[Carbon Design System](https://carbondesignsystem.com/patterns/loading-pattern/)).

**Focus & hover.** WCAG 2.2 now requires a focus indicator with a
≥2px-equivalent perimeter and ≥3:1 contrast, visible in both focused-only
and focused+hovered states ([72technologies.com](https://www.72technologies.com/blog/focus-rings-accessibility-design-2026);
[Deque](https://www.deque.com/blog/give-site-focus-tips-designing-usable-focus-indicators/)).

### Part 2 — Narrowed to MERGEN: decoration-less, single-toolbar, Qt Widgets, Wayland-tiling

**Toolbar/action design.** The convergent pattern across Fluent's command
bar+overflow, GNOME's header bar, and Apple's toolbar-unified-with-titlebar:
keep frequent actions always-visible as icon+tooltip, push infrequent ones
behind one overflow affordance. GNOME states this explicitly: **"header
bars should only contain a small number of controls"** ([GNOME Header Bars HIG](https://developer.gnome.org/hig/patterns/containers/header-bars.html)).
Worth stating plainly: MERGEN's "one toolbar, no separate menu bar,
compositor draws the titlebar" *is already* the end-state Apple/GNOME/
Windows have been converging toward for five years. MERGEN isn't behind
this trend — it landed there by necessity.

**Motion — scroll physics & zoom.** Qt Widgets ships kinetic scrolling out
of the box via **`QScroller`** (`QScroller::grabGesture` on the viewport) —
momentum + rubber-band overscroll, zero new dependency ([Qt QScroller docs](https://doc.qt.io/qt-5/qscroller.html);
"Flick Charm" pattern for `QAbstractScrollArea`, [ariya.io](https://ariya.io/2011/10/flick-list-with-its-momentum-scrolling-and-deceleration)).
Zoom/fit-mode changes and page-jumps read as more "2025-native" when the
transform is *tweened* (`QVariantAnimation`, ~150–250ms, ease-out) instead
of snapped. Gate any new animation behind the OS's reduce-motion setting.

**Typography.** 2025 consensus for a *utility* native app: use the system
UI font, don't import a brand typeface — validates MERGEN's current setup
exactly (system font for UI text, Nerd Font reserved purely as an
icon-glyph carrier).

**When should a native widget app imitate the web — and when shouldn't it?**
- **Borrow *interaction expectations* the web popularized, once they're
  universal native affordances in their own right:** command-palette/
  quick-open overlays (native via Spotlight/GNOME Overview/KRunner now),
  toast-style transient confirmations instead of modal dialogs (`AdwToast`
  in libadwaita), animated empty states with a call-to-action.
- **Don't borrow *rendering tropes* that only exist because a browser's
  compositor makes them cheap:** drop-shadowed cards on tinted backgrounds,
  hamburger menus, fully custom-drawn (non-`QStyle`) buttons/scrollbars,
  hover-triggered mega-menus. Native toolkits give accessibility and
  platform-consistency "for free" the moment controls are drawn through
  `QStyle`/`QPalette` instead of overridden — MERGEN's zero-QSS, live-palette
  approach is already on the correct side of this line.
- Rule of thumb: if it's about *what happens when you interact with
  something*, it generally transfers to Qt Widgets; if it's about *how a
  static surface is rendered*, it generally doesn't.

### Part 3 — Curated real-world opinions

- **Lobsters, ["<assistant> is an Electron App because we've lost native"](https://lobste.rs/s/r8kjli)**
  — best single discussion found. Conclusion: platform vendors stopped
  holding still, which is what made cross-platform native infeasible.
  MERGEN sidesteps that by targeting exactly one OS and one compositor.
- **HN threads on "Linux apps look dated/ugly"** — complaints center on
  font rendering/hinting and icons "drawn by a programmer, not a designer,"
  not the toolkit itself. MERGEN already sidesteps the icon complaint via
  its hand-illustrated app icon.
- **Nate Graham / [pointieststick.com](https://pointieststick.com/2024/06/09/new-human-interface-guidelines/),
  KDE's 2024 HIG rewrite** ([Phoronix](https://www.phoronix.com/news/KDE-New-HIG-2024))
  — rewritten so essentially all of it is actionable rather than aspirational.
- **GNOME Papers replacing Evince (GNOME 49, 2025)** ([Phoronix](https://www.phoronix.com/news/GNOME-Papers-Approved-49);
  [OMG Ubuntu](https://www.omgubuntu.co.uk/2025/07/papers-pdf-viewer-gnome-49))
  — the most directly comparable real project: a minimalist PDF viewer
  rewrite, GTK4/libadwaita-native. Worth looking at directly as the nearest
  existing "modern PDF viewer done natively" reference point.
- **Zathura / Sioyek** — MERGEN's closest philosophical relatives. Compared
  to Okular once a user wants *any* visible affordances — useful data point
  that MERGEN's one-toolbar approach sits at a sensible midpoint.
- **elementary OS / Pantheon HIG** — the Linux world's most disciplined
  small-team HIG, built on three self-imposed rules (concision, accessible
  configuration, minimal documentation).

### Part 4 — Component kits/templates that genuinely apply to Qt Widgets

- **[QtAwesome](https://github.com/gamecreature/QtAwesome)** (C++) /
  [spyder-ide/qtawesome](https://github.com/spyder-ide/qtawesome) (Python) —
  formalizes exactly what MERGEN already does by hand: rendering an icon
  font into `QIcon`-compatible pixmaps for `QAction`/`QToolButton`, with
  per-state (hover/disabled/active) color via `QPainter` rather than raw
  `QAction::setText(glyph)`.
- **[QFluentWidgets / PyQt-Fluent-Widgets](https://github.com/zhiyiYo/PyQt-Fluent-Widgets)**
  — full Fluent 2 component set on real `QWidget` subclasses. Too heavy to
  adopt wholesale, but proves Fluent's depth/material system is
  implementable in plain Qt Widgets.
- **[BreezeStyleSheets](https://github.com/Alexhuszagh/BreezeStyleSheets)** —
  KDE Breeze reconstructed as portable QSS. Architecturally opposed to
  MERGEN's zero-QSS stance — "worth knowing exists," not "worth adopting."
- **[qt-material](https://github.com/UN-GCPDS/qt-material)** —
  Material-flavored QSS; same caveat.
- **Kirigami's Units system** — QtQuick, doesn't transfer as code, but its
  font-metric-derived `gridUnit`/`smallSpacing`/`largeSpacing` is trivially
  reimplementable against `QFontMetrics` in plain Widgets — arguably the
  single most portable concrete idea in this pass: ties spacing to the
  user's actual font size/DPI rather than a hardcoded constant.
- **`QScroller`** — built into Qt Widgets, solves kinetic-scroll physics
  with zero new dependencies.

### Top recommendations (R1), ranked

1. **Tween zoom-level and page-jump transitions (~150–250ms ease-out via
   `QVariantAnimation`), and turn on `QScroller` for trackpad-momentum
   scrolling.** Highest-leverage, lowest-risk change available. Gate behind
   OS reduce-motion setting.
2. **Audit every Nerd-Font-glyph `QAction` for a real accessible
   name/tooltip distinct from the glyph character.** Screen readers may
   currently be announcing raw glyph codepoints as "unknown character."
3. **Design an actual empty state for "no document open,"** doubling as a
   full-window drag-and-drop target — the biggest structural gap versus
   every HIG surveyed. Cheapest place to extend the hand-illustrated app
   icon's brushed-metal/curled-page-corner motif into the app itself.
4. **Codify an "always-visible vs. overflow" toolbar split now,** before
   the action list grows — GNOME's "only a small number of controls" rule
   as an actual constraint.
5. **Upgrade the hue-rotated search-highlight derivation** to a
   perceptually-uniform, contrast-checked transform (Oklch/HCT rather than
   raw HSL, verify ≥3:1 contrast post-rotation).
6. **Lean on palette Button/Base role separation plus a single hairline
   separator for depth — never drop-shadow/card treatments.** Guardrail
   against sliding toward "web app in a native shell."
7. **(Ambitious, against the current DO-NOT list) A single Ctrl+K-style
   command overlay** for search + page-jump + action-invocation, instead of
   a permanent sidebar. Gets most of what a sidebar would offer without
   breaking single-window minimalism.
8. **(Speculative — verify compositor support first) Toolbar
   vibrancy/blur.** niri is young and minimal; unconfirmed protocol
   support. Check before spending any time here.

---

## R2 — Theming/design in Qt6 C++ Widgets

### 1. QSS (Qt Style Sheets): what's solid vs. what fights the toolkit

**The authoritative case against broad QSS** comes from KDAB, via
["Say No to Qt Style Sheets"](https://www.kdab.com/say-no-to-qt-style-sheets/).
Concrete claims: once a stylesheet is set, `setFont()`/`setPalette()` calls
silently **stop working**, even with no color rules in the sheet; styled
widgets **don't adapt** to system color changes at runtime; **stylesheets
don't mix with QProxyStyle/QStyle subclasses** — pick one lane, not both;
`setStyleSheet()` triggers a full re-parse and every reparenting event
recomputes the whole cascade — a real perf cost. Qt's own docs concede
stylesheets are unsupported on custom `QStyle` subclasses, have no effect
on undocked `QDockWidget`s or the Mac global menu bar, and warn "you will
be responsible for all the graphical details."

**Cautionary case study: OBS Studio.** Ships full custom QSS themes (Yami,
Rachni — [OBS theme docs](https://obsproject.com/kb/themes-guide),
[theme system wiki](https://github.com/obsproject/obs-studio/wiki/OBS-Studio-Theme-System)).
Their QSS grew unmaintainable enough that OBS built its own **OVT format
on top of QSS** just to get CSS-like inheritance QSS lacks. "A little QSS"
has real gravitational pull toward "a lot of QSS, then bespoke tooling."

**Where QSS is genuinely fine:** small, targeted, structural rules that
don't touch color — `QToolBar { spacing: Npx; }`, a border removal, one
widget's padding. The critique is about *systemic* theming, not a
two-line tweak.

### 2. QProxyStyle / custom QStyle — the "correct" extension point

From [Olivier Cléro's "Writing a custom QStyle"](https://www.olivierclero.com/code/custom-qstyle/)
and Qt's own examples: subclass **`QCommonStyle`** (not raw `QStyle`) to
inherit sane generic behavior and override only what's needed. The three
methods that matter: `drawControl()` (composite widgets), `drawPrimitive()`
(basic elements), `pixelMetric()` (sizes/margins/padding). Caveat: *"there
is no guarantee that `QStyle::proxy()` will be called by the QStyle you
override"* — some third-party/native styles ignore the proxy chain, so
`QProxyStyle` is reliable only for the hooks it's designed around
(palette/hint/pixelMetric overrides, hover behavior). Qt's own
**NorwegianWoodStyle example** is the canonical minimal pattern: override
`polish()` to set `Qt::WA_Hover` so Qt emits paint events on mouse
enter/leave, then style hover state in `drawControl`.

**Verdict:** QProxyStyle is the toolkit-sanctioned way to make *narrow,
additive* visual decisions (hover feel, focus rings, pixel-metrics) while
delegating everything else — the opposite of QSS's all-or-nothing cascade.

### 3. Ready-made theming kits — survey and verdict

| Library | What it is | Verdict for MERGEN |
|---|---|---|
| [QDarkStyleSheet](https://github.com/ColinDuquesnoy/QDarkStyleSheet) | Most widely-adopted dark/light QSS pack for QtWidgets | Well-regarded, but still full-cascade QSS — same KDAB caveats at scale |
| [qt-material](https://github.com/UN-GCPDS/qt-material) | Material-Design-inspired QSS, runtime theme switching | Reads as "Android/web app on the desktop," not native |
| [Qt-Advanced-Stylesheets](https://github.com/githubuser0xFFFF/Qt-Advanced-Stylesheets) | Another QSS theming framework | Same category — full reskin |
| [qfluentwidgets](https://qfluentwidgets.com/) | Replacement widget library implementing Fluent Design | Swaps the whole widget set — opposite of "native feeling" |
| [Kvantum](https://github.com/tsujan/Kvantum) | SVG-based **theme engine that IS a QStyle plugin**, system/user-installed | Correct division of labor: system-level theming, not app-level. MERGEN staying transparent to QPalette/QStyle is what makes it work under Breeze, Kvantum, Fusion, qt6ct automatically |
| [QtAwesome](https://github.com/gamecreature/QtAwesome) | Renders icon-font glyphs into real, cached, multi-state `QIcon`/`QPixmap` | Directly relevant technique — see §6 |

### 4. How KDE/Breeze achieve a cohesive "native" look

From [Nico Fella's "How platform integration in Qt/KDE apps works"](https://nicolasfella.de/posts/how-platform-integration-works/):
cohesion comes from **layered, independent plugins** — (1) Qt Platform
Theme sets fonts/dialogs/`QPalette`, (2) QStyle (Breeze) paints widgets
from scratch, (3) a separate QQC2 layer for QML. KDE apps use
**`KColorScheme`**, a superset of `QPalette` — and this is the source of a
documented cross-desktop bug: under GNOME, `QGnomePlatform` maps Adwaita
colors into `QPalette` correctly, but code querying `KColorScheme` directly
still gets Breeze's hardcoded defaults, producing **visibly mixed color
sets in one window**. Directly applicable lesson: reading colors from
exactly *one* canonical source (`QPalette`, as MERGEN already does) and
never a competing higher-level API is what keeps a cross-desktop app
coherent. MERGEN is already doing the thing this bug is a cautionary tale
*against*.

### 5. Automatic light/dark adaptation via desktop portal

Qt 6.5 added **`QStyleHints::colorScheme`** plus a `colorSchemeChanged`
signal ([Qt blog](https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5)).
As of 6.5, Qt **"always loads the system palette, based on the user's
preference"** on Linux/GNOME too — `QApplication::palette()` updates itself
live without the app touching D-Bus/the portal. Qt's recommended pattern:
pick a palette-agnostic base style (Fusion named explicitly), query
`colorScheme` directly only for *custom-drawn elements* needing an explicit
decision — exactly MERGEN's existing model, including its hue-rotated
search highlight as the sanctioned exception pattern.

**Niri-specific finding:** the portal path
(`org.freedesktop.appearance.color-scheme` via `xdg-desktop-portal`) needs
a running portal backend, commonly `xdg-desktop-portal-gnome`/`-gtk`. A live
GitHub issue on a niri-based setup ([CachyOS/cachyos-niri-noctalia#4](https://github.com/CachyOS/cachyos-niri-noctalia/issues/4))
documents `xdg-desktop-portal-gnome` **not auto-started in a niri session
by default** — dependent apps silently fall back to light theme, through no
fault of the app. Niri users typically get correct Qt theming via
**`qt6ct` + `QT_QPA_PLATFORMTHEME=qt6ct`**, portal-independent. **Implication:
this is a system/session configuration issue, not something MERGEN's code
should try to route around** — an in-app portal listener would be
redundant at best, strictly worse coverage than what Qt already does free.

### 6. Nerd Font glyph-toolbar pattern — lineage, limits, alternatives

**Known/respected?** Yes, but its home turf is **terminal/TUI and
text-editor tooling** (starship, powerlevel10k, lazygit, yazi, btop,
Neovim/VS Code plugins), not classic desktop GUI apps. A genuine Qt6
example exists ([Nerd Fonts icon browser](https://github.com/BaiduNano/nerd-fonts-icon-browser))
but is a niche utility *for* Nerd Fonts, not evidence of mainstream Qt
Widgets adoption — polished KDE/Qt apps overwhelmingly use real vector
icon themes instead. MERGEN's toolbar reads stylistically closer to "a
terminal/dev tool that happens to have a GUI" — given MERGEN's whole
minimalist premise, arguably a deliberate, coherent identity choice.

**Concrete limits found:**
- **Qt6 glyph-fallback regression** ([QTBUG-110502](https://bugreports.qt.io)):
  Qt6 intentionally stopped falling back to other installed fonts for a
  missing codepoint the way Qt5 did. A missing glyph renders as a tofu box,
  silently, on any font-version mismatch.
- **Font availability** — the whole toolbar depends on one font file at
  the exact version whose codepoints match the icon map.
- **Accessibility gap, real and fixable:** if the glyph is literally the
  `QAction`/button's *text*: (1) if `toolButtonStyle` is ever
  `Qt::ToolButtonIconOnly`, Qt hides the text — the glyph vanishes, since
  it's not actually an icon; (2) screen readers read `QAction::text()` as
  the accessible name — a Private-Use-Area codepoint has no meaningful
  name unless `setAccessibleName()`/`setStatusTip()`/`setToolTip()` is set
  separately. (Corroborated broadly — [24a11y case study](https://www.24a11y.com/2017/svg-icon-fonts-accessibility-case-study/),
  Font Awesome's own accessibility docs.)

**Better/complementary approaches:**
- **`QIcon::fromTheme()`** against freedesktop icon themes — reintroduces
  the external dependency MERGEN deliberately avoids.
- **Bundled SVG via Qt Resource System (`.qrc`)** — compiled-in first-party
  icon set, zero runtime/system dependency (stronger guarantee than a
  system font even being present), crisp at any DPI, full per-state color
  control. Standard technique for self-contained apps (e.g. qBittorrent).
- **QtAwesome-style hybrid** — keep the font as the *asset source*, stop
  treating the glyph as literal button text (see recommendation #3 below).

### 7. Verdict: MERGEN's current approach vs. adding QSS/QProxyStyle

**MERGEN's zero-QSS, live-QPalette, no-icon-theme baseline is not a corner
that was cut — it is the textbook "correct" end state** that Qt's own blog
and KDAB both independently arrive at. Apps start "looking like a skinned
web app" at the point they (a) apply broad QSS reskinning every widget
with flat colors and no native bevel/hover semantics, (b) replace the
widget set outright, or (c) let QSS scope creep from "one tweak" into "a
maintained theme" needing its own tooling (OBS's QSS→OVT evolution). Where
a **QProxyStyle** could add value *without* crossing that line: MERGEN's
glyph-based toolbar buttons are exactly the nonstandard element the
KDAB/Cléro sources say `QProxyStyle` is *for* — `pixelMetric()` overrides
for glyph hit-target sizing, or `polish()` + `Qt::WA_Hover` for a more
intentional hover/pressed feel, all while `baseStyle()` still draws
everything else.

### Top recommendations (R2), ranked

1. **Keep zero QSS and live QPalette as the foundation, full stop.**
   Validated by every authoritative source found. Don't let "brainstorm
   mode" talk you out of the one thing that's already textbook-correct.
2. **Don't build a custom xdg-desktop-portal client for dark-mode
   detection.** Qt 6.5+ already covers this at the toolkit level; on niri
   specifically the portal path is known-unreliable for reasons outside
   app control. A user's "dark mode doesn't work" is a `qt6ct`/portal
   config issue on their system, not something MERGEN's code should
   compensate for.
3. **Upgrade the glyph-toolbar plumbing, not its look.** Stop putting the
   glyph directly into `QAction::text()`/button text. Rasterize each glyph
   via `QPainter::drawText()` into an off-screen pixmap at the palette's
   current color (the QtAwesome technique), wrap as a real multi-state
   `QIcon` (Normal/Disabled/Active), attach via `QAction::setIcon()`, with
   `QAction::setText()` holding an actual human-readable label. Fixes
   three real bugs in one move: `ToolButtonIconOnly` no longer silently
   deletes icons, screen readers get a real accessible name, disabled/hover
   states generate correctly. Zero new dependencies, same font, same
   visual identity.
4. **Audit every toolbar `QAction` for a real
   `setAccessibleName()`/`setToolTip()`/`setStatusTip()` distinct from the
   glyph**, independent of #3 — cheapest, highest-value accessibility fix
   available.
5. **If any hover/pressed/focus polish is wanted, reach for a narrow
   `QProxyStyle`, not QSS.** Override `pixelMetric()`/`polish()`/
   `drawPrimitive()` for the one or two things that need it, delegate
   everything else to `baseStyle()`.
6. **Pin down font-fallback risk (QTBUG-110502) before any CaskaydiaCove NF
   version bump** — a font update that renumbers/drops a glyph MERGEN
   depends on produces a visible tofu box, not a graceful degrade. Worth a
   one-time regression check pinned to the exact shipped font version.
7. **Treat Kvantum/Breeze/qt6ct as the correct system-level answer to "I
   want a different look," not something MERGEN should ever try to be.**
   Strongest argument beyond the existing DO-NOT list: system-wide QStyle
   engines already do it better than an app-embedded one ever could.
8. **(Ambitious/optional) A tiny first-party SVG icon set compiled via
   Qt's resource system**, as a dependency-light fallback path independent
   of the Nerd Font being present at all — composes with recommendation #3
   (both end up as `QIcon`s either way).

---

## R3 — PDF feature comparison

**Method note:** ~20 web searches across Okular, Evince/GNOME Papers,
Sumatra PDF, qpdfview, zathura, sioyek, Xodo, PDF Expert, Foxit, PDF
Studio, Adobe Acrobat Reader, and macOS Preview, plus GitHub/GitLab issue
trackers, HN threads, and community forums. Effort estimates are grounded
in MERGEN's actual stack and existing code paths.

### Ranked value-per-effort: missing features

| # | Feature | Demand signal | Effort | Wow/competitive value | Notes |
|---|---|---|---|---|---|
| 1 | **Page color inversion / night reading mode** | Very high — whole category of dedicated apps/extensions exists solely for this | **Low** | High | Not the same as the banned "theme switcher" (app-chrome theming) — a content-render toggle on the already-decoded `QImage`. Cheapest big win on the list. |
| 2 | **Document/security inspector** (PDF version, fonts, tagged status, permissions, warn if the file embeds JS/forms/attachments) | Moderate, concentrated among power users; malicious-JS-in-PDF is a real, live attack vector | **Very low** | Medium-high | Poppler already exposes this data; can reuse the About-dialog pattern. No mainstream *minimal* viewer surfaces "this PDF can execute code" up front. |
| 3 | **Annotations / highlighting** | **Strongest signal in this entire survey.** Zathura's oldest standing feature request ([GitLab #7](https://git.pwmt.org/pwmt/zathura/-/issues/7)); the reason researchers pick [sioyek over zathura](https://news.ycombinator.com/item?id=40318653); headline feature in every [Xodo](https://www.g2.com/products/xodo/reviews)/PDF Expert review; Okular praised specifically for its annotation tools | **Medium** | Very high | Already the one item MX.md leaves open for later. Existing text-selection + clipboard-copy path is most of the prerequisite plumbing. Single biggest competitive gap to close. |
| 4 | **Presentation/slideshow mode** | Moderate-high — universal in every competitor surveyed | **Low** | Medium | Reuses fullscreen + fit-page + zoom code; mostly UI chrome auto-hide + letterboxing. |
| 5 | **Outline/TOC via a transient popup** (not a docked panel) | Very high as a raw feature | **Low-medium** | High | The ban targets a *persistent sidebar*; a shortcut-triggered overlay list (same idiom as the page-jump/recent-files dropdown) delivers most of the value without violating the letter of the rule. Worth flagging as a reinterpretation, not a violation. |
| 6 | **Dual-page / facing-page layout** | Moderate, recurring ask | **Low-medium** | Medium | Extends existing continuous-scroll + culling renderer with different row math. |
| 7 | **Page extract/reorder/delete/merge** (Preview.app-style) | Solid — macOS Preview's most beloved feature besides markup | **Medium-high** | Medium-high | Scoped to page-level ops only, distinct in kind from banned form-filling/signature items. Needs some visual page picker. |
| 8 | **SyncTeX forward/inverse search** | Narrow but intense — core to [sioyek](https://sioyek-documentation.readthedocs.io/en/latest/index.html)/[zathura](https://dev.to/hleb/zathura-cool-pdf-viewer-with-vim-bindings-66k)'s cult followings | **Medium** | Medium (high within niche) | MERGEN's likely audience (Arch, Wayland, tiling-WM) overlaps heavily with the LaTeX/academic crowd. |
| 9 | **Accessibility / screen-reader support** | Low volume, but real — Orca/AT-SPI is standard on Linux, [most minimal viewers skip it](https://orca.gnome.org/) | **Medium-high** | Medium (high goodwill, near-zero competition) | Qt gives standard-chrome AT-SPI baseline for free; hard part is exposing custom-painted page content via `QAccessibleInterface`. Genuine differentiator. |
| 10 | **Tabs / multi-document** | High in absolute terms (Sumatra, qpdfview, Foxit, Xodo default to tabs) | **High** | Medium | Currently hard-banned, arguably in tension with MERGEN's own identity — "the one that doesn't make you deal with tabs" is itself a positioning. Expensive: per-doc search/zoom/state duplication. |
| 11 | **Bookmarks (named/curated)** | Low-moderate — largely redundant once there's a good TOC + recent-files + page-jump | **Low** (zathura-style quickmarks) **to Medium** (full UI) | Low-medium | Banned outright. Zathura's quickmark model (assign a page to one keypress) gets ~80% of the value for ~10% of a full panel UI, if ever reconsidered. |
| 12 | **Thumbnails panel** | Moderate, largely substitutable | **Medium** | Low-medium | Banned, mostly overlaps with TOC + page-jump. Better captured by a "hover-peek" idea than a literal panel. |
| 13 | **OCR for scanned PDFs** | Real, but market treats it as separate/Pro-tier (Adobe Pro, Xodo Pro, PDF Studio, standalone `OCRmyPDF`), not a base-viewer expectation | **High** | Low-medium for MERGEN's scope | Heavy dependency (Tesseract + language packs), conflicts with minimalism more than most items here. Weakest value/effort ratio on this list; "pair with `OCRmyPDF`" may beat building it in. |

### Ambitious / longer-shot ideas ("nothing to lose" list)

- **Sioyek-style "portals"** — bidirectional spatial links between two
  locations (a figure ↔ the paragraph citing it, or across two documents).
  Nothing else in this survey has this; it's sioyek's signature feature.
- **Hover/hold-to-peek overview** instead of any panel — press-and-hold
  shows a floating preview of a link/citation/TOC target without
  navigating away or opening a permanent sidebar.
- **Visual PDF diff/compare mode** — render two PDFs and pixel/text-diff
  them page by page (see [diff-pdf](https://github.com/vslavik/diff-pdf),
  real HN traction). Rare in *minimal* viewers; cheap with poppler's
  existing rasterization, no banned dependency.
- **True content-level redaction** — strips the underlying content stream
  (via qpdf), not an opaque box laid on top (a well-known trust problem in
  cheap tools — text is still extractable underneath). Fits MERGEN's
  already security-conscious posture.
- **Optional vim-style modal command bar** — a colon-command input for
  navigation only (`:42` to jump, `/term` to search), zathura-style, as an
  alternate input mode rather than a UI panel. Doesn't touch the DO-NOT
  list, signals directly to the keyboard-driven audience MERGEN targets.
- **A native IPC/scripting surface** (Unix socket or D-Bus) — "open page
  N," "search X," "next match" callable from shell/WM keybindings. For an
  Arch+Wayland+tiling-WM audience, arguably *more* on-brand than any GUI
  feature — that crowd scripts everything else already.
- **Focus/typewriter reading mode** — dim everything except the current
  paragraph/column, borrowed from distraction-free editors, never seen in
  a PDF viewer.
- **(Flagged, not recommended) fully local/offline OCR or translation via
  a bundled small on-device model** — technically satisfies "no network
  access" (on-device), but bundling an ML runtime cuts hard against
  MERGEN's minimalism. The one idea here that fights the project's own DNA
  rather than extending it.

### If MERGEN could only do one or two things next

1. **Annotations/highlighting.** Most consistent, most repeated demand
   signal across every competitor and community surveyed. The reason
   people leave zathura, the reason people choose sioyek, the headline of
   every Xodo/PDF Expert review — and it's the one item MX.md already
   permits. Closes MERGEN's biggest real competitive gap; existing
   text-selection code is most of the runway needed.
2. **Page color inversion (night mode).** Cheapest possible large win:
   near-zero architectural risk, no new file-format/persistence concerns,
   and demand strong enough that whole dedicated apps exist for this one
   feature alone. Fast, visible "this feels like a modern app" signal for
   the eye-strain-conscious, dark-terminal crowd that self-selects into an
   Arch+Wayland-only PDF viewer.

Together these cost relatively little architecturally, don't require
reversing any of the harder bans (tabs, sidebars, network), and would
close the two gaps most likely to make a reviewer say "great, but no
highlighting/dark mode is a dealbreaker for me."

**Key sources:** [zathura annotations feature request](https://git.pwmt.org/pwmt/zathura/-/issues/7) ·
[sioyek GitHub](https://github.com/ahrm/sioyek) · [sioyek docs](https://sioyek-documentation.readthedocs.io/en/latest/index.html) ·
[sioyek vs zathura, HN](https://news.ycombinator.com/item?id=40318653) ·
[qpdfview vs Okular](https://appmus.com/vs/qpdfview-vs-okular) ·
[Evince vs Okular, Slant](https://www.slant.co/versus/15695/16712/~evince_vs_kde-okular) ·
[Xodo reviews, G2](https://www.g2.com/products/xodo/reviews) ·
[diff-pdf, HN](https://news.ycombinator.com/item?id=40854319) ·
[diff-pdf GitHub](https://github.com/vslavik/diff-pdf) ·
[Adobe Acrobat bloat complaints, HN](https://news.ycombinator.com/item?id=45598776) ·
[Orca screen reader](https://orca.gnome.org/) ·
[SumatraPDF](https://www.sumatrapdfreader.org/) ·
[Foxit PDF compare](https://www.foxit.com/blog/how-to-compare-pdf-files/).

---

## R4 — Eye-candy / professional polish

### The core finding, stated first

Across every source reviewed, the answer is **not animation, not chrome,
not visual effects**. It's **consistency, restraint, and the absence of
unauthored moments**. On an HN thread asking "what software feels
exceptionally polished," alongside Notion and 1Password, people named
**BBEdit** ("been using this since System 7. Still doesn't suck"), **dwm
and dmenu** ("top notch polish, true minimalism"), and **Ghostty**
("exceptionally good at what it was made for") — all chrome-light,
single-purpose, non-web, barely-animated tools. Polish reads as
*reliability + deliberateness*, not decoration. MERGEN's minimalism is not
a handicap to work around — it's already the substrate premium apps are
trying to fake.

That said, "restraint" ≠ "unfinished." The gap between "restrained on
purpose" and "merely bare" is almost always: **every state has been
authored, nothing is left to chance/default, and the few motions that
exist are physically plausible.**

### What the sources actually say

1. **Details aren't decoration, they're the design.** Eames: "the details
   are not the details, they make the design" (37signals' *Details are the
   design*). Corollary: address details *strategically* once the core is
   right, not from day one — relevant to a v1.0 that's already shipped.
2. **Consistency of one system beats any single flourish.** Raycast's 2023
   overhaul was one designer redrawing every icon to one stroke
   width/corner radius — the whole before/after is icon-set *consistency*,
   nothing else ([Raycast blog](https://www.raycast.com/blog/a-fresh-look-and-feel)).
3. **Motion, where it exists, follows physical rules, not decorative
   ones** (Emil Kowalski's [Apple-design craft notes](https://github.com/emilkowalski/skills),
   Blake Crosley's [Arc](https://blakecrosley.com/guides/design/arc)/[Bear](https://blakecrosley.com/guides/design/bear)
   teardowns): default ease-out ~200ms for ~80% of transitions;
   overshoot/bounce reserved for gesture-driven motion only; **animate
   from the current on-screen value, never the target**; feedback triggers
   on press, not release. Arc: sidebar collapse `0.2s ease-out`, spring
   `response 0.3, damping 0.7`. Bear: 0.3s spring for panels, 0.2s
   ease-in-out for focus mode, and explicitly brags **"no spinners, no
   skeleton loading screens, no toast messages. Only smooth animations."**
4. **Shadows/elevation used sparingly, only to encode real hierarchy**
   (Fluent's key+ambient shadow model), reserved for genuinely
   floating/transient surfaces.
5. **"Invisible" details are where amateur apps leak** ([evilmartians.com "How to make absolutely any app look native"](https://evilmartians.com/chronicles/how-to-make-absolutely-any-app-look-like-a-macos-app)):
   default arrow cursor except where it opens a link; don't give styled
   buttons a hover glow (only ghost/plain buttons should react); match
   feedback intensity to what actually changed.
6. **Empty/error states are part of the design system, not an
   afterthought** — should look like it belongs to *this* feature; if a
   screen has no natural next action, staying quiet is fine.
7. **Speed and correctness are themselves a polish signal**, independent
   of visuals — echoed repeatedly across sources: "it's forgiving, it
   respects your time, nothing about it fights you."

### What's already true in MERGEN's own code

*(The agent read `mainwindow.cpp`, `pageview.cpp`, and `MX.md` directly
rather than guessing.)* MERGEN already applies several of the above
correctly, on its own:

- Search-hit and selection colors are **never hardcoded** — derived live
  from `QPalette::Highlight` at runtime (`pageview.cpp:444`, `:456`),
  including hue-rotating 180° so hits always contrast with the system
  accent color, whatever it is.
- The **current** search hit reuses the *same* derived color as an
  outline instead of a second arbitrary fill color (`pageview.cpp:535–540`)
  — real color discipline, not an accident.
- The DO-NOT list itself is a design asset — the same restraint-as-feature
  move Bear makes with "no toasts."
- Empty/error states are already "centered text in the page view, never a
  dialog box" — matches the empty-state research directly.
- The window title (`MERGEN ///— document.pdf —\\\ MEGAS`) is a deliberate
  typographic flourish specifically *because* there's no titlebar to
  decorate — solving the "no chrome" constraint creatively.

*(These gaps are the next layer, not "fix your fundamentals" — the
fundamentals are handled.)*

### Where generic advice doesn't apply, and why

- **No window shadow, blur, vibrancy, or rounded corners are available at
  all** — niri draws minimal/no decoration, and MERGEN draws none of its
  own. The only place "elevation" can legitimately exist is *inside*
  MERGEN's own client area.
- **No sidebar, ever** — rules out the Arc/Notion/Bear "polish the
  sidebar" playbook.
- **No theme switcher, no onboarding, no first-run flow** — rules out a
  large fraction of generic "delight" advice.
- **Icon font, not SVG, is a deliberate architecture choice** (keeps
  `qt6-svg` out of the runtime dependency graph) — switching would cut
  against a documented trade-off, not an oversight; ranked low for that
  reason, with cheaper fixes surfaced that get most of the benefit within
  the existing approach.
- **Live QPalette, no custom stylesheet** — closer to the Evil Martians
  "respect the platform" philosophy than to Raycast/Arc's fully custom
  chrome. The right move is to *author more states* using the palette
  that's already there, not introduce a hardcoded brand palette.

### Ranked punch list

| # | Idea | Impact | Effort |
|---|---|---|---|
| 1 | **Animate the state changes that currently snap.** `pageview.cpp:499/501` jumps the scrollbar to a search hit with a bare `setValue()`; zoom steps and the search bar's show/hide are similarly instant. Wrap in short `QPropertyAnimation`s (~150–250ms ease-out) on the scrollbar's `value` property (natively animatable, no subclassing). Largest gap versus every source reviewed; zero architecture change, zero dependencies, nothing on the DO-NOT list. | High | Low–Med |
| 2 | **Author the toolbar's hover/press feedback** instead of leaving it to the ambient QStyle. Every button is `setAutoRaise(true)` with no custom paint (`mainwindow.cpp:197–204`) — looks entirely different across Breeze/Fusion/Kvantum/Adwaita-Qt, often raw on stock Fusion. A small custom-painted `QToolButton` (or scoped `QProxyStyle`) drawing a **palette-derived**, softly-rounded highlight rect on hover/press fixes this without violating "no theming code." Needs a few lines of C++ paint code — plain QSS can't read `QPalette` roles at runtime. | High | Med |
| 3 | **Glyph optical-alignment and weight-consistency pass.** Toolbar concatenates glyph + two literal spaces + label (`mainwindow.cpp:196`) with no per-glyph baseline tuning; Font Awesome 5 mixes solid/regular/brands weights. Same payoff as Raycast's icon-set consistency pass, far cheaper here (9 codepoints/offsets to check, no icon set to redraw). | Med | Low |
| 4 | **Group the toolbar visually.** The 8 fixed actions are logically four clusters but lay out as one flat row. `addSeparator()` or slim spacers between clusters — cheapest possible application of "whitespace encodes hierarchy," directly legible given the toolbar is 100% of MERGEN's chrome. | Med | Low |
| 5 | **Verify the app icon at switcher/taskbar sizes.** The hand-illustrated badge's detailed texture/curled corner is exactly the kind of asset that turns to mud at 16–24px. Protect the one hero asset already invested in. | Med | Low |
| 6 | **Tooltip completeness pass.** With no menu bar, toolbar tooltips are the *only* place a user discovers keyboard shortcuts — a MERGEN-specific stake other apps don't have. Confirm every action shows name + shortcut consistently, including the page-counter field and recent-files dropdown. | Med | Low |
| 7 | **Consistent disabled-state treatment** for toolbar actions (Zoom in at 1000% cap, Print with nothing open) — palette-derived dimming authored once rather than whatever the ambient QStyle produces. | Med | Low–Med |
| 8 | **Cursor-shape discipline audit** across the page view — I-beam only over selectable text, arrow elsewhere, correct cursor during click-drag. Nobody notices when right, everybody notices when wrong. | Low–Med | Low |
| 9 | **Float the search bar** as a small overlay near the top of the page view with a soft `QGraphicsDropShadowEffect`, instead of the current docked bar. The one legitimate "elevation" moment available anywhere in MERGEN's surface, since it's inside the client area rather than window chrome. Purely optional — the docked version isn't wrong. | High (if done) | Med–High |
| 10 | **A quiet empty-state echo of the icon's material language** — the curled-corner/brushed-metal motif as a faint watermark behind the centered empty-state text. Reuses real existing art rather than inventing new decoration; stays inside "never a dialog, never onboarding" since it's idle-state art, not a prompt. | Med–High | Med |

**Two ideas surfaced but not ranked highly by the agent itself:**

- **SVG icon migration** — real, well-documented wisdom, but MERGEN's
  glyph-font choice is a deliberate trade recorded in spec to keep
  `qt6-svg` out of the runtime dependency graph. Punch-list #3 gets most
  of the benefit without reopening that decision.
- **UI sound effects** — skip. No established Linux-desktop sound
  convention the way iOS/macOS has one; MERGEN's own DO-NOT list is a
  restraint-by-policy document in the same spirit as Bear's "no toasts"
  bragging rights. Sound here would cut against the app's own stated
  identity.

### Sources

[Raycast blog](https://www.raycast.com/blog/a-fresh-look-and-feel) ·
[Linear — Craft](https://linear.app/now/craft) ·
[CleanShot X — OWC Blog](https://www.owc.com/blog/cleanshot-x-is-the-screenshot-utility-built-for-pros) ·
[Arc — Blake Crosley](https://blakecrosley.com/guides/design/arc) ·
[Bear — Blake Crosley](https://blakecrosley.com/guides/design/bear) ·
[apple-design skill — Emil Kowalski](https://github.com/emilkowalski/skills/blob/main/skills/apple-design/SKILL.md) ·
[Evil Martians — native macOS app look](https://evilmartians.com/chronicles/how-to-make-absolutely-any-app-look-like-a-macos-app) ·
[Details are the design — Signal v. Noise](https://signalvnoise.com/posts/184-details-are-the-design) ·
[Ask HN: exceptionally polished software](https://news.ycombinator.com/item?id=48486225) ·
[Apple Design Awards 2025](https://www.apple.com/newsroom/2025/06/apple-unveils-winners-and-finalists-of-the-2025-apple-design-awards/) ·
[Icon Fonts vs SVGs — KeyCDN](https://www.keycdn.com/blog/icon-fonts-vs-svgs) ·
[Elevation — Fluent 2](https://fluent2.microsoft.design/elevation) ·
[Elevation — Atlassian](https://atlassian.design/foundations/elevation) ·
[Empty state UI design — Setproduct](https://www.setproduct.com/blog/empty-state-ui-design).

---

## R5 — sudo-megas app family traditions

### Access summary

All research done via the GitHub REST API (`gh api`, authenticated as
`sudo-megas`) against `https://github.com/sudo-megas`. **Nothing was
private or inaccessible.** The account has 10 public repositories:

| Repo | Language | License | Created | One-line description |
|---|---|---|---|---|
| [SAAT](https://github.com/sudo-megas/SAAT) | Python | GPL-3.0 | 2026-07-22 | Wristwatch collection cataloguing app |
| [JADEITE](https://github.com/sudo-megas/JADEITE) | TypeScript | GPL-3.0 | 2026-08-03 | Encrypted personal finance vault |
| [PARACHRON](https://github.com/sudo-megas/PARACHRON) | Rust | **AGPL-3.0** | 2026-08-05 | Invoice/document/warranty organiser |
| [INDIUM](https://github.com/sudo-megas/INDIUM) | Rust | GPL-3.0 | 2026-08-08 | Archive manager, Linux/Wayland |
| [RESONANCE](https://github.com/sudo-megas/RESONANCE) | Go | GPL-3.0 | 2026-08-11 | Dotfile syncing tool |
| [indium.yazi](https://github.com/sudo-megas/indium.yazi) | Lua | GPL-3.0 | 2026-08-12 | Yazi file-manager plugin (companion to INDIUM) |
| [yazi-rs.github.io](https://github.com/sudo-megas/yazi-rs.github.io) | TypeScript | MIT | 2026-08-12 | **Fork** of the upstream Yazi docs site — not original work, excluded from analysis |
| [megas-xlr](https://github.com/sudo-megas/megas-xlr) | Shell | *(none)* | 2026-08-13 | The author's own personal pacman package repository |
| **[TRITIUM](https://github.com/sudo-megas/TRITIUM)** | TypeScript | GPL-3.0 | 2026-08-17 | Fuel logging / vehicle maintenance tracker — **public, read in full** |
| MERGEN | C++ | GPL-3.0 | 2026-08-24 (today) | This project |

Read: READMEs, LICENSE headers, commit history (subjects + full bodies,
checked for trailers), and the constitution/spec doc for every non-fork
repo: `XTRITIUM.md` (TRITIUM), `XJADEITE.md` (JADEITE), `CORE.md` (INDIUM,
PARACHRON, RESONANCE at `build/docs/CORE.md`), `SPEC.md` (SAAT). Also
checked MERGEN's own **public** root listing and README as ordinary
research surface. Per the task's framing, the agent deliberately did
**not** fetch `build/docs/MX.md` itself — that fact set was supplied
in-task rather than treated as open research surface.

### TRITIUM in detail

TRITIUM's [`XTRITIUM.md`](https://github.com/sudo-megas/TRITIUM/blob/main/XTRITIUM.md)
opens by calling itself **"the constitution of the project… When code and
XTRITIUM disagree, XTRITIUM wins until XTRITIUM is amended. Amendments are
edits to this file with a dated note — never silent drift."** States
outright: *"Family | Megas — app 6, alongside SAAT, JADEITE, PARACHRON,
INDIUM, RESONANCE"* — confirming the author refers to the whole body of
work as **"the Megas family"** and numbers apps sequentially (MERGEN,
created a week later, would be app 7).

Release mechanism, §9.2, literally named **"The PUTAG protocol"**:

> `PUTAG` → commit locally → tag locally → build locally. **No push.**
> `PUTAGREL` (final milestone only) → commit → tag → build → push → release.

Almost certainly the exact "per-milestone build-and-tag protocol used on
TRITIUM" MERGEN's own spec references replacing. Milestones coded `F1…F16`
(desktop) and `AF1…` (a later Android rewrite), each becoming a `v0.x.y`
tag with **decimal-roll versioning** ("the tenth version of a series is
`.0` of the next, never `.10`") — TRITIUM-specific, not found elsewhere in
the family. Packaging: `packaging/PKGBUILD`, built from a git tag in a
clean chroot (`extra-x86_64-build`), signed by hand, placed into the
author's own pacman repo, **megas-xlr** — explicitly "**No AUR**."

### Recurring conventions across the family

**1. One constitution/spec document, ranked above code — universal, name
varies.** Every non-fork repo has exactly one such file, each explicitly
claiming final authority: TRITIUM's `XTRITIUM.md` ("XTRITIUM wins"),
[PARACHRON's `CORE.md`](https://github.com/sudo-megas/PARACHRON/blob/master/CORE.md)
("Single source of truth... update CORE.md first"), [INDIUM's `CORE.md`](https://github.com/sudo-megas/INDIUM/blob/master/CORE.md),
[RESONANCE's `build/docs/CORE.md`](https://github.com/sudo-megas/RESONANCE/blob/master/build/docs/CORE.md)
("ratified by the maker... may not be changed... without his explicit
permission"), [JADEITE's `XJADEITE.md`](https://github.com/sudo-megas/JADEITE/blob/main/XJADEITE.md),
[SAAT's `SPEC.md`](https://github.com/sudo-megas/SAAT/blob/master/SPEC.md).
Notably, PARACHRON's own CORE.md contains a "Typical vs. Parachron uses"
table naming **`SPEC.md`** and **"Milestone 1, 2, 3…"** as the family
*baseline* it's deviating from — i.e. plain numeric milestone naming is
the stated family default, not a MERGEN invention.

**2. Milestone-based build → tag → release, one doc per milestone, always
project-coded.** TRITIUM's F/AF, PARACHRON's `Chron1.md, Chron2.md…`,
INDIUM's `P`-numbered phases, RESONANCE's `STEP1–5.md`, SAAT's inline
"milestone 19," "milestone 23/24." Commit history for **every** repo is
single-author, `sudo-megas`, no exceptions found.

**3. An explicit written DO-NOT / anti-feature list — sometimes literally
titled that.** INDIUM has `## 9. DO NOT` (e.g. "No tray icon, ever," "No
RAR," "No X11 — Wayland only"), SAAT has `## 9. Do not`, PARACHRON/JADEITE
express the same as numbered "Development rules"/hard constraints
functionally identical (PARACHRON binds its rules "for all tooling,
**including <assistant-cli>**").

**4. Zero AI attribution — stated almost verbatim family-wide, and
verified in practice.** INDIUM: *"No commits from any account other than
**sudo-megas**; no AI attribution anywhere."* [PARACHRON](https://github.com/sudo-megas/PARACHRON/blob/master/CORE.md):
*"**No AI attribution anywhere.** ...trailers like 'Made by <assistant>,'
'<assistant> Session,' '<assistant-cli>,' 'Co-Authored‑By: <assistant>'... Banned
outright."* JADEITE: *"No AI implications, flags, or banners anywhere in
the build or release process."* RESONANCE: *"**No AI trailers** in
commits, tags, or anywhere else. Ever."* SAAT bans *"tooling attribution
anywhere"* (broader than just AI). A full-history grep for
`<the vendor pattern>|produced with|<vendor-c>|<vendor-c-model>` across every
repo's commit bodies found **no AI trailers in any actual history** — the
only hits were incidental (`.<assistant>/` listed as gitignored) plus a SAAT
commit documenting the author running *"a full case-insensitive sweep...
two confirmed-unrelated hits"* before a release — this rule is **actively
audited**, not just aspirational. Nuance: the rule bans **credit/traces**,
not **use** — AI tooling is visibly used but must leave no fingerprint in
the record.

**5. GPL-3.0 is the default licence, with one outlier.** SAAT, JADEITE,
INDIUM, RESONANCE, TRITIUM, MERGEN are all GPL-3.0 (TRITIUM's README
self-declares "-or-later" explicitly; MERGEN's `PKGBUILD` pins
`GPL-3.0-only` — a small textual inconsistency, not a real deviation).
**PARACHRON alone is AGPL-3.0** — the sole license outlier.

**6. A shared README/About-page template.** TRITIUM's `XTRITIUM.md` §10
states policy: *"README in the family convention: banner, badges (version
/ release date / licence / Arch package size), bilingual subtitle,
numbered ALL-CAPS sections (DESCRIPTION, DEPENDENCIES, INSTALLATION, HOW
TO USE, LICENCE SUMMARY), 'Built with Reason and Passion.'"* and *"About
page in the family **cult layout**: the mark, the maker, version, release
date, source address, full licence text — addresses selectable, never
clickable."* Confirmed live in TRITIUM's actual README (shields.io badges
incl. a package-size badge, bilingual EN/TR subtitle, numbered sections,
sign-off line verbatim). SAAT and JADEITE follow the same shape.

**7. Distribution via the author's own pacman repo, `megas-xlr`,
self-signed, "no AUR."** [`megas-xlr`](https://github.com/sudo-megas/megas-xlr)
holds `README.md`, `install-megas.sh`, a published GPG key
(`sudo-megas-pubkey.asc`). TRITIUM's doc: packages go *"→ the maker's own
`megas-xlr` pacman repo... **No AUR**... The maker signs packages
himself."* INDIUM's DO-NOT list independently states *"No AppImage,
Flatpak, or Snap — only `.pkg.tar.zst` and `.deb`."* PARACHRON is the one
exception that designed an AUR package and then **withdrew** it when the
AUR was temporarily disabled, keeping the design struck-through rather
than deleted.

**8. Self-correction machinery: dated amendments and a "deviations"
ledger.** TRITIUM's `XTRITIUM.md` carries multiple inline **"Amendment —
19/08/2026."** blocks with dated correction notes. INDIUM's `CORE.md` has
a numbered `## 10. DEVIATIONS` section explicitly for recording where the
shipped app departs from spec "rather than quietly repaired." Both also
keep a separate `issues.md` defect ledger cross-referenced by ID.

**9. Dependency philosophy is a spectrum, not a single rule — and so is
platform scope.** MERGEN's "libraries earn their place" sits at the strict
end with INDIUM (*"a dependency enters INDIUM only when it does genuine
work... honest at fifty [total]"*) and SAAT (*"Three dependencies:
PySide6, tomlkit, Pillow. Anything else needs written [justification]"*).
TRITIUM calls its own budget **"Soft"**; JADEITE explicitly **rejects**
minimalism as a goal: *"Dependency count is not a constraint... Package
size up to 1 GB is acceptable."* Similarly, Arch+Wayland-only exclusivity
(MERGEN, INDIUM) is a minority stance — SAAT and PARACHRON both ship
Windows builds, RESONANCE ships Debian+Arch, TRITIUM ships a full separate
Android rewrite alongside its Arch desktop line.

### Observations for the author

**(1) Traditions MERGEN already follows:**
- One spec document (`build/docs/MX.md`) outranking code — the universal
  family pattern.
- Milestone-numbered build → tag → release (M1…M7 → v0.1.0…v1.0.0) —
  matches the family habit, and per PARACHRON's own comparison table,
  plain numeric "Milestone N" naming is literally the family's stated
  *default*.
- An explicit, written DO-NOT/anti-feature list — mirrors INDIUM's and
  SAAT's literal "DO NOT" sections.
- Zero AI attribution, enforced as policy — matches wording nearly
  verbatim across INDIUM, PARACHRON, JADEITE, RESONANCE, SAAT, and matches
  what's actually true of every commit in every sibling repo's history.
- Single-author (`sudo-megas`-only) commit discipline — explicit rule
  elsewhere, empirically 100% true across every repo checked.
- GPL-3.0 licensing — the dominant family default (5 of 6 non-fork
  siblings besides PARACHRON's AGPL-3.0 outlier).

**(2) Traditions visible elsewhere in the family that MERGEN doesn't (yet)
follow** — offered as observations, not directives:
- **No `README.md` currently exists in MERGEN's public repo** (root has
  `.clang-format`, `.github`, `.gitignore`, `CMakeLists.txt`, `LICENSE`,
  `PKGBUILD`, `build/`, `data/`, `src/` — no README). Every sibling ships
  one following the stated shared template. *(MX.md itself already notes
  the README is deliberately deferred to a later, unscheduled session —
  so this isn't a surprise, just confirms the template to follow when it
  happens.)* Given M6 already added an About dialog, a README covering the
  same "family cult layout" may be a natural pre/post-v1.0 companion.
- **Dated-amendment / deviations-ledger discipline** — TRITIUM's inline
  "Amendment — DATE" blocks, INDIUM's `DEVIATIONS` section + `issues.md`
  ledger. Not currently part of MX.md's stated process; worth considering
  for tracking MERGEN's evolution post-v1.0, or deliberately staying
  lighter given MERGEN's smaller scope.
- **Automated audit scripts enforcing the constitution in CI**, not just
  in prose — JADEITE's `audit-egress`/`audit-strings`/`audit-colours`/
  `audit-locale`; TRITIUM adopted them "in the JADEITE pattern"; INDIUM's
  own `check-deps.sh` gating dependency rules in CI on every push. MERGEN's
  zero-AI-attribution and dependency-minimalism rules currently live only
  as written policy, not CI-enforced.
- **Distribution into `megas-xlr`** with a self-signed package is the
  stated channel for TRITIUM and INDIUM, both explicitly "No AUR." M7's
  PKGBUILD/release workflow lands the packaging machinery, but nothing
  confirms whether `megas-xlr` publication is wired up for MERGEN too.
- Two minor/cosmetic notes: MERGEN keeps `PKGBUILD` unnested at repo root
  vs. TRITIUM/SAAT/PARACHRON's `packaging/` subdirectory; MERGEN's
  `PKGBUILD` pins `GPL-3.0-only` where TRITIUM states `GPL-3.0-or-later`.
- **Framed deliberately as *not* a gap:** MERGEN's strict
  dependency-minimalism and Arch+Wayland-only exclusivity are real family
  traditions, but belong to a specific wing of the family (shared with
  INDIUM, largely SAAT) rather than being universal — JADEITE explicitly
  rejects dependency minimalism, and SAAT/PARACHRON/TRITIUM(Android) all
  ship beyond Arch-only. MERGEN sitting at the strict/exclusive end is a
  considered stylistic position, not something "missing."
