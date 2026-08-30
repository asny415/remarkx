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

#include <algorithm>
#include <dlfcn.h>

#include "crashctx.h"
#include "inkitem.h"
#include "telegram.h"

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
    m_favsJsonFile = baseDir + "/favs.json";
    m_calibFile = baseDir + "/calib.json";
    QDir().mkpath(m_bookDir);

    m_client = new XClient(this);
    m_client->configure(baseDir);
    m_renderer = new Renderer(this);
    m_renderer->configure(baseDir);
    m_telegram = new Telegram(this);
    m_telegram->configure(baseDir);
    // 推送成功后删除本地帖图（笔迹层保留，供翻页恢复）
    connect(m_telegram, &Telegram::sent, this, &PageStore::onFavSent);

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
    // 读取 favs.json 收藏索引（帖图 + 原始链接）
    QFile bf(m_favsJsonFile);
    if (bf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(bf.readAll()).object();
        m_favs = o["favs"].toArray();
        bf.close();
    }
    cleanupOnStartup();
    // 重启后补发上次没发完的 Telegram 收藏通知（不受 X 登录态影响）
    m_telegram->flush();

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

// 启动清理：收藏帖图（.png）与笔迹层（.draw.png）保留，其余 PNG
// （旧版整页收藏/历史缓存）删除。
void PageStore::cleanupOnStartup()
{
    QSet<QString> keep;
    for (int i = 0; i < m_favs.size(); ++i) {
        const QJsonObject e = m_favs.at(i).toObject();
        const QString num = e["number"].toString();
        keep.insert(num + ".png");
        keep.insert(num + ".draw.png");
    }
    QDir dir(m_bookDir);
    const QFileInfoList files =
        dir.entryInfoList(QStringList{"*.png"}, QDir::Files);
    int removed = 0;
    for (const QFileInfo &fi : files) {
        // 笔迹层一律保留（翻页恢复用）；收藏帖图按索引保留
        if (fi.fileName().endsWith(QLatin1String(".draw.png"))
                || keep.contains(fi.fileName()))
            continue;
        QFile::remove(fi.absoluteFilePath());
        ++removed;
    }
    if (removed)
        qInfo() << "cleanupOnStartup: removed" << removed << "obsolete pngs";
}

void PageStore::syncFeed()
{
    // 只在 feed 真正变更时重新快照，避免每次翻页都整体深拷贝
    // （QVector 的隐式共享在媒体写回/续抓后失效，此处做"按需拷贝"）
    const quint64 rev = m_client->feedRevision();
    if (rev == m_feedRev)
        return;
    m_feedRev = rev;
    m_feed = m_client->feed();
}

void PageStore::rebuildPages(bool resetPageNumbers)
{
    remarkxSetCtx("rebuildPages");
    syncFeed();
    m_pages = m_renderer->paginate(m_feed);
    m_totalPages = qMax(1, m_pages.size());
    // feed/排版已变：已渲染位图全部作废（页码可能映射到不同内容）
    m_pageCache.clear();
    m_pageCacheOrder.clear();
    // 只有整批刷新（homeReady）才清笔迹编号映射；续抓（extend）只是尾部追加，
    // 已展示页面不变，编号必须保留，否则同页反复写字出重复收藏
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
    if (m_pages.isEmpty())
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
    // 媒体就绪：重存含此推文的收藏帖图（补上先前缺失的图）
    for (int i = 0; i < m_favs.size(); ++i) {
        const QJsonObject e = m_favs.at(i).toObject();
        if (e["tweet_id"].toString() != tweetId)
            continue;
        updateFavImage(e["number"].toString(), tweetId);
    }
}

void PageStore::updateLabel()
{
    m_bookLabel = QString("第 %1 页 · 共 %2 页")
                      .arg(m_feedPage + 1)
                      .arg(m_totalPages);
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
    if (m_feedPage > 0) {
        goPage(m_feedPage - 1);
        maybePrefetchOlder();
    } else {
        // 第 1 页继续上一页：没有收藏夹了，停留原地
        qInfo() << "prev at first page";
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
    // 离开当前页：笔迹编号交给 m_pageNumbers 记录，避免后续误用旧编号
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
    const int cur = m_feedPage;
    // LRU 页面位图缓存：翻页回看直接复用位图，省掉整页 QPainter 重排
    // 渲染（一次渲染 ~100ms，缓存小页集换来明显更快的翻页手感）。
    // 头像/媒体到达时才以 force=true 重渲染当前页并更新缓存条目。
    auto touch = [&](int key) {
        m_pageCacheOrder.removeAll(key);
        m_pageCacheOrder.append(key);
    };
    auto it = m_pageCache.constFind(cur);
    if (!force && it != m_pageCache.constEnd()) {
        m_currentBase = it.value();
        touch(cur);
    } else {
        m_currentBase = m_renderer->renderPage(m_feed, m_pages, cur, false);
        m_pageCache.insert(cur, m_currentBase);
        touch(cur);
        while (m_pageCache.size() > 6) {   // 6 页 ≈ 63MB，换翻页免重渲染
            const int oldest = m_pageCacheOrder.takeFirst();
            m_pageCache.remove(oldest);
        }
    }
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
        // 只读遍历（无任何会改写 feed 的调用），用引用避免拷贝整个推文
        const XTweet &t = m_feed.at(s.tweetIndex);
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
    // 遍历当前页所有帖子块（含纯文本帖）请求媒体+头像：只遍历图片槽位会漏掉
    // 纯文本帖，导致同一用户的头像在该帖上永远不解析/不下载（即使本地已缓存）。
    for (const RenderChunk &c : m_pages.at(m_feedPage).chunks) {
        const int ti = c.tweetIndex;
        if (ti < 0 || ti >= m_feed.size())
            continue;
        // 只快照用到的 id/avatar 两个字符串（而非整条推文）：ensureMediaFor
        // 在媒体已缓存时会在调用栈内同步触发 mediaReady→onMediaReady→syncFeed，
        // 重新赋值 m_feed 会释放共享缓冲区，其后不得再触碰 feed 内存。
        const XTweet &t = m_feed.at(ti);
        if (seen.contains(t.id))
            continue;
        const QString tid = t.id;
        const QString avatar = t.avatar;
        seen.insert(tid);
        m_client->ensureMediaFor(tid);
        if (!avatar.isEmpty() && !avatar.startsWith("avatars/"))
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
    if (m_pages.isEmpty())
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

void PageStore::saveInkNow()
{
    remarkxSetCtx("saveInkNow");
    if (!m_ink || !m_ink->hasInk())
        return;
    if (!m_ink->hasInkPixels())
        return;   // 只有擦除痕迹，没有实际墨迹，不收藏
    // 页面笔迹层编号：一页一个（翻页回来恢复笔迹）
    QString pageNum = m_currentNumber;
    if (pageNum.isEmpty())
        pageNum = m_pageNumbers.value(m_feedPage);
    if (pageNum.isEmpty()) {
        pageNum = allocNumber();
        m_pageNumbers[m_feedPage] = pageNum;
        persistState();
    }
    m_currentNumber = pageNum;
    const QString base = m_bookDir + "/" + pageNum;
    // 笔迹层落盘（翻页回来恢复）
    if (!m_ink->saveDraw(base + ".draw.png"))
        return;

    // 按笔迹像素命中的帖子逐帖收藏：每帖一个独立编号，一页可收藏多条帖子。
    // 一笔划过多个帖子块（起笔跨卡/跨栏）时就收藏多次，不再一页只收一条。
    QVector<int> tis;
    hitTweets(&m_ink->inkImage(), &tis);
    if (tis.isEmpty()) {
        // 写到卡片外空白：兜底取最近块，保证总有归属
        const QPoint start = m_ink->inkStart();
        const int ti = hitTweetIndex(start.x(), start.y());
        if (ti >= 0)
            tis.append(ti);
    }
    QSet<QString> done;
    for (int i = 0; i < tis.size(); ++i) {
        const int ti = tis.at(i);
        if (ti < 0 || ti >= m_feed.size())
            continue;
        const XTweet &t = m_feed.at(ti);
        if (done.contains(t.id))
            continue;
        done.insert(t.id);
        bool fresh = false;
        const QString number = upsertFav(t, pageNum, &fresh);
        updateFavImage(number, t.id);
        if (fresh)
            m_telegram->enqueue(number, t.url);
    }
}

QString PageStore::allocNumber()
{
    const QString today = QDate::currentDate().toString("yyyyMMdd");
    if (m_date != today) {
        m_date = today;
        m_seq = 0;
    }
    m_seq += 1;
    return m_date + QString::number(m_seq).rightJustified(3, '0');
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

void PageStore::persistFavs()
{
    QFile f(m_favsJsonFile);
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject o;
        o["favs"] = m_favs;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
    }
}

// 收藏索引写回：同页同帖（feed_page + tweet_id）只保留一条收藏（更新链接，
// 保留编号/创建时间）；不同帖子、或同一帖子在不同页，各自分配独立编号。
// 返回该收藏的编号；新收藏时 *fresh=true（触发 Telegram 推送）。
QString PageStore::upsertFav(const XTweet &t, const QString &pageNum,
                             bool *fresh)
{
    if (fresh)
        *fresh = false;
    for (int i = 0; i < m_favs.size(); ++i) {
        QJsonObject e = m_favs.at(i).toObject();
        if (e["feed_page"].toInt(-1) == m_feedPage
                && e["tweet_id"].toString() == t.id) {
            e["url"] = t.url;
            m_favs.replace(i, e);
            persistFavs();
            return e["number"].toString();
        }
    }
    if (fresh)
        *fresh = true;
    const QString number = allocNumber();
    QJsonObject e;
    e["number"] = number;
    e["tweet_id"] = t.id;
    e["url"] = t.url;
    e["feed_page"] = m_feedPage;
    e["page_num"] = pageNum;
    e["created"] = QDateTime::currentDateTime()
                       .toString("yyyy-MM-dd HH:mm:ss");
    m_favs.append(e);
    persistFavs();
    return number;
}

// 用当前 feed/排版重渲染某收藏的"帖+笔迹"图（媒体到位后补图用）。
// 笔迹层按页面编号（page_num）存，多帖共用同一页的笔迹层。
void PageStore::updateFavImage(const QString &number, const QString &tweetId)
{
    int ti = -1;
    for (int i = 0; i < m_feed.size(); ++i) {
        if (m_feed.at(i).id == tweetId) {
            ti = i;
            break;
        }
    }
    if (ti < 0)
        return;
    // 该收藏所在页的笔迹层编号与页号
    QString pageNum;
    int inkPage = -1;
    for (int i = 0; i < m_favs.size(); ++i) {
        const QJsonObject e = m_favs.at(i).toObject();
        if (e["number"].toString() == number) {
            pageNum = e["page_num"].toString();
            inkPage = e["feed_page"].toInt(-1);
            break;
        }
    }
    if (pageNum.isEmpty())
        pageNum = number;   // 兼容旧版收藏：编号即页面笔迹层编号
    QImage ink;
    if (inkPage >= 0)
        ink.load(m_bookDir + "/" + pageNum + ".draw.png");
    const QImage post = m_renderer->renderFavorite(m_feed, m_pages, ti,
                                                   inkPage, ink);
    if (!post.isNull())
        post.save(m_bookDir + "/" + number + ".png", "PNG", 30);
}

// Telegram 推送成功：删除本地帖图（占空间大头）并移出索引。
// 笔迹层 .draw.png 保留——翻页回来还要恢复笔迹，且体积很小。
void PageStore::onFavSent(const QString &number)
{
    qInfo() << "fav sent, drop local image" << number;
    QFile::remove(m_bookDir + "/" + number + ".png");
    for (int i = m_favs.size() - 1; i >= 0; --i) {
        const QJsonObject e = m_favs.at(i).toObject();
        if (e["number"].toString() == number) {
            m_favs.removeAt(i);
            break;
        }
    }
    persistFavs();
}

// 笔迹像素命中的当前页所有帖子（m_feed 下标，去重、按 feed 顺序）。
// 一笔跨过多个帖子块（跨卡/跨栏）时全部命中，实现"一页收藏多条帖子"。
// 精确到像素：只有墨迹真正落在帖子块内才命中，栏间/页边空白处的长笔
// 不会误伤相邻卡片。
void PageStore::hitTweets(const QImage *ink, QVector<int> *out)
{
    remarkxSetCtx("hitTweets");
    out->clear();
    if (!ink || ink->isNull() || m_pages.isEmpty()
            || m_feedPage >= m_pages.size())
        return;
    syncFeed();
    QSet<int> seen;
    const RenderPage &pg = m_pages.at(m_feedPage);
    for (const RenderChunk &c : pg.chunks) {
        if (c.tweetIndex < 0 || c.tweetIndex >= m_feed.size())
            continue;
        if (seen.contains(c.tweetIndex))
            continue;
        const QRect r = c.rect.intersected(
            QRect(0, 0, ink->width(), ink->height()));
        if (r.isEmpty())
            continue;
        bool hasInk = false;
        for (int y = r.top(); y <= r.bottom(); ++y) {
            const QRgb *line =
                reinterpret_cast<const QRgb *>(ink->constScanLine(y));
            for (int x = r.left(); x <= r.right(); ++x) {
                if (qAlpha(line[x]) > 0) {
                    hasInk = true;
                    break;
                }
            }
            if (hasInk)
                break;
        }
        if (!hasInk)
            continue;
        seen.insert(c.tweetIndex);
        out->append(c.tweetIndex);
    }
    std::sort(out->begin(), out->end());
}

// 笔迹起始位置命中当前页的哪个帖子（m_feed 下标）。
// 写到卡片外空白时取距离最近的块，保证总有归属。
int PageStore::hitTweetIndex(int x, int y)
{
    remarkxSetCtx("hitTweetIndex");
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size())
        return -1;
    syncFeed();
    const RenderPage &pg = m_pages.at(m_feedPage);
    int best = -1;
    qint64 bestDist = Q_INT64_C(1) << 62;
    for (const RenderChunk &c : pg.chunks) {
        if (c.tweetIndex < 0 || c.tweetIndex >= m_feed.size())
            continue;
        if (c.rect.contains(x, y))
            return c.tweetIndex;
        const int dx = qMax(0, qMax(c.rect.left() - x, x - c.rect.right() - 1));
        const int dy = qMax(0, qMax(c.rect.top() - y, y - c.rect.bottom() - 1));
        const qint64 d = qint64(dx) * dx + qint64(dy) * dy;
        if (d < bestDist) {
            bestDist = d;
            best = c.tweetIndex;
        }
    }
    return best;
}
