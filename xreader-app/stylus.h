#include <QObject>
#include <QPointF>
#include <QSocketNotifier>

struct input_event;
class QTimer;

class Stylus : public QObject {
    Q_OBJECT
public:
    explicit Stylus(QObject *parent = nullptr);
    ~Stylus() override;

    Q_PROPERTY(bool calibrated READ calibrated NOTIFY calibChanged)
    Q_PROPERTY(bool penActive READ penActive NOTIFY penActiveChanged)
    bool calibrated() const { return m_calibrated; }
    // 手写笔是否处于使用中（笔尖在有效范围内，含悬停与按下，或刚离开后的一小段
    // 保护窗口）。手势只在非 penActive 时生效——笔靠近屏幕（准备写/正在写）时
    // 手掌误触屏幕不会触发任何手势。
    bool penActive() const { return m_penActive; }

    bool start();
    Q_INVOKABLE void setCalib(qreal a, qreal b, qreal c,
                              qreal d, qreal e, qreal f);
    Q_INVOKABLE void loadCalib(const QString &file);
    Q_INVOKABLE void saveCalib(const QString &file) const;
    Q_INVOKABLE QPointF rawToScreen(qreal rx, qreal ry) const;
    // 距最后一次笔/橡皮活动（靠近/按下/移动/抬起）的毫秒数，用于 QML 判断
    // "最近在写字"——手势只允许在笔完全空闲一段时间后才生效
    Q_INVOKABLE qreal penIdleMs() const;

signals:
    void calibChanged();
    void penActiveChanged();
    void penDown(int x, int y, int pressure);
    void penMove(int x, int y, int pressure);
    void penUp();
    void eraserDown(int x, int y, int pressure);
    void eraserMove(int x, int y, int pressure);
    void eraserUp();
    void rawPenDown(int rx, int ry);

private:
    void onData();
    void touchPoint(bool eraser);
    void onPenNear(bool near);
    void setPenActive(bool active);

    int m_fd = -1;
    QSocketNotifier *m_not = nullptr;
    QTimer *m_tapTimer = nullptr;
    bool m_touching = false;
    bool m_penActive = false;
    bool m_eraser = false;
    bool m_penNear = false;    // 笔尖在有效范围内（悬停/按下），未离开屏幕上方
    bool m_started = false;
    int m_lastX = 0, m_lastY = 0, m_lastP = 0;
    qreal m_a = 0, m_b = 0.0893, m_c = 0;
    qreal m_d = 0.0893, m_e = 0, m_f = 0;
    bool m_calibrated = false;
    qint64 m_lastActivityMs = 0;
};
