#include "pagestore.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <cstdio>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QProcess>
#include <QQuickWindow>
#include <QTimer>

#include <dlfcn.h>

#include "crashctx.h"
#include "inkitem.h"

static const int SCREEN_W = 1404;
static const int SCREEN_H = 1872;

QImage PageImageProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);
    // image://pages/text/<tweetId>/<page>
    if (id.startsWith(QLatin1String("text/"))) {
        const QStringList parts = id.split(QLatin1Char('/'));
        if (parts.size() >= 3) {
            const QImage img = m_store->textPageImage(parts.at(1),
                                                      parts.at(2).toInt());
            if (size)
                *size = img.size();
            return img;
        }
    }
    const QImage img = m_store->currentBaseImage();
    if (size)
        *size = img.size();
    return img;
}

PageStore::PageStore(QObject *parent) : QObject(parent)
{
}

void PageStore::configure(const QString &baseDir)
{
    m_baseDir = baseDir;
    m_bookDir = baseDir + "/book";
    m_stateFile = baseDir + "/state.json";
    m_bookJsonFile = baseDir + "/book.json";
    m_calibFile = baseDir + "/calib.json";
    QDir().mkpath(m_bookDir);

    m_client = new XClient(this);
    m_client->configure(baseDir);
    m_renderer = new Renderer(this);
    m_renderer->configure(baseDir);

    connect(m_client, &XClient::homeReady, this, &PageStore::onHomeReady);
    connect(m_client, &XClient::olderReady, this, &PageStore::onOlderReady);
    connect(m_client, &XClient::errorOccurred, this,
            &PageStore::onFetchError);
    connect(m_client, &XClient::mediaReady, this, &PageStore::onMediaReady);

    // 头像下载到位后的基础页重渲染去抖：一页多次头像到达合并成一次重绘
    m_avatarTimer = new QTimer(this);
    m_avatarTimer->setSingleShot(true);
    m_avatarTimer->setInterval(300);
    connect(m_avatarTimer, &QTimer::timeout, this, [this]() {
        if (m_avatarRefreshPending) {
            m_avatarRefreshPending = false;
            renderCurrent(true);
        }
    });

    m_provider = new PageImageProvider(this);
}

void PageStore::setInk(InkItem *ink) { m_ink = ink; }
void PageStore::setWindow(QQuickWindow *window) { m_window = window; }

void PageStore::setStatus(const QString &s)
{
    if (m_status != s) {
        m_status = s;
        emit stateChanged();
    }
}

void PageStore::setCalib(const QString &file)
{
    m_calibFile = file;
}

// 页面展示完成后请求一次全屏强制刷新（等待下一帧真正画上墨水屏后再执行），
// 消除多次翻页累积的残影。通过 dlsym 调用 libqsgepaper 的
// EPFramebuffer::ghostControl(BlinkNow)，它会对全屏做一次 FullUpdate。
void PageStore::requestFullRefresh()
{
    if (!m_window || m_refreshArmed)
        return;
    m_refreshArmed = true;
    m_refreshConn = connect(m_window, &QQuickWindow::frameSwapped, this,
                            [this]() {
                                QObject::disconnect(m_refreshConn);
                                m_refreshConn = {};
                                doFullRefresh();
                            },
                            Qt::QueuedConnection);
    m_window->update();
}

void PageStore::doFullRefresh()
{
    m_refreshArmed = false;
    forceEpdFullRefresh();
}

void PageStore::forceEpdFullRefresh()
{
    void *so = dlopen("libqsgepaper.so", RTLD_NOW | RTLD_NOLOAD);
    if (!so) {
        const QStringList names = {"libqsgepaper.so", "libqsgepaper.so.6"};
        const QStringList dirs = {
            QLibraryInfo::path(QLibraryInfo::PluginsPath) + "/scenegraph",
            "/usr/lib/plugins/scenegraph",
            "/usr/lib/qt6/plugins/scenegraph",
        };
        for (const QString &name : names) {
            for (const QString &dir : dirs) {
                so = dlopen((dir + '/' + name).toUtf8().constData(),
                            RTLD_NOW | RTLD_NOLOAD);
                if (so)
                    break;
            }
            if (so)
                break;
        }
    }
    if (!so)
        return;
    using InstanceFn = void *(*)();
    using GhostControlFn = void (*)(void *, int);
    auto inst = reinterpret_cast<InstanceFn>(
        dlsym(so, "_ZN13EPFramebuffer8instanceEv"));
    auto ghost = reinterpret_cast<GhostControlFn>(
        dlsym(so, "_ZN13EPFramebuffer12ghostControlENS_16GhostControlModeE"));
    if (!inst || !ghost)
        return;
    void *fb = inst();
    if (fb)
        ghost(fb, 0); // EPFramebuffer::GhostControlMode::BlinkNow
}

void PageStore::start()
{
    remarkxSetCtx("start");
    // 读取 state.json（日期 + 当日序号）
    QFile sf(m_stateFile);
    if (sf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(sf.readAll()).object();
        m_date = o["date"].toString();
        m_seq = o["seq"].toInt();
        sf.close();
    }
    // 读取 book.json 索引
    QFile bf(m_bookJsonFile);
    if (bf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(bf.readAll()).object();
        m_entries = o["entries"].toArray();
        bf.close();
    }
    cleanupOnStartup();

    if (!m_client->hasSession()) {
        m_error = "未配置 X 登录态：请在 PC 运行安装脚本导入 Cookie 后再启动";
        emit errorChanged();
        setStatus("");
        return;
    }
    m_loading = true;
    setStatus("正在从 X 抓取最新内容…");
    m_client->start();
}

// 只有带笔迹的页面（.draw.png 存在）会连同其页面 PNG 一起保留，
// 其余缓存 PNG 全部删除（设备端渲染后，非收藏页不再落盘）。
void PageStore::cleanupOnStartup()
{
    QSet<QString> keep;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QJsonObject e = m_entries.at(i).toObject();
        if (!e["has_draw"].toBool())
            continue;
        const QString num = e["number"].toString();
        keep.insert(num + ".png");
        keep.insert(num + ".draw.png");
    }
    QDir dir(m_bookDir);
    const QFileInfoList files =
        dir.entryInfoList(QStringList{"*.png"}, QDir::Files);
    int removed = 0;
    for (const QFileInfo &fi : files) {
        if (!keep.contains(fi.fileName())) {
            QFile::remove(fi.absoluteFilePath());
            ++removed;
        }
    }
    if (removed)
        qInfo() << "cleanupOnStartup: removed" << removed << "cached pages";
}

void PageStore::syncFeed()
{
    m_feed = m_client->feed();
}

void PageStore::rebuildPages(bool resetPageNumbers)
{
    remarkxSetCtx("rebuildPages");
    syncFeed();
    m_pages = m_renderer->paginate(m_feed);
    m_totalPages = qMax(1, m_pages.size());
    m_version = QStringLiteral("s%1").arg(++m_sessionSeq);
    // 只有整批刷新（homeReady）才清收藏编号映射；续抓（extend）只是尾部追加，
    // 已展示页面不变，编号必须保留，否则同页反复收藏出重复页
    if (resetPageNumbers)
        m_pageNumbers.clear();
    updateLabel();
}

void PageStore::onHomeReady()
{
    remarkxSetCtx("onHomeReady");
    rebuildPages(true);
    m_waitingOlder = false;
    m_prefetchOlder = false;
    m_lastPrefetchEmpty = false;
    m_loading = false;
    setStatus("");
    goPage(0);
}

void PageStore::onOlderReady()
{
    remarkxSetCtx("onOlderReady");
    const int pagesBefore = m_totalPages;
    rebuildPages(false);
    m_prefetchOlder = false;
    // 页码未增长 = 没有新内容（时间线已到头），后续翻页不再空抓
    m_lastPrefetchEmpty = (m_totalPages == pagesBefore);
    m_waitingOlder = false;
    m_loading = false;
    setStatus("");
    goPage(m_feedPage);
}

void PageStore::onFetchError(const QString &msg)
{
    remarkxSetCtx("onFetchError");
    // 续抓失败也必须解锁翻页，否则 next/prev 永远被 m_waitingOlder 卡住
    m_extendErrorWas = m_waitingOlder;
    m_waitingOlder = false;
    m_loading = false;
    setStatus("");
    if (m_prefetchOlder) {
        // 后台预抓失败：静默放弃（不打扰阅读），翻到旧书尾时前台续抓兜底
        m_prefetchOlder = false;
        qWarning() << "prefetch older failed, ignored:" << msg;
        return;
    }
    m_error = msg;
    emit errorChanged();
}

void PageStore::onMediaReady(const QString &tweetId)
{
    remarkxSetCtx("onMediaReady");
    if (m_mode != FeedMode || m_pages.isEmpty())
        return;
    syncFeed();   // 确保 m_feed 与 m_pages 索引一致
    const int cur = m_feedPage;
    bool onCurrent = false;
    if (cur >= 0 && cur < m_pages.size()) {
        for (const ImageSlot &s : m_pages.at(cur).images) {
            if (s.tweetIndex < 0 || s.tweetIndex >= m_feed.size())
                continue;
            if (m_feed.at(s.tweetIndex).id == tweetId) {
                onCurrent = true;
                break;
            }
        }
    }
    if (onCurrent) {
        // 刷新槽位状态：QML overlay 换图 / 隐藏占位
        buildSlotList();
        emit imageSlotsChanged();
        // 头像到位 → 去抖后重渲染基础页
        if (m_avatarWanted.contains(tweetId)) {
            m_avatarWanted.remove(tweetId);
            for (const XTweet &t : m_feed) {
                if (t.id == tweetId && t.avatar.startsWith("avatars/")) {
                    m_avatarRefreshPending = true;
                    m_avatarTimer->start();
                    break;
                }
            }
        }
    }
    // 所有带收藏编号且含此推文的页 → 用最新图片重存合成页（补上先前缺失的图）
    for (auto it = m_pageNumbers.cbegin(); it != m_pageNumbers.cend(); ++it) {
        const int pg = it.key();
        if (pg < 0 || pg >= m_pages.size())
            continue;
        bool has = false;
        for (const ImageSlot &s : m_pages.at(pg).images) {
            if (s.tweetIndex < 0 || s.tweetIndex >= m_feed.size())
                continue;
            if (m_feed.at(s.tweetIndex).id == tweetId) {
                has = true;
                break;
            }
        }
        if (has) {
            const QString num = it.value();
            QImage full = m_renderer->renderPage(m_feed, m_pages, pg, true);
            full.save(m_bookDir + "/" + num + ".png", "PNG", 30);
        }
    }
}

void PageStore::updateLabel()
{
    if (m_mode == FavMode) {
        const int n = favCount();
        m_bookLabel = QString("收藏 %1/%2")
                          .arg(qBound(1, m_favIndex + 1, qMax(n, 1)))
                          .arg(n);
        return;
    }
    m_bookLabel = QString("第 %1 页 · 共 %2 页")
                      .arg(m_feedPage + 1)
                      .arg(m_totalPages);
}

// 收藏页 = 所有带笔迹(has_draw)的书页，按编号（时间）升序
QList<QString> PageStore::favNumbers() const
{
    QList<QString> out;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QJsonObject e = m_entries.at(i).toObject();
        if (e["has_draw"].toBool())
            out.append(e["number"].toString());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int PageStore::favCount() const
{
    return favNumbers().size();
}

void PageStore::enterFav(int index)
{
    remarkxSetCtx("enterFav");
    const QList<QString> nums = favNumbers();
    if (nums.isEmpty())
        return;
    index = qBound(0, index, nums.size() - 1);
    saveInkNow();
    m_mode = FavMode;
    m_favIndex = index;
    m_loading = false;
    m_imageSlots.clear();
    emit imageSlotsChanged();
    loadLocal(nums.at(index));
    updateLabel();
    emit stateChanged();
    // 页切换计数
    const QString key = QStringLiteral("v") + nums.at(index);
    if (m_lastDisplayKey != key) {
        m_lastDisplayKey = key;
        ++m_pageKey;
    }
}

// 删除当前收藏页：移除笔迹图与页面 PNG，从索引剔除，跳到下一张收藏
void PageStore::deleteCurrentFav()
{
    if (m_mode != FavMode)
        return;
    const QList<QString> nums = favNumbers();
    if (nums.isEmpty() || m_favIndex < 0 || m_favIndex >= nums.size())
        return;
    const QString number = nums.at(m_favIndex);
    qInfo() << "deleteFav" << number;
    // 先清笔迹与编号，避免 enterFav 里的 saveInkNow 把已删笔迹又写回
    if (m_ink)
        m_ink->clear();
    m_currentNumber.clear();
    QFile::remove(m_bookDir + "/" + number + ".draw.png");
    QFile::remove(m_bookDir + "/" + number + ".png");
    for (int i = 0; i < m_entries.size(); ++i) {
        QJsonObject e = m_entries.at(i).toObject();
        if (e["number"].toString() == number) {
            m_entries.removeAt(i);
            break;
        }
    }
    persistBook();
    if (favCount() > 0)
        enterFav(qMin(m_favIndex, favCount() - 1));
    else {
        m_mode = FeedMode;
        goPage(0);
    }
}

void PageStore::refresh()
{
    if (m_client->fetching())
        return;
    saveInkNow();
    m_loading = true;
    setStatus("正在刷新…");
    m_client->refresh();
}

void PageStore::next()
{
    remarkxSetCtx("next");
    if (m_loading || m_waitingOlder) {
        qInfo() << "next ignored: busy";
        return;
    }
    if (m_mode == FavMode) {
        if (m_favIndex + 1 < favCount()) {
            enterFav(m_favIndex + 1);
        } else {
            qInfo() << "fav last -> feed page 0";
            m_mode = FeedMode;
            goPage(0);
        }
        return;
    }
    if (m_feedPage + 1 < m_totalPages) {
        goPage(m_feedPage + 1);
        maybePrefetchOlder();
    } else {
        // 书尾：实时续抓更早内容（不做后台定时抓取）
        if (m_client->fetching()) {
            // 后台预抓在途：转前台等待，避免按钮无响应
            m_waitingOlder = true;
            m_loading = true;
            setStatus("正在加载更早内容…");
            return;
        }
        qInfo() << "next at last page -> fetchOlder";
        m_waitingOlder = true;
        m_loading = true;
        setStatus("正在加载更早内容…");
        m_client->fetchOlder();
    }
}

void PageStore::prev()
{
    remarkxSetCtx("prev");
    if (m_loading || m_waitingOlder) {
        qInfo() << "prev ignored: busy";
        return;
    }
    if (m_mode == FavMode) {
        if (m_favIndex > 0)
            enterFav(m_favIndex - 1);
        return;
    }
    if (m_feedPage > 0) {
        goPage(m_feedPage - 1);
        maybePrefetchOlder();
    } else {
        // 第 1 页继续上一页 → 进入收藏（带笔迹页）浏览
        if (favCount() > 0) {
            qInfo() << "prev at first page -> fav view";
            enterFav(favCount() - 1);
        } else {
            qInfo() << "prev at first page: no fav pages";
        }
    }
}

void PageStore::quit()
{
    saveInkNow();
    persistState();
    // 退出后拉回原生 UI，避免屏幕冻结在最后一帧
    QProcess::startDetached("systemctl", {"start", "xochitl"});
    QCoreApplication::quit();
}

void PageStore::suspendNow()
{
    saveInkNow();
    persistState();
    qInfo() << "suspending...";
    QProcess::startDetached("systemctl", {"suspend"});
}

void PageStore::menuExit(int code)
{
    QCoreApplication::exit(code);
}

void PageStore::retry()
{
    remarkxSetCtx("retry");
    if (m_error.isEmpty())
        return;
    const bool wasExtend = m_extendErrorWas;
    m_error.clear();
    emit errorChanged();
    m_extendErrorWas = false;
    if (wasExtend) {
        // 续抓失败：重试续抓（保持当前页位置，不跳回首页）
        m_waitingOlder = true;
        m_loading = true;
        setStatus("正在加载更早内容…");
        m_client->fetchOlder();
        return;
    }
    m_loading = true;
    setStatus("正在从 X 抓取最新内容…");
    m_client->start();
}

void PageStore::goPage(int n)
{
    remarkxSetCtx("goPage");
    saveInkNow();
    if (m_mode == FavMode)
        m_mode = FeedMode;
    // 离开当前页：收藏编号交给 m_pageNumbers 记录，避免后续误用旧编号
    m_currentNumber.clear();
    m_feedPage = qBound(0, n, qMax(0, m_totalPages - 1));
    m_loading = false;
    updateLabel();
    setStatus("");
    renderCurrent();
    // 页切换计数（供 QML 每 N 页强制刷新）
    const QString key = QStringLiteral("f%1").arg(m_feedPage);
    if (m_lastDisplayKey != key) {
        m_lastDisplayKey = key;
        ++m_pageKey;
        // 换页：恢复本页已存笔迹，否则清空（避免上一页笔迹叠到新页）
        if (m_ink) {
            const QString num = m_pageNumbers.value(m_feedPage);
            if (!num.isEmpty()
                    && QFile::exists(m_bookDir + "/" + num + ".draw.png"))
                m_ink->loadDraw(m_bookDir + "/" + num + ".draw.png");
            else
                m_ink->clear();
        }
    }
}

// 靠近书尾（距末页不足 2 页）时后台预抓更早内容：
// - 不必等翻到真正的末页才抓，翻页过程不被 m_waitingOlder/m_loading 阻塞；
// - 预抓完成即追加页码，用户到旧末页时已有后续内容，避免大段空白。
// 失败/到头时静默，回退到末页前台续抓的原有路径。
void PageStore::maybePrefetchOlder()
{
    remarkxSetCtx("maybePrefetchOlder");
    if (m_loading || m_waitingOlder || m_prefetchOlder)
        return;
    if (m_client->fetching())
        return;
    if (m_mode != FeedMode)
        return;
    if (m_feedPage + 2 < m_totalPages)
        return;   // 距书尾 ≥2 页：不预抓
    if (m_lastPrefetchEmpty)
        return;   // 时间线已到头：避免每次翻页都空抓
    m_prefetchOlder = true;
    qInfo() << "prefetch older at page" << (m_feedPage + 1)
            << "/" << m_totalPages;
    m_client->fetchOlder();
}

void PageStore::renderCurrent(bool force)
{
    remarkxSetCtx("renderCurrent");
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size()) {
        m_currentBase = QImage();
        return;
    }
    syncFeed();
    // 不缓存页面位图：翻页实时重建，节省内存（e-ink 渲染 ~100ms 可接受）
    m_currentBase = m_renderer->renderPage(m_feed, m_pages, m_feedPage, false);
    m_currentFile = QStringLiteral("image://pages/base?r=%1").arg(++m_baseRev);
    buildSlotList();
    requestSlotMedia();
    emit currentFileChanged();
    emit imageSlotsChanged();
    emit stateChanged();
}

void PageStore::buildSlotList()
{
    remarkxSetCtx("buildSlotList");
    QVariantList list;
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size()) {
        m_imageSlots = list;
        return;
    }
    syncFeed();
    const RenderPage &pg = m_pages.at(m_feedPage);
    for (int i = 0; i < pg.images.size(); ++i) {
        const ImageSlot &s = pg.images.at(i);
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "buildSlotList:i%d/tw%d", i, s.tweetIndex);
        remarkxSetCtx(ctx);
        if (s.tweetIndex < 0 || s.tweetIndex >= m_feed.size())
            continue;
        // 值拷贝而非引用：杜绝 t 指向被释放的 feed 缓冲区（use-after-free 防护）
        const XTweet t = m_feed.at(s.tweetIndex);
        const QVector<XMedia> *ml = s.isQuoted ? &t.quoted.media : &t.media;
        const XMedia *m = (s.mediaIndex < ml->size()) ? &(*ml).at(s.mediaIndex)
                                                      : nullptr;
        const bool ready = m && !m->path.isEmpty();
        const bool failed = m && !ready
                            && m_client->mediaFailed(t.id, s.mediaIndex,
                                                     s.isQuoted);
        QVariantMap o;
        o["x"] = s.x;
        o["y"] = s.y;
        o["w"] = s.w;
        o["h"] = s.h;
        o["index"] = i;
        o["tweetId"] = t.id;
        o["quoted"] = s.isQuoted;
        o["ready"] = ready;
        o["failed"] = failed;
        o["video"] = s.video;
        o["nMedia"] = s.nMedia;
        o["path"] = ready ? QVariant(m_client->mediaPath(m->path))
                          : QVariant();
        list.append(o);
    }
    m_imageSlots = list;
}

void PageStore::requestSlotMedia()
{
    remarkxSetCtx("requestSlotMedia");
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size())
        return;
    syncFeed();
    QSet<QString> seen;
    for (const ImageSlot &s : m_pages.at(m_feedPage).images) {
        if (s.tweetIndex < 0 || s.tweetIndex >= m_feed.size())
            continue;
        // 值拷贝而非引用：ensureMediaFor 在媒体已缓存时会在调用栈内同步触发
        // mediaReady→onMediaReady→syncFeed，重新赋值 m_feed 会释放共享缓冲区，
        // 引用随即悬垂（use-after-free，同 buildSlotList 的防护）。id 也先快照
        // 成局部值，确保调用前后都不触碰可能被释放的 feed 内存。
        const XTweet t = m_feed.at(s.tweetIndex);
        if (seen.contains(t.id))
            continue;
        const QString tid = t.id;
        seen.insert(tid);
        m_client->ensureMediaFor(tid);
        if (!t.avatar.isEmpty() && !t.avatar.startsWith("avatars/"))
            m_avatarWanted.insert(tid);
    }
}

int PageStore::hitSlot(int x, int y)
{
    for (int i = 0; i < m_imageSlots.size(); ++i) {
        const QVariantMap o = m_imageSlots.at(i).toMap();
        const int sx = o["x"].toInt(), sy = o["y"].toInt();
        const int sw = o["w"].toInt(), sh = o["h"].toInt();
        if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh)
            return i;
    }
    return -1;
}

QString PageStore::hitFullText(int x, int y)
{
    remarkxSetCtx("hitFullText");
    if (m_mode != FeedMode || m_pages.isEmpty())
        return {};
    if (m_feedPage < 0 || m_feedPage >= m_pages.size())
        return {};
    for (const TextButton &b : m_pages.at(m_feedPage).buttons) {
        if (x >= b.rect.x() && x <= b.rect.x() + b.rect.width()
                && y >= b.rect.y() && y <= b.rect.y() + b.rect.height()) {
            if (b.tweetIndex >= 0 && b.tweetIndex < m_feed.size())
                return m_feed.at(b.tweetIndex).id;
        }
    }
    return {};
}

int PageStore::fullTextPages(const QString &tweetId)
{
    remarkxSetCtx("fullTextPages");
    syncFeed();
    for (const XTweet &t : m_feed) {
        if (t.id == tweetId)
            return m_renderer->textPageCount(t);
    }
    return 1;
}

QImage PageStore::textPageImage(const QString &tweetId, int page)
{
    remarkxSetCtx("textPageImage");
    syncFeed();
    for (const XTweet &t : m_feed) {
        if (t.id == tweetId) {
            int total = 1;
            QImage img = m_renderer->renderTextPage(t, page, &total);
            if (!img.isNull())
                return img;
            break;
        }
    }
    QImage blank(1404, 1872, QImage::Format_RGB32);
    blank.fill(Qt::white);
    return blank;
}

QStringList PageStore::slotFiles(int slotIndex)
{
    remarkxSetCtx("slotFiles");
    QStringList files;
    if (slotIndex < 0 || slotIndex >= m_imageSlots.size())
        return files;
    const QVariantMap o = m_imageSlots.at(slotIndex).toMap();
    const QString tweetId = o["tweetId"].toString();
    const bool quoted = o["quoted"].toBool();
    syncFeed();
    for (const XTweet &t : m_feed) {
        if (t.id != tweetId)
            continue;
        const QVector<XMedia> &ml = quoted ? t.quoted.media : t.media;
        for (const XMedia &m : ml)
            if (!m.path.isEmpty())
                files << m_client->mediaPath(m.path);
        break;
    }
    return files;
}

void PageStore::loadLocal(const QString &number)
{
    const QString base = m_bookDir + "/" + number;
    if (!QFile::exists(base + ".png")) {
        qInfo() << "loadLocal: page missing" << number;
        m_error = "收藏页缺失：" + number;
        emit errorChanged();
        return;
    }
    m_currentNumber = number;
    m_currentFile = "file://" + base + ".png";
    m_loading = false;
    setStatus("");
    if (m_ink) {
        if (!m_ink->loadDraw(base + ".draw.png"))
            m_ink->clear();
    }
    emit currentFileChanged();
    emit stateChanged();
}

void PageStore::saveInkNow()
{
    remarkxSetCtx("saveInkNow");
    if (!m_ink || !m_ink->hasInk())
        return;
    // 优先用当前页已分配编号；收藏页（loadLocal）直接沿用 m_currentNumber
    QString number = m_currentNumber;
    if (number.isEmpty())
        number = m_pageNumbers.value(m_feedPage);
    const bool isNew = number.isEmpty();
    if (isNew) {
        if (m_mode != FeedMode)
            return;
        const QString today = QDate::currentDate().toString("yyyyMMdd");
        if (m_date != today) {
            m_date = today;
            m_seq = 0;
        }
        m_seq += 1;
        number = m_date + QString::number(m_seq).rightJustified(3, '0');
        m_pageNumbers[m_feedPage] = number;
        persistState();
    }
    m_currentNumber = number;
    const QString base = m_bookDir + "/" + number;
    if (m_mode == FeedMode) {
        // 合成完整页（文本 + 已就绪图片）落盘，供收藏浏览
        QImage full = m_renderer->renderPage(m_feed, m_pages, m_feedPage, true);
        full.save(base + ".png", "PNG", 30);
    }
    if (m_ink->saveDraw(base + ".draw.png")) {
        bool found = false;
        for (int i = 0; i < m_entries.size(); ++i) {
            QJsonObject e = m_entries.at(i).toObject();
            if (e["number"].toString() == number) {
                if (!e["has_draw"].toBool()) {
                    e["has_draw"] = true;
                    m_entries.replace(i, e);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            QJsonObject e;
            e["number"] = number;
            e["date"] = QDate::currentDate().toString("yyyy-MM-dd");
            e["feed_page"] = m_feedPage;
            e["version"] = m_version;
            e["has_draw"] = true;
            m_entries.append(e);
        }
        persistBook();
    }
}

void PageStore::persistState()
{
    QFile f(m_stateFile);
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject o;
        o["date"] = m_date;
        o["seq"] = m_seq;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void PageStore::persistBook()
{
    QFile f(m_bookJsonFile);
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject o;
        o["entries"] = m_entries;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
    }
}
