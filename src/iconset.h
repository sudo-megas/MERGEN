// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QFont>
#include <QIcon>

class QPalette;

namespace mergen {

/// Font Awesome 5 code points as patched into CaskaydiaCove Nerd Font. MERGEN
/// ships no image assets and depends on no icon theme — MZ.md §6.
///
/// Written as escapes rather than literal characters: these live in the Unicode
/// private use area, where they render as nothing in most editors and are
/// silently dropped by tools that do not expect them.
namespace glyphs {
constexpr char16_t kOpen = u'\uF07C';
constexpr char16_t kZoomOut = u'\uF010';
constexpr char16_t kZoomIn = u'\uF00E';
constexpr char16_t kFitWidth = u'\uF07E';
constexpr char16_t kFitPage = u'\uF065';
constexpr char16_t kRotate = u'\uF01E';
constexpr char16_t kSearch = u'\uF002';
constexpr char16_t kPrint = u'\uF02F';
constexpr char16_t kClose = u'\uF00D';
} // namespace glyphs

/// Turns a glyph into a real icon.
///
/// v1.0 set the glyph as the action's text, which cost three things at once: an
/// icon-only toolbar would have hidden it, assistive technology read a
/// private-use code point aloud, and the disabled and active states relied on
/// whatever dimming the text happened to inherit. All three follow from the
/// same mistake. Here the glyph is painted into a pixmap and attached with
/// setIcon(), leaving the action's text free to be a word — MZ.md §9.
class IconSet {
public:
    /// Whether the glyph font is actually installed. A missing font costs the
    /// glyphs, not the toolbar: callers fall back to text-only buttons.
    static bool hasGlyphFont();

    /// The glyph font at the application's point size.
    static QFont glyphFont();

    /// The family that was found, for the About dialog and for verification.
    /// Empty when none of the candidates are installed.
    static QString glyphFamily();

    /// A cached icon for one glyph at one edge length, carrying Normal,
    /// Disabled and Active pixmaps drawn from \a palette. Returns a null icon
    /// when the font is missing, which leaves the button showing its label
    /// alone rather than a placeholder.
    ///
    /// The cache is keyed by code point and size and is dropped wholesale when
    /// \a palette differs from the one it was built against: a pixmap tinted
    /// for the old palette is wrong the moment the reader's theme changes.
    static QIcon icon(char16_t code, int pixels, const QPalette &palette);

    /// Drops every cached pixmap. Call on a palette change.
    static void clearCache();
};

} // namespace mergen
