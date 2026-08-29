#include "inkitem.h"
#include "stylus.h"

#include <QLibraryInfo>
#include <QPainter>
#include <QPen>
#include <QTimer>

#include <dlfcn.h>

namespace {
// Qt 以绝对路径加载场景图插件，dlopen 短文件名(RTLD_NOLOAD)会找不到，
// 需按插件目录定位已加载的 libqsgepaper.so。
void *epaperLib()
{
    static void *lib = nullptr;
    if (lib)
        return lib;
    lib = dlopen("libqsgepaper.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib)
        return lib;
    const QStringList names = {"libqsgepaper.so", "libqsgepaper.so.6"};
    const QStringList dirs = {
        QLibraryInfo::path(QLibraryInfo::PluginsPath) + "/scenegraph",
        "/usr/lib/plugins/scenegraph",
        "/usr/lib/qt6/plugins/scenegraph",
    };
    for (const QString &name : names) {
        for (const QString &dir : dirs) {
            lib = dlopen((dir + '/' + name).toUtf8().constData(),
                         RTLD_NOW | RTLD_NOLOAD);
            if (lib)
                return lib;
        }
    }
    return nullptr;
}
} // namespace

static const int SW = 1404;
static const int SH = 1872;

InkItem::InkItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::NoButton);
    m_img = QImage(SW, SH, QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    // 逐段提交会被 swtcon 掉帧导致虚线；大区域提交又慢(~300ms)。
    // 用 ~20ms 节流把近段笔迹合并成"小区域"提交：小区域处理快、
    // 覆盖近段保证实线、不掉帧。之前节流不跟手是因为用了 ±24 大矩形。
    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(1);
    connect(m_flushTimer, &QTimer::timeout, this, &InkItem::flushInk);
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
    clearPenBuffer();
    update();
}

void InkItem::loadBlank(int w, int h)
{
    m_img = QImage(qMax(w, 1), qMax(h, 1), QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    m_hasInk = false;
    emit hasInkChanged();
    clearPenBuffer();
    update();
}

bool InkItem::saveDraw(const QString &path) const
{
    if (!m_hasInk)
        return false;
    // PNG 低压缩快编码：收藏页不多，体积换取翻页流畅
    return m_img.save(path, "PNG", 30);
}

bool InkItem::loadDraw(const QString &path)
{
    QImage img(path);
    if (img.isNull())
        return false;
    m_img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_hasInk = true;
    emit hasInkChanged();
    clearPenBuffer();
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
    // 收尾：把节流期间累积的笔迹段立即提交，不丢尾段
    flushInk();
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
        // 抗锯齿：细斜线在覆盖不足处不再出现断点（断点会被 DU 显示成 dash）
        p.setRenderHint(QPainter::Antialiasing, true);
        m_width = 1.2 + qreal(pressure) / 4095.0 * 3.5;
        p.setPen(QPen(QColor(0, 0, 0), m_width, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    }
    p.drawPoint(m_last);
    p.end();
    m_stroke = true;
    if (!m_hasInk) {
        m_hasInk = true;
        emit hasInkChanged();
    }
    // 首段立即通过 pen 快速路径显示
    m_pending = segmentRect(m_last, m_last);
    if (!fastSubmit(m_pending))
        update(m_pending);
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
        p.setRenderHint(QPainter::Antialiasing, true);
        m_width = 1.2 + qreal(pressure) / 4095.0 * 3.5;
        p.setPen(QPen(QColor(0, 0, 0), m_width, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    }
    p.drawLine(m_last, cur);
    p.end();
    m_pending = m_pending.isNull()
                    ? segmentRect(m_last, cur)
                    : m_pending.united(segmentRect(m_last, cur));
    m_last = cur;
    // 节流合并：定时器到点把小区域一次性提交（小区域快 + 覆盖近段防虚线）
    if (!m_flushTimer->isActive())
        m_flushTimer->start();
}

void InkItem::flushInk()
{
    if (m_pending.isNull())
        return;
    if (!fastSubmit(m_pending))
        update(m_pending);
    m_pending = QRect();
    m_flushTimer->stop();
}

// 笔迹段的紧致脏区：线段包围盒 + 笔宽半径（含圆帽与抗锯齿余量）。
// 官方笔迹只刷新笔尖经过的细条区域，整块大矩形会闪得很厉害。
QRect InkItem::segmentRect(const QPointF &a, const QPointF &b) const
{
    const int r = qMax(2, int(m_width / 2) + 1);
    return QRect(a.toPoint(), b.toPoint()).normalized().adjusted(-r, -r, r, r);
}

// 绕过框架对 QQuickPaintedItem 区域的"默认灰度→慢波形"路径：笔迹段直接写进
// 8-bit pen 缓冲（FB112），再走 pen 快速分支（DU + pixel_mode 7）瞬时显示。
// 关键：pixel_mode 7 把 FB112 当"墨迹掩码"——非 0 的像素才用主画布(FB96)
// 的值驱动显示，0 的像素被 gate 跳过。所以 FB112 必须填掩码（有墨=255，
// 无墨=0），而不是页面拷贝；否则页面(255)被无谓重刷→闪烁，笔迹线(0)反而
// 被 gate 掉→显示成 dash。
// 失败（插件不可用/缓冲不对）返回 false，由调用方回退到框架 update()。
bool InkItem::fastSubmit(const QRect &region)
{
    if (m_erase || region.isNull())
        return false;
    void *lib = epaperLib();
    if (!lib)
        return false;
    using InstFn = void *(*)();
    using SwapFn = void (*)(void *, QRect, int, int, int);
    static auto inst = reinterpret_cast<InstFn>(
        dlsym(lib, "_ZN13EPFramebuffer8instanceEv"));
    static auto swap = reinterpret_cast<SwapFn>(
        dlsym(lib, "_ZN13EPFramebuffer11swapBuffersE5QRect13EPContentType"
                   "12EPScreenMode6QFlagsINS_10UpdateFlagEE"));
    if (!inst || !swap)
        return false;
    void *fb = inst();
    if (!fb)
        return false;
    auto *buf = reinterpret_cast<QImage *>(static_cast<char *>(fb) + 96);
    auto *penBuf = reinterpret_cast<QImage *>(static_cast<char *>(fb) + 112);
    if (buf->isNull() || penBuf->isNull())
        return false;
    if (buf->size() != m_img.size() || penBuf->size() != m_img.size())
        return false;
    {
        // 主画布（pixel_mode 7 读取的显示内容）：页面 + 笔迹
        QPainter p(buf);
        p.drawImage(region.topLeft(), m_img, region);
        p.end();
        // 8-bit pen 掩码：有墨=255，无墨=0（只驱动有墨像素 → 线实、页面不闪）
        for (int y = region.top(); y <= region.bottom(); ++y) {
            const QRgb *src =
                reinterpret_cast<const QRgb *>(m_img.constScanLine(y))
                + region.left();
            uchar *dst = penBuf->scanLine(y) + region.left();
            for (int x = region.left(); x <= region.right(); ++x, ++src, ++dst)
                *dst = qAlpha(*src) > 0 ? 255 : 0;
        }
    }
    // contentType=0(Pen) + flags=0 → 走 pen 分支 mode 1(DU)+pixel 7，
    // 与 xochitl 一致；flags=2 会进 fast 分支用波形 8（细线显示成虚线）
    swap(fb, region, 0, 1, 0);
    return true;
}

// 翻页/清空笔迹时清掉 8-bit pen 叠加层，防止旧笔迹叠到新页上。
void InkItem::clearPenBuffer()
{
    void *lib = epaperLib();
    if (!lib)
        return;
    using InstFn = void *(*)();
    static auto inst = reinterpret_cast<InstFn>(
        dlsym(lib, "_ZN13EPFramebuffer8instanceEv"));
    if (!inst)
        return;
    void *fb = inst();
    if (!fb)
        return;
    auto *penBuf = reinterpret_cast<QImage *>(static_cast<char *>(fb) + 112);
    if (penBuf->isNull() || penBuf->size() != m_img.size())
        return;
    penBuf->fill(0);   // 掩码全 0 = 无墨，全部 gate，不残留旧笔迹
}
