#include "powerkey.h"

#include <QDateTime>
#include <QDebug>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

static const qint64 SHORT_PRESS_MS = 1500;

PowerKey::PowerKey(QObject *parent) : QObject(parent) {}

PowerKey::~PowerKey()
{
    if (m_not)
        delete m_not;
    if (m_fd >= 0)
        ::close(m_fd);
}

bool PowerKey::start()
{
    m_fd = ::open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        qWarning("PowerKey: cannot open /dev/input/event0");
        return false;
    }
    m_not = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_not, &QSocketNotifier::activated, this, &PowerKey::onData);
    return true;
}

void PowerKey::onData()
{
    struct input_event ev;
    while (::read(m_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY || ev.code != KEY_POWER)
            continue;
        if (ev.value == 1 && !m_down) {
            m_down = true;
            m_downMs = QDateTime::currentMSecsSinceEpoch();
        } else if (ev.value == 0 && m_down) {
            m_down = false;
            const qint64 dur = QDateTime::currentMSecsSinceEpoch() - m_downMs;
            if (dur < SHORT_PRESS_MS) {
                qInfo() << "power short press" << dur << "ms";
                emit shortPress();
            }
        }
    }
}
