#include "touchguard.h"

#include <QEvent>
#include <QTouchEvent>
#include <QDebug>

TouchGuard::TouchGuard(std::function<bool()> penActive, QObject *parent)
    : QObject(parent), m_penActive(std::move(penActive))
{
}

bool TouchGuard::eventFilter(QObject *obj, QEvent *ev)
{
    Q_UNUSED(obj);
    switch (ev->type()) {
    case QEvent::TouchBegin:
        // 在 Begin 一次性判定：笔在用 + 接触明显偏大 → 手掌，整段吞掉
        checkTouch(static_cast<QTouchEvent *>(ev));
        m_consume = m_palmActive;
        return m_consume;
    case QEvent::TouchUpdate:
        return m_consume;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        const bool c = m_consume;
        m_consume = false;
        return c;
    }
    default:
        return false;
    }
}

void TouchGuard::checkTouch(QTouchEvent *te)
{
    qreal maxD = 0;
    for (const QTouchEvent::TouchPoint &pt : te->points()) {
        const QSizeF d = pt.ellipseDiameters();
        maxD = qMax(maxD, qMax(d.width(), d.height()));
    }
    if (maxD != m_lastDiameter) {
        m_lastDiameter = maxD;
        emit diameterChanged();
    }
    const bool palm = m_penActive() && maxD >= kPalmDiameter;
    if (palm)
        qInfo() << "PALM touch diameter" << maxD << "at"
                << te->points().first().position() << "consumed";
    setPalm(palm);
}

void TouchGuard::setPalm(bool p)
{
    if (m_palmActive == p)
        return;
    m_palmActive = p;
    emit palmActiveChanged();
}
