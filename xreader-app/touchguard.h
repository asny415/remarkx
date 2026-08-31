#pragma once

#include <QObject>
#include <QSizeF>
#include <functional>

class QTouchEvent;

// 触屏手掌误触过滤：区分手指单击与手掌托屏（手掌接触面积大）。
// 在应用事件过滤器里监视 QTouchEvent：笔处于使用中（写笔记）时把整段触摸
// 一律吞掉（此时不应有任何手指手势），笔空闲时把接触椭圆明显偏大的手掌
// 触摸整段吞掉。另过滤触摸合成的鼠标事件（SynthesizedByQt），防止平台把
// 触摸转成鼠标事件绕过 QTouchEvent 过滤。判定在 TouchBegin 一次性决定并
// 整段吞掉该触摸序列，避免中途切换导致 Qt Quick 触摸状态失步。
class TouchGuard : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool palmActive READ palmActive NOTIFY palmActiveChanged)
    Q_PROPERTY(qreal lastDiameter READ lastDiameter NOTIFY diameterChanged)
public:
    explicit TouchGuard(std::function<bool()> penActive,
                        QObject *parent = nullptr);

    bool eventFilter(QObject *obj, QEvent *ev) override;
    bool palmActive() const { return m_palmActive; }
    qreal lastDiameter() const { return m_lastDiameter; }

    // 笔空闲时，接触椭圆最大直径超过该值判定为手掌。单位是触屏原始上报值
    // （ABS_MT_TOUCH_MAJOR，一般 0~255，手指尖 ~20~40，手掌 ~80~200），
    // 可能需按设备微调。
    static constexpr qreal kPalmDiameter = 60.0;

signals:
    void palmActiveChanged();
    void diameterChanged();

private:
    void checkTouch(QTouchEvent *te);
    void setPalm(bool p);

    std::function<bool()> m_penActive;
    qreal m_lastDiameter = 0;
    bool m_palmActive = false;
    bool m_consume = false;
};
