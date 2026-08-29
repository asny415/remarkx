#include "pagestore.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
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

#include "inkitem.h"

static const int SCREEN_W = 1404;
static const int SCREEN_H = 1872;

QImage PageImageProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    Q_UNUSED(id);
    Q_UNUSED(requestedSize);
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

void PageStore::rebuildPages()
{
    syncFeed();
    m_pages = m_renderer->paginate(m_feed);
    m_totalPages = qMax(1, m_pages.size());
    m_version = QStringLiteral("s%1").arg(++m_sessionSeq);
    m_pageCache.clear();
    m_cacheOrder.clear();
    m_pageNumbers.clear();
    updateLabel();
}

void PageStore::onHomeReady()
{
    rebuildPages();
    m_waitingOlder = false;
    m_loading = false;
    setStatus("");
    goPage(0);
}

void PageStore::onOlderReady()
{
    rebuildPages();
    m_loading = false;
    setStatus("");
    goPage(m_feedPage);
}

void PageStore::onFetchError(const QString &msg)
{
    m_loading = false;
    setStatus("");
    m_error = msg;
    emit errorChanged();
}

void PageStore::onMediaReady(const QString &tweetId)
{
    // 只关心当前页的推文
    bool onPage = false;
    if (m_mode == FeedMode && !m_pages.isEmpty()
            && m_feedPage < m_pages.size()) {
        for (const ImageSlot &s : m_pages.at(m_feedPage).images) {
            if (m_feed[s.tweetIndex].id == tweetId) {
                onPage = true;
                break;
            }
        }
    }
    if (!onPage)
        return;
    // 刷新槽位状态：QML overlay 换图 / 隐藏占位
    buildSlotList();
    emit imageSlotsChanged();
    // 头像到位 → 重渲染基础页
    if (m_avatarWanted.contains(tweetId)) {
        m_avatarWanted.remove(tweetId);
        syncFeed();
        for (int i = 0; i < m_feed.size(); ++i) {
            if (m_feed[i].id == tweetId && m_feed[i].avatar.startsWith("avatars/")) {
                renderCurrent(true);
                break;
            }
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
    } else {
        // 书尾：实时续抓更早内容（不做后台定时抓取）
        if (m_client->fetching())
            return;
        qInfo() << "next at last page -> fetchOlder";
        m_waitingOlder = true;
        m_loading = true;
        setStatus("正在加载更早内容…");
        m_client->fetchOlder();
    }
}

void PageStore::prev()
{
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
    if (m_error.isEmpty())
        return;
    m_error.clear();
    emit errorChanged();
    m_loading = true;
    setStatus("正在从 X 抓取最新内容…");
    m_client->start();
}

void PageStore::goPage(int n)
{
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
}

void PageStore::renderCurrent(bool force)
{
    if (m_pages.isEmpty()) {
        m_currentBase = QImage();
        return;
    }
    syncFeed();
    QImage base;
    auto it = m_pageCache.constFind(m_feedPage);
    if (!force && it != m_pageCache.constEnd()) {
        base = it.value();
    } else {
        base = m_renderer->renderPage(m_feed, m_pages, m_feedPage, false);
        insertCache(m_feedPage, base);
    }
    if (m_currentBase.size() != base.size()
            || m_currentBase.cacheKey() != base.cacheKey())
        m_currentBase = base;
    m_currentFile = QStringLiteral("image://pages/base?r=%1").arg(++m_baseRev);
    buildSlotList();
    requestSlotMedia();
    emit currentFileChanged();
    emit imageSlotsChanged();
    emit stateChanged();
}

void PageStore::insertCache(int n, const QImage &img)
{
    if (m_pageCache.contains(n)) {
        m_pageCache[n] = img;
        return;
    }
    if (m_pageCache.size() >= 8) {
        const int oldest = m_cacheOrder.takeFirst();
        m_pageCache.remove(oldest);
    }
    m_pageCache.insert(n, img);
    m_cacheOrder.append(n);
}

void PageStore::buildSlotList()
{
    QVariantList list;
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size()) {
        m_imageSlots = list;
        return;
    }
    syncFeed();
    const RenderPage &pg = m_pages.at(m_feedPage);
    for (int i = 0; i < pg.images.size(); ++i) {
        const ImageSlot &s = pg.images.at(i);
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
    if (m_pages.isEmpty() || m_feedPage >= m_pages.size())
        return;
    syncFeed();
    QSet<QString> seen;
    for (const ImageSlot &s : m_pages.at(m_feedPage).images) {
        const XTweet &t = m_feed.at(s.tweetIndex);
        if (seen.contains(t.id))
            continue;
        seen.insert(t.id);
        m_client->ensureMediaFor(t.id);
        if (!t.avatar.isEmpty() && !t.avatar.startsWith("avatars/"))
            m_avatarWanted.insert(t.id);
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

QStringList PageStore::slotFiles(int slotIndex)
{
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
        full.save(base + ".png");
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
