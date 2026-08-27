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
    QImage m_img;
    QPointF m_last;
    bool m_stroke = false;
    bool m_erase = false;
    qreal m_width = 4.0;
    bool m_hasInk = false;
};
