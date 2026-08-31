#include "stylus.h"

#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QTimer>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

static const int SCREEN_W = 1404;
static const int SCREEN_H = 1872;

Stylus::Stylus(QObject *parent) : QObject(parent)
{
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    // 笔/橡皮在有效范围内（悬停或按下）及刚离开后的一段时间内保持 penActive：
    // 手写时手掌可能碰到屏幕，这段时间内忽略手指手势。笔离开有效范围后再延时
    // 1s 清除，避免刚停笔时手掌误触翻页。
    m_tapTimer = new QTimer(this);
    m_tapTimer->setSingleShot(true);
    m_tapTimer->setInterval(1000);
    connect(m_tapTimer, &QTimer::timeout, this, [this]() {
        // 只有笔/橡皮都不在有效范围、也未按下时才清除
        if (!m_penNear && !m_eraser && !m_touching)
            setPenActive(false);
    });
}

Stylus::~Stylus()
{
    if (m_not)
        delete m_not;
    if (m_fd >= 0)
        ::close(m_fd);
}

bool Stylus::start()
{
    m_fd = ::open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        qWarning("Stylus: cannot open /dev/input/event1");
        return false;
    }
    m_not = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_not, &QSocketNotifier::activated, this, &Stylus::onData);
    return true;
}

void Stylus::setCalib(qreal a, qreal b, qreal c, qreal d, qreal e, qreal f)
{
    m_a = a; m_b = b; m_c = c;
    m_d = d; m_e = e; m_f = f;
    m_calibrated = true;
    emit calibChanged();
}

void Stylus::loadCalib(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.contains("a")) {
        m_a = o["a"].toDouble();
        m_b = o["b"].toDouble();
        m_c = o["c"].toDouble();
        m_d = o["d"].toDouble();
        m_e = o["e"].toDouble();
        m_f = o["f"].toDouble();
        m_calibrated = true;
        emit calibChanged();
    }
    f.close();
}

void Stylus::saveCalib(const QString &file) const
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QJsonObject o;
    o["a"] = m_a; o["b"] = m_b; o["c"] = m_c;
    o["d"] = m_d; o["e"] = m_e; o["f"] = m_f;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.close();
}

QPointF Stylus::rawToScreen(qreal rx, qreal ry) const
{
    qreal sx = m_a * rx + m_b * ry + m_c;
    qreal sy = m_d * rx + m_e * ry + m_f;
    sx = qBound<qreal>(0, sx, SCREEN_W - 1);
    sy = qBound<qreal>(0, sy, SCREEN_H - 1);
    return QPointF(sx, sy);
}

qreal Stylus::penIdleMs() const
{
    return qreal(QDateTime::currentMSecsSinceEpoch() - m_lastActivityMs);
}

// 笔/橡皮进入或离开有效范围。进入时立即保持 penActive（手带笔靠近屏幕、
// 笔尖还没碰到屏幕就已算"在用"，手掌此时触屏不会误触）；离开时用定时器
// 留一小段保护窗口。
void Stylus::onPenNear(bool near)
{
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    qInfo() << (near ? "pen near" : "pen far");
    if (near) {
        m_tapTimer->stop();
        setPenActive(true);
    } else if (!m_touching) {
        m_tapTimer->start();
    }
}

void Stylus::setPenActive(bool active)
{
    if (m_penActive == active)
        return;
    m_penActive = active;
    emit penActiveChanged();
}

void Stylus::touchPoint(bool eraser)
{
    QPointF s = rawToScreen(m_lastX, m_lastY);
    if (eraser)
        emit eraserDown(int(s.x()), int(s.y()), m_lastP);
    else
        emit penDown(int(s.x()), int(s.y()), m_lastP);
    emit rawPenDown(m_lastX, m_lastY);
    m_started = true;
}

void Stylus::onData()
{
    struct input_event ev;
    while (::read(m_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY) {
            if (ev.code == BTN_TOOL_RUBBER) {
                m_eraser = ev.value != 0;
                onPenNear(m_eraser);
            } else if (ev.code == BTN_TOOL_PEN) {
                m_penNear = ev.value != 0;
                onPenNear(m_penNear);
            } else if (ev.code == BTN_TOUCH) {
                m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                if (ev.value) {
                    m_touching = true;
                    m_tapTimer->stop();
                    setPenActive(true);
                } else if (m_touching) {
                    m_touching = false;
                    m_started = false;
                    // 笔/橡皮抬起后仍保留 penActive（1s 窗口，或笔仍在有效范围
                    // 则更久），期间忽略手掌/手指误触
                    if (!m_penNear && !m_eraser)
                        m_tapTimer->start();
                    if (m_eraser)
                        emit eraserUp();
                    else
                        emit penUp();
                }
            }
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X)
                m_lastX = ev.value;
            else if (ev.code == ABS_Y)
                m_lastY = ev.value;
            else if (ev.code == ABS_PRESSURE)
                m_lastP = ev.value;
            if (m_touching && (ev.code == ABS_X || ev.code == ABS_Y)) {
                m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                if (!m_started) {
                    touchPoint(m_eraser);
                } else {
                    QPointF s = rawToScreen(m_lastX, m_lastY);
                    if (m_eraser)
                        emit eraserMove(int(s.x()), int(s.y()), m_lastP);
                    else
                        emit penMove(int(s.x()), int(s.y()), m_lastP);
                }
            }
        }
    }
}
