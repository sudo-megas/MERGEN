#include <QApplication>
#include <QScrollBar>
#include <QPropertyAnimation>
#include <cstdio>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    std::printf("platform            : %s\n", qPrintable(QApplication::platformName()));
    std::printf("UI_General enabled  : %s\n", QApplication::isEffectEnabled(Qt::UI_General) ? "true" : "false");
    std::printf("UI_AnimateMenu      : %s\n", QApplication::isEffectEnabled(Qt::UI_AnimateMenu) ? "true" : "false");
    // Does a QPropertyAnimation on a scrollbar's value actually tick?
    QScrollBar bar; bar.setRange(0, 1000); bar.setValue(0);
    QPropertyAnimation a(&bar, "value");
    a.setDuration(200); a.setStartValue(0); a.setEndValue(1000);
    a.start();
    std::printf("immediately after start: %d (state=%d)\n", bar.value(), int(a.state()));
    return 0;
}
