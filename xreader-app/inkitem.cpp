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

// 目标笔宽（压感 0~4095 → 2.2~5.7px）。下限 2.2px 不是审美选择：DU 快速
// 通道是 1 位显示，45° 斜线最窄也要 ≥√2 px 才能让线经过的每个像素都满墨
// （覆盖 <50% 的像素被二值栅格化跳过=断点=虚线）；上限与旧版 4.7px 接近，
// 整体仍偏细
static inline qreal inkTargetWidth(int pressure)
{
    return 2.2 + qreal(pressure) / 4095.0 * 3.5;
}

InkItem::InkItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::NoButton);
    m_img = QImage(SW, SH, QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    // 提交节奏的平衡（swtcon 生成器线程限速消化更新，超出会排队/掉帧）：
    // - 1ms 逐段提交：生成器追不上（"generator thread has fallen behind"），
    //   排队延迟越滚越大=笔迹拖后，挤掉的帧=线里断点（虚线）
    // - 20ms 以上：合并区域变大，笔尖可见"拖后"
    // - 10ms：覆盖 2~4 个采样点（笔 220Hz），正常书写每次区域仅几~几十像素，
    //   延迟与刷新面积平衡最好（10ms 是此前实测的 swtcon 安全区）
    // 区域取紧致段包围盒（见 segmentRect），小区域 DU 刷新快、无大面积闪烁
    m_flushTimer = new QTimer(this);
    m_flushTimer->setTimerType(Qt::PreciseTimer);
    m_flushTimer->setInterval(10);
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

void InkItem::setInkEnabled(bool enabled)
{
    if (m_inkEnabled == enabled)
        return;
    m_inkEnabled = enabled;
    emit inkEnabledChanged();
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

bool InkItem::hasInkPixels() const
{
    for (int y = 0; y < m_img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(m_img.constScanLine(y));
        for (int x = 0; x < m_img.width(); ++x) {
            if (qAlpha(line[x]) > 0)
                return true;
        }
    }
    return false;
}

QPoint InkItem::inkStart() const
{
    // 起始位置取"最上、最左"的墨迹像素：首次落笔点（或加载笔迹后的首个墨点），
    // 用于 PageStore 判断这笔是写给哪个帖子的。
    for (int y = 0; y < m_img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(m_img.constScanLine(y));
        for (int x = 0; x < m_img.width(); ++x) {
            if (qAlpha(line[x]) > 0)
                return QPoint(x, y);
        }
    }
    return QPoint(0, 0);
}

void InkItem::onPenDown(int x, int y, int pressure)
{
    if (!m_inkEnabled)
        return;
    strokeDown(x, y, pressure, false);
}

void InkItem::onPenMove(int x, int y, int pressure)
{
    if (!m_inkEnabled)
        return;
    strokeMove(x, y, pressure, false);
}

void InkItem::onPenUp()
{
    m_stroke = false;
    // 收尾：把节流期间累积的笔迹段立即提交，不丢尾段。
    // 点状落笔（顿笔/句号/误点）一律当墨迹保留——不再区分"点"与"笔迹"，
    // 笔即书写，误点由 QML 侧的手势判定兜底
    flushInk();
}

void InkItem::onErDown(int x, int y, int pressure)
{
    if (!m_inkEnabled)
        return;
    strokeDown(x, y, pressure, true);
}

void InkItem::onErMove(int x, int y, int pressure)
{
    if (!m_inkEnabled)
        return;
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
        // 不开抗锯齿：pen 快速通道走 DU 波形，DU 是纯 1 位黑白（WBF mode 1，
        // 见 swtcon：1-bit black/white only）。抗锯齿在 16 位画布上产生的灰边
        // 是中间灰值，DU 不驱动这类 (src,tgt) 组合，显示后就是线里的白斑
        // =dash。改为整像素实心黑，且最窄 2.2px（45° 斜线下每个被线经过的
        // 像素覆盖都 >50%，二值栅格化后仍是连续黑链；1.2px 时斜线只在对角
        // 像素上有墨，显示为断续点线），斜线/任意角度都是连续实线
        m_width = inkTargetWidth(pressure);
        p.setPen(QPen(QColor(0, 0, 0), m_width, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
    }
    // 落笔点（顿笔/句号）：直径=当前笔画宽的实心圆。
    // 不依赖 drawPoint / 零长 drawLine：Qt 6.8 光栅引擎把它们当"单个 1px
    // 像素"（QCosmeticStroker::drawLine 里 start==end 直接转 drawPoints 画单
    // 点），细宽时只剩几乎不可见的 1px 点。显式椭圆稳定，且对橡皮同样适用
    //（Clear 合成模式下按覆盖清空圆形区域，颜色无关）
    const qreal d = eraser ? 26.0 : m_width;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0));
    p.drawEllipse(m_last, d / 2.0, d / 2.0);
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
        // 同 strokeDown：不开抗锯齿（DU 1 位只吃纯黑/白），实心黑保实线
        // 压感平滑：目标宽按指数滑动平均逼近，避免逐采样跳宽造成的粗细
        // 顿挫，落笔首宽直接取目标值（strokeDown 已设）
        m_width += (inkTargetWidth(pressure) - m_width) * 0.35;
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

// 笔迹段的紧致脏区：线段包围盒 + 当前笔画宽度半径（覆盖圆帽外溢）。
// 官方笔迹只刷新笔尖经过的细条区域，整块大矩形会闪得很厉害。
// 宽度按当前工具取：橡皮直径固定 26，若误用笔宽（2~6px）脏区太小，
// 橡皮边缘外的墨迹不在刷新范围内，擦不干净
QRect InkItem::segmentRect(const QPointF &a, const QPointF &b) const
{
    const qreal w = m_erase ? 26.0 : m_width;
    const int r = qMax(2, int(w / 2) + 1);
    return QRect(a.toPoint(), b.toPoint()).normalized().adjusted(-r, -r, r, r);
}

// 绕过框架对 QQuickPaintedItem 区域的"默认灰度→慢波形"路径：笔迹段直接写进
// 8-bit pen 缓冲（FB112），再走 pen 快速分支（DU + pixel_mode 7）瞬时显示。
// 关键：pixel_mode 7 把 FB112 当"墨迹掩码"——非 0 的像素才用主画布(FB96)
// 的值驱动显示，0 的像素被 gate 跳过。所以 FB112 必须填掩码（有墨=255，
// 无墨=0），而不是页面拷贝；否则页面(255)被无谓重刷→闪烁，笔迹线(0)反而
// 被 gate 掉→显示成 dash。
// 另一个硬约束：DU 波形是纯 1 位黑白，掩码内像素的画布值必须是纯黑(0)
// 或白(0xFFFF)——中间灰值（抗锯齿灰边）不在 DU 的驱动范围，显示即断点。
// 故 m_img 里的墨迹一律无抗锯齿实心黑（见 strokeDown/strokeMove）。
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
