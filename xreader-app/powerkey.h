#pragma once

#include <QObject>
#include <QSocketNotifier>

struct input_event;

// 监听电源键（/dev/input/event0）：短按下落下抬起触发 shortPress()
class PowerKey : public QObject {
    Q_OBJECT
public:
    explicit PowerKey(QObject *parent = nullptr);
    ~PowerKey() override;

    bool start();

signals:
    void shortPress();

private slots:
    void onData();

private:
    int m_fd = -1;
    QSocketNotifier *m_not = nullptr;
    bool m_down = false;
    qint64 m_downMs = 0;
};
