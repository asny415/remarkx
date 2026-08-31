#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QRect>

class QTimer;
class Stylus;

class InkItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(bool hasInk READ hasInk NOTIFY hasInkChanged)
    Q_PROPERTY(bool inkEnabled READ inkEnabled WRITE setInkEnabled
               NOTIFY inkEnabledChanged)
public:
    explicit InkItem(QQuickItem *parent = nullptr);

    bool hasInk() const { return m_hasInk; }
    // 是否接受笔迹：不透明白层（校准/加载/错误/全屏看图/全屏全文）盖住
    // 基础页时置 false——那时画下的墨迹看不见，留在墨层里只会被
    // saveInkNow 当笔迹误收藏帖子
    bool inkEnabled() const { return m_inkEnabled; }
    void setInkEnabled(bool enabled);
    void paint(QPainter *painter) override;

    const QImage &inkImage() const { return m_img; }
    // 笔迹起始位置（最靠上、最靠左的墨迹像素，用于判断归属哪个帖子）
    QPoint inkStart() const;
    // 是否真的有墨迹像素（擦除痕迹不算）
    bool hasInkPixels() const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE bool saveDraw(const QString &path) const;
    Q_INVOKABLE bool loadDraw(const QString &path);
    Q_INVOKABLE void loadBlank(int w, int h);

public slots:
    void setStylus(QObject *stylus);

signals:
    void hasInkChanged();
    void inkEnabledChanged();

private slots:
    void onPenDown(int x, int y, int pressure);
    void onPenMove(int x, int y, int pressure);
    void onPenUp();
    void onErDown(int x, int y, int pressure);
    void onErMove(int x, int y, int pressure);
    void onErUp();

private:
    void strokeDown(int x, int y, int pressure, bool eraser);
    void strokeMove(int x, int y, int pressure, bool eraser);
    // 笔迹段的紧致脏区（线段包围盒 + 笔宽半径），避免整块大矩形闪烁
    QRect segmentRect(const QPointF &a, const QPointF &b) const;
    // 直接写 8-bit pen 缓冲 + DU 快速波形下发，绕过框架慢路径；失败返回 false
    bool fastSubmit(const QRect &region);
    // 节流定时器到点：把近段时间累积的笔迹段合并成一次小区域提交
    void flushInk();
    // 翻页/清空时同步清掉 8-bit pen 叠加层，防止旧笔迹叠到新页上
    void clearPenBuffer();

    QImage m_img;
    QPointF m_last;
    bool m_stroke = false;
    bool m_erase = false;
    qreal m_width = 4.0;
    bool m_hasInk = false;
    bool m_inkEnabled = true;
    QTimer *m_flushTimer = nullptr;
    QRect m_pending;
};
