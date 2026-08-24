#include "iconset.h"
#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QPalette>
#include <QDebug>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const struct { const char *name; char16_t cp; } items[] = {
        {"Open", glyphs::kOpen}, {"ZoomOut", glyphs::kZoomOut}, {"ZoomIn", glyphs::kZoomIn},
        {"FitWidth", glyphs::kFitWidth}, {"FitPage", glyphs::kFitPage}, {"Rotate", glyphs::kRotate},
        {"Search", glyphs::kSearch}, {"Print", glyphs::kPrint}, {"Close", glyphs::kClose},
    };
    qInfo() << "glyph family:" << IconSet::glyphFamily() << " hasFont:" << IconSet::hasGlyphFont();
    const int sizes[] = {16, 22, 24, 32};
    QPixmap sheet(9 * 40 + 60, 4 * 40 + 10);
    sheet.fill(Qt::white);
    QPainter p(&sheet);
    QPalette pal;
    pal.setColor(QPalette::Active, QPalette::ButtonText, Qt::black);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(160,160,160));
    int row = 0;
    for (int s : sizes) {
        p.drawText(2, row*40 + 24, QString::number(s));
        int col = 0;
        for (const auto &it : items) {
            QIcon ic = IconSet::icon(it.cp, s, pal);
            if (ic.isNull()) { qWarning() << "NULL ICON for" << it.name; }
            // last column shows the Disabled state instead
            QIcon::Mode m = (col == 8) ? QIcon::Disabled : QIcon::Normal;
            p.drawPixmap(60 + col*40, row*40 + 5, ic.pixmap(QSize(s,s), 1.0, m));
            col++;
        }
        row++;
    }
    p.end();
    sheet.save("icons.png");
    qInfo() << "wrote icons.png";
    return 0;
}
