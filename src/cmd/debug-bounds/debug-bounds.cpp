/*
 * EtherWaver debug bounds overlay.
 *
 * Draws a persistent red rectangle at the coordinates supplied by the client
 * enter() path. The process stays alive until the client replaces or kills it.
 */

#include <QApplication>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <cstdlib>

class BoundsOverlay : public QWidget {
public:
    explicit BoundsOverlay(const QRect& bounds)
        : QWidget(NULL, Qt::Tool | Qt::FramelessWindowHint |
                         Qt::WindowStaysOnTopHint |
                         Qt::X11BypassWindowManagerHint)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(bounds);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(Qt::red, 6));
        painter.setBrush(QColor(255, 0, 0, 24));
        painter.drawRect(rect().adjusted(3, 3, -4, -4));
    }
};

static bool parseInt(const char* text, int& value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    char* end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        return 2;
    }

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!parseInt(argv[1], x) ||
        !parseInt(argv[2], y) ||
        !parseInt(argv[3], w) ||
        !parseInt(argv[4], h) ||
        w <= 0 || h <= 0) {
        return 2;
    }

    QApplication app(argc, argv);
    BoundsOverlay overlay(QRect(x, y, w, h));
    overlay.show();
    overlay.raise();

    QTimer raiseTimer;
    QObject::connect(&raiseTimer, &QTimer::timeout, [&overlay]() {
        overlay.raise();
        overlay.update();
    });
    raiseTimer.start(1000);

    return app.exec();
}
