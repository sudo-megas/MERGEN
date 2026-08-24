// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "iconset.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace mergen {

QIcon applicationIcon() {
    static const QIcon icon = [] {
        QIcon built;
        for (int size : {16, 32, 48, 64, 128, 256}) {
            built.addFile(QStringLiteral(":/icons/mergen-%1.png").arg(size));
        }
        return built;
    }();
    return icon;
}
namespace {

/// Fraction of the icon box the glyph's ink is scaled to fill. Below 1 so that
/// a tall glyph and a wide one read as the same weight beside each other.
constexpr double kFill = 0.80;

/// The families the Nerd Font patch ships under, in the order they are tried.
const QStringList &candidateFamilies() {
    static const QStringList families = {
        QStringLiteral("CaskaydiaCove Nerd Font"),
        QStringLiteral("CaskaydiaCove NF"),
        QStringLiteral("CaskaydiaMono Nerd Font"),
    };
    return families;
}

/// Resolved once: the first candidate family actually installed, or empty.
const QString &resolvedFamily() {
    static const QString family = [] {
        const QStringList installed = QFontDatabase::families();
        for (const QString &candidate : candidateFamilies()) {
            if (installed.contains(candidate)) {
                return candidate;
            }
        }
        return QString();
    }();
    return family;
}

struct CacheKey {
    char16_t code;
    int pixels;

    bool operator==(const CacheKey &other) const {
        return code == other.code && pixels == other.pixels;
    }
};

size_t qHash(const CacheKey &key, size_t seed = 0) {
    return ::qHash(static_cast<uint>(key.code), seed) ^ ::qHash(key.pixels, seed);
}

QHash<CacheKey, QIcon> &cache() {
    static QHash<CacheKey, QIcon> icons;
    return icons;
}

/// The palette and device pixel ratio the cache was built against. A pixmap
/// tinted for one palette is simply wrong under another, so both are watched
/// and the cache is dropped wholesale rather than patched.
qint64 &cachedPaletteKey() {
    static qint64 key = 0;
    return key;
}

qreal &cachedPixelRatio() {
    static qreal ratio = 0.0;
    return ratio;
}

/// Paints one glyph, centred on its ink rather than on the font's advance box,
/// so glyphs of different widths sit on the same optical centre.
QPixmap render(char16_t code, int pixels, const QColor &colour, qreal ratio) {
    const QString text = QString(QChar(code));

    QFont font = IconSet::glyphFont();
    font.setPixelSize(pixels);

    // Scale to the ink, not the em box: Font Awesome's glyphs vary enough in
    // height that a fixed point size makes some read heavier than others.
    const QRectF firstPass = QFontMetricsF(font).tightBoundingRect(text);
    const double extent = qMax(firstPass.width(), firstPass.height());
    if (extent > 0.0) {
        const double scaled = pixels * (pixels * kFill) / extent;
        font.setPixelSize(qMax(1, qRound(scaled)));
    }

    const QFontMetricsF metrics(font);
    const QRectF ink = metrics.tightBoundingRect(text);

    QPixmap pixmap(QSize(pixels, pixels) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(colour);
    // tightBoundingRect is relative to the baseline origin, so subtracting its
    // corner puts the ink's top-left where we want it.
    painter.drawText(QPointF((pixels - ink.width()) / 2.0 - ink.left(),
                             (pixels - ink.height()) / 2.0 - ink.top()),
                     text);
    return pixmap;
}

} // namespace

bool IconSet::hasGlyphFont() {
    return !resolvedFamily().isEmpty();
}

QString IconSet::glyphFamily() {
    return resolvedFamily();
}

QFont IconSet::glyphFont() {
    QFont font = QApplication::font();
    if (!resolvedFamily().isEmpty()) {
        font.setFamily(resolvedFamily());
    }
    return font;
}

void IconSet::clearCache() {
    cache().clear();
    cachedPaletteKey() = 0;
    cachedPixelRatio() = 0.0;
}

QIcon IconSet::icon(char16_t code, int pixels, const QPalette &palette) {
    if (!hasGlyphFont() || pixels <= 0) {
        return QIcon();
    }

    // Qt6 no longer falls back to another installed font for a missing code
    // point the way Qt5 did (QTBUG-110502), so a font revision that renumbers
    // or drops a glyph would draw a tofu box. Refuse instead: a button with a
    // label and no icon is honest, a button with a tofu box is not.
    if (!QFontMetricsF(glyphFont()).inFont(QChar(code))) {
        return QIcon();
    }

    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    if (palette.cacheKey() != cachedPaletteKey() || !qFuzzyCompare(ratio, cachedPixelRatio())) {
        cache().clear();
        cachedPaletteKey() = palette.cacheKey();
        cachedPixelRatio() = ratio;
    }

    const CacheKey key{code, pixels};
    const auto found = cache().constFind(key);
    if (found != cache().constEnd()) {
        return found.value();
    }

    QIcon icon;
    icon.addPixmap(
        render(code, pixels, palette.color(QPalette::Active, QPalette::ButtonText), ratio),
        QIcon::Normal);
    // The disabled colour comes from the palette's own disabled group rather
    // than from Qt's generic fade, so it dims the way the reader's theme dims.
    icon.addPixmap(
        render(code, pixels, palette.color(QPalette::Disabled, QPalette::ButtonText), ratio),
        QIcon::Disabled);
    icon.addPixmap(
        render(code, pixels, palette.color(QPalette::Active, QPalette::ButtonText), ratio),
        QIcon::Active);

    cache().insert(key, icon);
    return icon;
}

} // namespace mergen
