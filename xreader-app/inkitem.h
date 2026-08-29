#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QRect>

class Stylus;

class InkItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(bool hasInk READ hasInk NOTIFY hasInkChanged)
public:
    explicit InkItem(QQuickItem *parent = nullptr);

    bool hasInk() const { return m_hasInk; }
    void paint(QPainter *painter) override;

    Q_INVOKABLE void clear();
    Q_INVOKABLE bool saveDraw(const QString &path) const;
    Q_INVOKABLE bool loadDraw(const QString &path);
    Q_INVOKABLE void loadBlank(int w, int h);

public slots:
    void setStylus(QObject *stylus);

signals:
    void hasInkChanged();

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
    // 翻页/清空时同步清掉 8-bit pen 叠加层，防止旧笔迹叠到新页上
    void clearPenBuffer();

    QImage m_img;
    QPointF m_last;
    bool m_stroke = false;
    bool m_erase = false;
    qreal m_width = 4.0;
    bool m_hasInk = false;
};
