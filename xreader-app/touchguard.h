#pragma once

#include <QObject>
#include <QSizeF>
#include <functional>

class QTouchEvent;

// 触屏手掌误触过滤：区分手指单击与手掌托屏（手掌接触面积大）。
// 在应用事件过滤器里监视 QTouchEvent 的接触椭圆直径，当笔处于使用中（写笔记）
// 时，把明显偏大的手掌触摸整段吞掉，避免误触打开图片/按钮/翻页；手指单击
// 照常放行。判定在 TouchBegin 一次性决定并整段吞掉该触摸序列，避免中途切换
// 导致 Qt Quick 触摸状态失步。
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

    // 接触椭圆最大直径超过该值判定为手掌。单位是触屏原始上报值（ABS_MT_TOUCH_
    // MAJOR，一般 0~255，手指尖 ~20~40，手掌 ~80~200），可能需按设备微调。
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
