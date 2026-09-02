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
    // image://pages/detail?r=N —— 详情页基础位图
    if (id.startsWith(QLatin1String("detail"))) {
        const QImage img = m_store->detailBaseImage();
        if (size)
            *size = img.size();
        return img;
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
    connect(m_client, &XClient::detailReady, this, &PageStore::onDetailReady);
    connect(m_client, &XClient::detailFailed, this, &PageStore::onDetailFailed);

    // 头像下载到位后的基础页重渲染去抖：一页多次头像到达合并成一次重绘。
    // 详情页打开时各自去抖：feed 页在背景照常重绘（回看时头像已就位），
    // 当前详情页重绘自己。
    m_avatarTimer = new QTimer(this);
    m_avatarTimer->setSingleShot(true);
    m_avatarTimer->setInterval(300);
    connect(m_avatarTimer, &QTimer::timeout, this, [this]() {
        if (m_detailAvatarRefreshPending) {
            m_detailAvatarRefreshPending = false;
            if (!m_detail.isEmpty())
                renderDetailCurrent(true);
        }
        if (m_avatarRefreshPending) {
            m_avatarRefreshPending = false;
            renderCurrent(true);
        }
    });

    m_provider = new PageImageProvider(this);
}

void PageStore::setInk(InkItem *ink) { m_ink = ink; }
void PageStore::setWindow(QQuickWindow *window) { m_window = window; }

QString PageStore::clockText()
{
    return m_renderer ? m_renderer->nowClock() : QString();
}

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
    // 读取 favs.json 收藏索引（帖图 + 原始链接）+ 已推送的 mid 集合
    QFile bf(m_favsJsonFile);
    if (bf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(bf.readAll()).object();
        m_favs = o["favs"].toArray();
        const QJsonArray sent = o["sent"].toArray();
        for (const QJsonValue &v : sent)
            if (!v.toString().isEmpty())
                m_sentMids.insert(v.toString());
        bf.close();
    }
    // 启动去重：同一帖子只保留一条收藏（老版本按页收藏可能留下重复项），
    // 已推送过的条目清掉（帖图早被删除，属于残留），避免再补发/重复发
    QSet<QString> seenFav;
    for (int i = m_favs.size() - 1; i >= 0; --i) {
        const QString id = m_favs.at(i).toObject()["tweet_id"].toString();
        if (id.isEmpty() || seenFav.contains(id)
                || m_sentMids.contains(id))
            m_favs.removeAt(i);
        else
            seenFav.insert(id);
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
    for (const QFileInfo &fi : files) {
        // 笔迹层一律保留（翻页恢复用）；收藏帖图按索引保留
        if (fi.fileName().endsWith(QLatin1String(".draw.png"))
                || keep.contains(fi.fileName()))
            continue;
        QFile::remove(fi.absoluteFilePath());
    }
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
    // feed 重建（刷新/重试）：详情层级与回复会话已失效，整体关闭
    clearDetail();
    m_detailErrorId.clear();
    m_detailErrorFocal = XTweet();
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
    // 详情页：当前页槽位刷新 + 头像去抖重绘（feed 分支不受影响，继续执行）
    if (!m_detail.isEmpty()) {
        // 媒体路径/头像写回发生在 XClient 的容器（feed 或详情会话回复缓存），
        // lv.feed 里的副本是本地快照，必须在此刷回，否则 buildDetailSlotList
        // 读不到路径（回复图片永远占位）。各层都刷：被盖住的层也有在途下载。
        if (const XTweet *auth = m_client->findTweet(tweetId)) {
            for (DetailLevel &l : m_detail) {
                for (XTweet &t : l.feed) {
                    if (t.id == tweetId) {
                        t.media = auth->media;
                        t.quoted.media = auth->quoted.media;
                        t.avatar = auth->avatar;
                    }
                }
            }
        }
        DetailLevel &lv = m_detail.last();
        if (lv.page >= 0 && lv.page < lv.pages.size()) {
            bool onCurrent = false;
            for (const ImageSlot &s : lv.pages.at(lv.page).images) {
                if (s.tweetIndex < 0 || s.tweetIndex >= lv.feed.size())
                    continue;
                if (lv.feed.at(s.tweetIndex).id == tweetId) {
                    onCurrent = true;
                    break;
                }
            }
            if (onCurrent) {
                buildDetailSlotList();
                emit detailSlotsChanged();
                if (m_detailAvatarWanted.contains(tweetId)) {
                    m_detailAvatarWanted.remove(tweetId);
                    for (const XTweet &t : lv.feed) {
                        if (t.id == tweetId && t.avatar.startsWith("avatars/")) {
                            m_detailAvatarRefreshPending = true;
                            m_avatarTimer->start();
                            break;
                        }
                    }
                }
            }
        }
    }
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
    if (m_loading || m_waitingOlder)
        return;
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
        m_waitingOlder = true;
        m_loading = true;
        setStatus("正在加载更早内容…");
        m_client->fetchOlder();
    }
}

void PageStore::prev()
{
    remarkxSetCtx("prev");
    if (m_loading || m_waitingOlder)
        return;
    // 第 1 页继续上一页：没有收藏夹了，停留原地
    if (m_feedPage > 0) {
        goPage(m_feedPage - 1);
        maybePrefetchOlder();
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
    if (!m_detailErrorId.isEmpty()) {
        // 详情页首抓失败：重新进详情抓回复（不重抓 feed）
        const XTweet focal = m_detailErrorFocal;
        m_detailErrorId.clear();
        m_detailErrorFocal = XTweet();
        openDetailTweet(focal);
        return;
    }
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
    // 模拟阅读进度：本页展示过的推文全部上报给 X（网页端翻页时同样会把
    // 可见推文 id 以 seenTweetIds 附在 HomeTimeline 请求上，供下次刷新去重）
    if (m_feedPage >= 0 && m_feedPage < m_pages.size()) {
        QSet<int> pageTweets;
        for (const RenderChunk &c : m_pages.at(m_feedPage).chunks) {
            if (c.tweetIndex < 0 || c.tweetIndex >= m_feed.size())
                continue;
            if (pageTweets.contains(c.tweetIndex))
                continue;
            pageTweets.insert(c.tweetIndex);
            m_client->reportSeen(m_feed.at(c.tweetIndex).id);
        }
    }
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
        // feed 第一页页底画操作提示（点按帖子进详情页）
        m_currentBase = m_renderer->renderPage(m_feed, m_pages, cur, false,
                                               m_feedPage == 0);
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

QString PageStore::hitCard(int x, int y)
{
    remarkxSetCtx("hitCard");
    if (m_pages.isEmpty())
        return {};
    if (m_feedPage < 0 || m_feedPage >= m_pages.size())
        return {};
    syncFeed();
    const RenderPage &pg = m_pages.at(m_feedPage);
    // 点落在哪张卡片（chunk）内——同一卡片跨栏/跨页拆块时任一命中即可
    for (const RenderChunk &c : pg.chunks) {
        if (c.tweetIndex < 0 || c.tweetIndex >= m_feed.size())
            continue;
        if (c.rect.contains(x, y))
            return m_feed.at(c.tweetIndex).id;
    }
    return {};
}

// ---- 详情页（点按卡片打开：主帖全文 + 按热度排序的回复） ----
// 详情页是盖在基础页之上的全屏叠加层：基础页状态（页码/笔迹）完整保留，
// 返回（顶部下滑）立即恢复。主帖用全文副本（fullTextCopy），回复经
// TweetDetail（rankingMode=Relevance 即按热度）cursor 分页追加；点按某条
// 回复再入栈一层（该回复成为新主帖），层数上限 5。

// 全文副本：feed 卡片正文是截断预览（长文/长译文/note 帖的 full_text
// 只是开头摘录）；详情页展开完整文本——有译文用完整译文，否则完整原文
// （note_tweet 全文 / full_text）。引用块同理。
static XTweet fullTextCopy(const XTweet &t)
{
    XTweet c = t;
    c.text = c.translated ? c.text : c.originalText;
    c.quoted.text = c.quoted.translated ? c.quoted.text
                                        : c.quoted.originalText;
    c.isExpandable = false;
    c.quoted.isExpandable = false;
    return c;
}

void PageStore::openDetail(const QString &tweetId)
{
    remarkxSetCtx("openDetail");
    if (tweetId.isEmpty() || m_detail.size() >= 5)
        return;   // 层数上限（防无限下钻）
    // 找主帖：先在当前详情页层内（详情里点回复），再在基础页 feed
    XTweet focal;
    bool found = false;
    if (!m_detail.isEmpty()) {
        for (const XTweet &t : m_detail.last().feed) {
            if (t.id == tweetId) {
                focal = t;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        for (const XTweet &t : m_feed) {
            if (t.id == tweetId) {
                focal = t;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return;
    openDetailTweet(focal);
}

void PageStore::openDetailTweet(const XTweet &focal)
{
    remarkxSetCtx("openDetailTweet");
    DetailLevel lv;
    lv.tweetId = focal.id;
    lv.focal = fullTextCopy(focal);
    lv.feed.append(lv.focal);
    // 主帖先立即全文排版（回复稍后到达再重排追加页）
    lv.pages = m_renderer->paginate(lv.feed, true);
    m_detail.append(lv);
    renderDetailCurrent();
    updateDetailStatus();
    emit detailVisibleChanged();
    // 回复第一页（热度序）；该帖回复已有缓存时 XClient 立即发出
    m_client->fetchDetail(lv.tweetId);
}

void PageStore::clearDetail()
{
    remarkxSetCtx("clearDetail");
    m_detail.clear();
    m_detailBase = QImage();
    m_detailFile.clear();
    m_detailSlots.clear();
    m_detailAvatarWanted.clear();
    updateDetailStatus();
    emit detailFileChanged();
    emit detailSlotsChanged();
    emit detailVisibleChanged();
}

void PageStore::renderDetailCurrent(bool force)
{
    remarkxSetCtx("renderDetailCurrent");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (lv.dirty) {
        // 深层回复在背景里已追加：回到该层时补重排（整体重排最简单，
        // 追加不影响前面页内容，但页边界可能变化）
        lv.pages = m_renderer->paginate(lv.feed, true);
        lv.dirty = false;
        lv.cache.clear();
        lv.cacheOrder.clear();
        if (lv.page >= lv.pages.size())
            lv.page = qMax(0, lv.pages.size() - 1);
    }
    const int cur = lv.page;
    if (cur < 0 || cur >= lv.pages.size()) {
        m_detailBase = QImage();
        return;
    }
    auto touch = [&](int key) {
        lv.cacheOrder.removeAll(key);
        lv.cacheOrder.append(key);
    };
    auto it = lv.cache.constFind(cur);
    if (!force && it != lv.cache.end()) {
        m_detailBase = it.value();
        touch(cur);
    } else {
        m_detailBase = m_renderer->renderPage(lv.feed, lv.pages, cur, false);
        lv.cache.insert(cur, m_detailBase);
        touch(cur);
        while (lv.cache.size() > 4) {   // 4 页 ≈ 42MB，够来回翻看
            const int oldest = lv.cacheOrder.takeFirst();
            lv.cache.remove(oldest);
        }
    }
    m_detailFile = QStringLiteral("image://pages/detail?r=%1").arg(++m_detailRev);
    buildDetailSlotList();
    requestDetailSlotMedia();   // 同 renderCurrent：每次渲染都为当前页请求媒体
    emit detailFileChanged();
    emit detailSlotsChanged();
    // 换页计数（供 QML 每 N 页强制刷新）：含层号，换层同页也算换页
    const QString key = QStringLiteral("d%1/%2").arg(m_detail.size() - 1)
                                .arg(cur);
    if (m_lastDetailDisplayKey != key) {
        m_lastDetailDisplayKey = key;
        ++m_detailPageKey;
        emit detailPageKeyChanged();
    }
}

void PageStore::buildDetailSlotList()
{
    remarkxSetCtx("buildDetailSlotList");
    QVariantList list;
    if (m_detail.isEmpty()) {
        m_detailSlots = list;
        return;
    }
    DetailLevel &lv = m_detail.last();
    if (lv.page < 0 || lv.page >= lv.pages.size()) {
        m_detailSlots = list;
        return;
    }
    const RenderPage &pg = lv.pages.at(lv.page);
    for (int i = 0; i < pg.images.size(); ++i) {
        const ImageSlot &s = pg.images.at(i);
        if (s.tweetIndex < 0 || s.tweetIndex >= lv.feed.size())
            continue;
        const XTweet &t = lv.feed.at(s.tweetIndex);
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
    m_detailSlots = list;
}

void PageStore::requestDetailSlotMedia()
{
    remarkxSetCtx("requestDetailSlotMedia");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (lv.page < 0 || lv.page >= lv.pages.size())
        return;
    QSet<QString> seen;
    // 遍历当前页所有帖子块（含纯文本帖）请求媒体+头像（同 requestSlotMedia：
    // 只走图片槽位会漏掉纯文本帖的头像）
    for (const RenderChunk &c : lv.pages.at(lv.page).chunks) {
        const int ti = c.tweetIndex;
        if (ti < 0 || ti >= lv.feed.size())
            continue;
        // 只快照 id/avatar 两个字符串：ensureMediaFor 在媒体已缓存时会在
        // 调用栈内同步触发 mediaReady→onMediaReady，不得跨调用持有 feed 引用
        const XTweet &t = lv.feed.at(ti);
        if (seen.contains(t.id))
            continue;
        const QString tid = t.id;
        const QString avatar = t.avatar;
        seen.insert(tid);
        m_client->ensureMediaFor(tid);
        if (!avatar.isEmpty() && !avatar.startsWith("avatars/"))
            m_detailAvatarWanted.insert(tid);
    }
}

void PageStore::updateDetailStatus()
{
    remarkxSetCtx("updateDetailStatus");
    QString s;
    if (!m_detail.isEmpty()) {
        DetailLevel &lv = m_detail.last();
        const int total = qMax(1, lv.pages.size());
        if (!lv.initialDone)
            s = QStringLiteral("第 %1 / %2 页 · 正在加载回复…")
                    .arg(lv.page + 1).arg(total);
        else if (lv.loadingMore)
            s = QStringLiteral("第 %1 / %2 页 · 正在加载更多回复…")
                    .arg(lv.page + 1).arg(total);
        else if (lv.hasMore)
            s = QStringLiteral("第 %1 / %2 页 · 底部上滑加载更多")
                    .arg(lv.page + 1).arg(total);
        else
            s = QStringLiteral("第 %1 / %2 页 · 回复已全部加载")
                    .arg(lv.page + 1).arg(total);
    }
    if (m_detailStatus != s) {
        m_detailStatus = s;
        emit detailStatusChanged();
    }
}

void PageStore::onDetailReady(const QString &tweetId,
                              const QVector<XTweet> &fresh, bool hasMore)
{
    remarkxSetCtx("onDetailReady");
    int li = -1;
    for (int i = m_detail.size() - 1; i >= 0; --i) {
        if (m_detail.at(i).tweetId == tweetId) {
            li = i;
            break;
        }
    }
    if (li < 0)
        return;   // 该层已离开（返回过）；回复缓存仍留在 XClient
    DetailLevel &lv = m_detail[li];
    lv.initialDone = true;
    lv.loadingMore = false;
    for (const XTweet &t : fresh)
        lv.feed.append(fullTextCopy(t));
    lv.hasMore = hasMore;
    if (li == m_detail.size() - 1) {
        // 顶层：立即重排（新回复追加在尾部）
        lv.pages = m_renderer->paginate(lv.feed, true);
        lv.cache.clear();
        lv.cacheOrder.clear();
        // 翻页触发的加载更多：新回复到达后自动进下一页
        if (lv.pendingAdvance) {
            lv.pendingAdvance = false;
            lv.page = qMin(lv.page + 1, qMax(0, lv.pages.size() - 1));
        }
        renderDetailCurrent();
    } else {
        lv.dirty = true;   // 被更深层盖着：回到该层时补重排
    }
    if (m_detailErrorId == tweetId) {
        m_detailErrorId.clear();
        m_detailErrorFocal = XTweet();
    }
    updateDetailStatus();
    maybePrefetchDetail();
}

void PageStore::onDetailFailed(const QString &tweetId)
{
    remarkxSetCtx("onDetailFailed");
    int li = -1;
    for (int i = m_detail.size() - 1; i >= 0; --i) {
        if (m_detail.at(i).tweetId == tweetId) {
            li = i;
            break;
        }
    }
    if (li < 0)
        return;
    DetailLevel &lv = m_detail[li];
    lv.loadingMore = false;
    lv.pendingAdvance = false;
    if (lv.initialDone) {
        // 翻页失败：保留已加载的回复，静默停止继续加载
        lv.hasMore = false;
        updateDetailStatus();
        return;
    }
    if (li == m_detail.size() - 1) {
        // 首抓失败：弹全局错误页；"重试"重新进详情（见 retry），
        // 不重抓 feed
        m_detailErrorId = lv.tweetId;
        m_detailErrorFocal = lv.focal;
        clearDetail();
        m_error = m_client->lastError();
        emit errorChanged();
        return;
    }
    // 非顶层层首抓失败（更深层已入栈）：静默弹掉失败层
    m_detail.removeAt(li);
    renderDetailCurrent();
    updateDetailStatus();
}

void PageStore::detailNext()
{
    remarkxSetCtx("detailNext");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (lv.page + 1 < lv.pages.size()) {
        lv.page += 1;
        renderDetailCurrent();
        updateDetailStatus();
        maybePrefetchDetail();
        return;
    }
    if (!lv.hasMore)
        return;
    // 末页但还有更多：触发加载更多，回复到达后自动进下一页
    lv.pendingAdvance = true;
    if (lv.loadingMore) {
        updateDetailStatus();
        return;   // 已在加载（预抓）：等回复到达即可
    }
    lv.loadingMore = true;
    updateDetailStatus();
    m_client->fetchDetailNext(lv.tweetId);
}

void PageStore::detailPrev()
{
    remarkxSetCtx("detailPrev");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (lv.page > 0) {
        lv.page -= 1;
        renderDetailCurrent();
        updateDetailStatus();
    }
}

void PageStore::detailBack()
{
    remarkxSetCtx("detailBack");
    if (m_detail.isEmpty())
        return;
    m_detail.removeLast();
    if (m_detail.isEmpty()) {
        clearDetail();   // 回到基础页（页码/笔迹原样保留）
        return;
    }
    renderDetailCurrent();   // 新顶层可能 dirty（回复在背景里追加过）
    updateDetailStatus();
}

void PageStore::detailLoadMore()
{
    remarkxSetCtx("detailLoadMore");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (!lv.initialDone || !lv.hasMore || lv.loadingMore)
        return;
    lv.loadingMore = true;
    updateDetailStatus();
    m_client->fetchDetailNext(lv.tweetId);
}

void PageStore::maybePrefetchDetail()
{
    remarkxSetCtx("maybePrefetchDetail");
    if (m_detail.isEmpty())
        return;
    DetailLevel &lv = m_detail.last();
    if (!lv.initialDone || !lv.hasMore || lv.loadingMore)
        return;
    if (lv.page + 2 < lv.pages.size())
        return;   // 距尾部 ≥2 页：不预抓
    lv.loadingMore = true;
    updateDetailStatus();
    m_client->fetchDetailNext(lv.tweetId);
}

int PageStore::detailHitSlot(int x, int y)
{
    for (int i = 0; i < m_detailSlots.size(); ++i) {
        const QVariantMap o = m_detailSlots.at(i).toMap();
        const int sx = o["x"].toInt(), sy = o["y"].toInt();
        const int sw = o["w"].toInt(), sh = o["h"].toInt();
        if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh)
            return i;
    }
    return -1;
}

QString PageStore::detailHitCard(int x, int y)
{
    remarkxSetCtx("detailHitCard");
    if (m_detail.isEmpty())
        return {};
    DetailLevel &lv = m_detail.last();
    if (lv.page < 0 || lv.page >= lv.pages.size())
        return {};
    const RenderPage &pg = lv.pages.at(lv.page);
    for (const RenderChunk &c : pg.chunks) {
        if (c.tweetIndex < 0 || c.tweetIndex >= lv.feed.size())
            continue;
        if (c.tweetIndex == 0)
            continue;   // 主帖自身正在看，点了无反应
        if (c.rect.contains(x, y))
            return lv.feed.at(c.tweetIndex).id;
    }
    return {};
}

QStringList PageStore::detailSlotFiles(int slotIndex)
{
    remarkxSetCtx("detailSlotFiles");
    QStringList files;
    if (m_detail.isEmpty() || slotIndex < 0
            || slotIndex >= m_detailSlots.size())
        return files;
    const QVariantMap o = m_detailSlots.at(slotIndex).toMap();
    const QString tweetId = o["tweetId"].toString();
    const bool quoted = o["quoted"].toBool();
    for (const XTweet &t : m_detail.last().feed) {
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
        // 已成功推送过的帖子：不再收藏、不再发送（防翻页/刷新后重复）
        if (m_sentMids.contains(t.id))
            continue;
        bool fresh = false;
        const QString number = upsertFav(t, pageNum, &fresh);
        updateFavImage(number, t.id);
        if (fresh)
            m_telegram->enqueue(number, t.id, t.url);
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
        QJsonArray sent;
        for (const QString &id : m_sentMids)
            sent.append(id);
        o["sent"] = sent;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
    }
}

// 收藏索引写回：同一帖子（tweet_id）无论出现在哪一页，只保留一条收藏。
// 翻页/刷新/重排导致同一帖子在不同页反复出现时，都归并到首次收藏，
// 不再分配新编号、不再触发 Telegram 推送。
// 返回该收藏的编号；新收藏时 *fresh=true（触发 Telegram 推送）。
QString PageStore::upsertFav(const XTweet &t, const QString &pageNum,
                             bool *fresh)
{
    if (fresh)
        *fresh = false;
    for (int i = 0; i < m_favs.size(); ++i) {
        QJsonObject e = m_favs.at(i).toObject();
        if (e["tweet_id"].toString() == t.id) {
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

// Telegram 推送成功：记录该帖子 mid（此后不再收藏/发送），
// 删除本地帖图（占空间大头）并移出索引。
// 笔迹层 .draw.png 保留——翻页回来还要恢复笔迹，且体积很小。
void PageStore::onFavSent(const QString &number, const QString &tweetId)
{
    // 无论索引里是否还找得到该编号（老版本可能有残留/已去重），
    // 都记下 mid，保证同一个帖子此后绝不再收藏、再发送
    if (!tweetId.isEmpty())
        m_sentMids.insert(tweetId);
    for (int i = m_favs.size() - 1; i >= 0; --i) {
        const QJsonObject e = m_favs.at(i).toObject();
        if (e["number"].toString() == number) {
            m_favs.removeAt(i);
            break;
        }
    }
    QFile::remove(m_bookDir + "/" + number + ".png");
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
