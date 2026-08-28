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
    // 手写笔是否处于使用中（笔按下，或刚合成过点击）。手势只在非 penActive 时生效。
    bool penActive() const { return m_penActive; }

    bool start();
    Q_INVOKABLE void setCalib(qreal a, qreal b, qreal c,
                              qreal d, qreal e, qreal f);
    Q_INVOKABLE void loadCalib(const QString &file);
    Q_INVOKABLE void saveCalib(const QString &file) const;
    Q_INVOKABLE QPointF rawToScreen(qreal rx, qreal ry) const;
    Q_PROPERTY(bool tapEnabled MEMBER m_tapEnabled CONSTANT)

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
    void synthesizeTap(int x, int y);
    void setPenActive(bool active);

    int m_fd = -1;
    QSocketNotifier *m_not = nullptr;
    QTimer *m_tapTimer = nullptr;
    bool m_touching = false;
    bool m_penActive = false;
    bool m_eraser = false;
    bool m_started = false;
    int m_lastX = 0, m_lastY = 0, m_lastP = 0;
    qreal m_a = 0, m_b = 0.0893, m_c = 0;
    qreal m_d = 0.0893, m_e = 0, m_f = 0;
    bool m_calibrated = false;
    bool m_tapEnabled = true;
    qint64 m_pressMs = 0;
    QPointF m_pressPt;
    double m_travel = 0;
};
