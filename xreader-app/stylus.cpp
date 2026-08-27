#include "stylus.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGui/6.8.2/QtGui/qpa/qwindowsysteminterface.h>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

static const int SCREEN_W = 1404;
static const int SCREEN_H = 1872;

static const qint64 TAP_MAX_MS = 400;
static const qreal TAP_MAX_TRAVEL = 24.0;

Stylus::Stylus(QObject *parent) : QObject(parent) {}

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

void Stylus::synthesizeTap(int x, int y)
{
    QWindow *win = QGuiApplication::topLevelWindows().isEmpty()
                        ? nullptr
                        : QGuiApplication::topLevelWindows().first();
    if (!win)
        return;
    const QPointF pos(x, y);
    ulong ts = QDateTime::currentMSecsSinceEpoch() & 0xffffffffUL;
    QWindowSystemInterface::handleMouseEvent(win, ts, pos, pos,
                                             Qt::LeftButton, Qt::LeftButton,
                                             QEvent::MouseButtonPress);
    QWindowSystemInterface::handleMouseEvent(win, ts + 1, pos, pos,
                                             Qt::NoButton, Qt::LeftButton,
                                             QEvent::MouseButtonRelease);
    qInfo() << "TAP synthesized" << x << y;
}

void Stylus::touchPoint(bool eraser)
{
    QPointF s = rawToScreen(m_lastX, m_lastY);
    m_pressMs = QDateTime::currentMSecsSinceEpoch();
    m_pressPt = s;
    m_travel = 0;
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
            } else if (ev.code == BTN_TOUCH) {
                if (ev.value) {
                    m_touching = true;
                } else if (m_touching) {
                    m_touching = false;
                    m_started = false;
                    const qint64 dur = QDateTime::currentMSecsSinceEpoch() - m_pressMs;
                    qInfo() << "pen gesture: dur" << dur << "travel" << m_travel
                            << "at" << m_pressPt;
                    // 校准前禁用（防误触跳过按钮）；校准后短促笔点可点按钮
                    if (!m_eraser && m_tapEnabled && m_calibrated
                        && dur < TAP_MAX_MS && m_travel < TAP_MAX_TRAVEL) {
                        synthesizeTap(int(m_pressPt.x()), int(m_pressPt.y()));
                        emit penUp();
                        return;
                    }
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
                if (!m_started) {
                    touchPoint(m_eraser);
                } else {
                    QPointF s = rawToScreen(m_lastX, m_lastY);
                    m_travel += qSqrt(qPow(s.x() - m_pressPt.x(), 2)
                                      + qPow(s.y() - m_pressPt.y(), 2));
                    m_pressPt = s;
                    if (m_eraser)
                        emit eraserMove(int(s.x()), int(s.y()), m_lastP);
                    else
                        emit penMove(int(s.x()), int(s.y()), m_lastP);
                }
            }
        }
    }
}
