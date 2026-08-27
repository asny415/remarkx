#include "inkitem.h"
#include "stylus.h"

#include <QPainter>
#include <QPen>

static const int SW = 1404;
static const int SH = 1872;

InkItem::InkItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::NoButton);
    m_img = QImage(SW, SH, QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
}

void InkItem::setStylus(QObject *stylus)
{
    auto *s = qobject_cast<Stylus *>(stylus);
    if (!s)
        return;
    connect(s, &Stylus::penDown, this, &InkItem::onPenDown);
    connect(s, &Stylus::penMove, this, &InkItem::onPenMove);
    connect(s, &Stylus::penUp, this, &InkItem::onPenUp);
    connect(s, &Stylus::eraserDown, this, &InkItem::onErDown);
    connect(s, &Stylus::eraserMove, this, &InkItem::onErMove);
    connect(s, &Stylus::eraserUp, this, &InkItem::onErUp);
}

void InkItem::paint(QPainter *painter)
{
    painter->drawImage(0, 0, m_img);
}

void InkItem::clear()
{
    m_img.fill(Qt::transparent);
    m_hasInk = false;
    emit hasInkChanged();
    update();
}

void InkItem::loadBlank(int w, int h)
{
    m_img = QImage(qMax(w, 1), qMax(h, 1), QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    m_hasInk = false;
    emit hasInkChanged();
    update();
}

bool InkItem::saveDraw(const QString &path) const
{
    if (!m_hasInk)
        return false;
    return m_img.save(path, "PNG");
}

bool InkItem::loadDraw(const QString &path)
{
    QImage img(path);
    if (img.isNull())
        return false;
    m_img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_hasInk = true;
    emit hasInkChanged();
    update();
    return true;
}

void InkItem::onPenDown(int x, int y, int pressure)
{
    strokeDown(x, y, pressure, false);
}

void InkItem::onPenMove(int x, int y, int pressure)
{
    strokeMove(x, y, pressure, false);
}

void InkItem::onPenUp()
{
    m_stroke = false;
}

void InkItem::onErDown(int x, int y, int pressure)
{
    strokeDown(x, y, pressure, true);
}

void InkItem::onErMove(int x, int y, int pressure)
{
    strokeMove(x, y, pressure, true);
}

void InkItem::onErUp()
{
    m_stroke = false;
}

void InkItem::strokeDown(int x, int y, int pressure, bool eraser)
{
    m_erase = eraser;
    m_last = QPointF(x, y);
    QPainter p(&m_img);
    if (eraser) {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(QPen(Qt::transparent, 26, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    } else {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        m_width = 1.8 + qreal(pressure) / 4095.0 * 6.0;
        p.setPen(QPen(QColor(15, 15, 15), m_width, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    }
    p.drawPoint(m_last);
    p.end();
    m_stroke = true;
    if (!m_hasInk) {
        m_hasInk = true;
        emit hasInkChanged();
    }
    QRect r(m_last.toPoint() - QPoint(24, 24), QSize(48, 48));
    update(r);
}

void InkItem::strokeMove(int x, int y, int pressure, bool eraser)
{
    if (!m_stroke)
        return;
    QPointF cur(x, y);
    QPainter p(&m_img);
    if (eraser) {
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(QPen(Qt::transparent, 26, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    } else {
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        m_width = 1.8 + qreal(pressure) / 4095.0 * 6.0;
        p.setPen(QPen(QColor(15, 15, 15), m_width, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    }
    p.drawLine(m_last, cur);
    p.end();
    QRect r = QRect(m_last.toPoint(), cur.toPoint()).normalized()
                  .adjusted(-24, -24, 24, 24);
    m_last = cur;
    update(r);
}
