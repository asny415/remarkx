#include "pagestore.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QSet>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQuickWindow>
#include <QTimer>

#include <dlfcn.h>

#include "inkitem.h"

static const int SCREEN_W = 1404;
static const int SCREEN_H = 1872;

PageStore::PageStore(QObject *parent) : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1200);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        pollStatus(30);
    });
}

void PageStore::configure(const QString &relayBase, const QString &baseDir)
{
    m_relay = relayBase;
    if (m_relay.endsWith('/'))
        m_relay.chop(1);
    m_bookDir = baseDir + "/book";
    m_stateFile = baseDir + "/state.json";
    m_bookJsonFile = baseDir + "/book.json";
    m_calibFile = baseDir + "/calib.json";
    QDir().mkpath(m_bookDir);
}

void PageStore::setInk(InkItem *ink) { m_ink = ink; }
void PageStore::setWindow(QQuickWindow *window) { m_window = window; }
void PageStore::loadCalib(const QString &file) { Q_UNUSED(file); }

void PageStore::setCalib(const QString &file)
{
    m_calibFile = file;
}

void PageStore::setStatus(const QString &s)
{
    if (m_status != s) {
        m_status = s;
        emit stateChanged();
    }
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
    // Qt 以绝对路径加载场景图插件，先按插件目录定位已加载的 libqsgepaper.so
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
    qInfo() << "PageStore::start relay=" << m_relay;
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
    setStatus("正在连接中转站…");
    fetchStatus();
}

// 只有带笔迹的页面（.draw.png 存在）会连同其页面 PNG 一起保留，
// 其余缓存 PNG 全部删除，避免占用空间（需要的页面浏览时会按需重新下载）。
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

void PageStore::fetchStatus()
{
    QNetworkRequest req(QUrl(m_relay + "/api/status"));
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onStatus(reply);
    });
}

void PageStore::onStatus(QNetworkReply *reply)
{
    reply->deleteLater();
    qInfo() << "onStatus err=" << reply->error() << reply->errorString();
    if (reply->error() != QNetworkReply::NoError) {
        m_error = "无法连接中转站（" + m_relay + "）";
        emit errorChanged();
        setStatus("");
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
    m_totalPages = o["pages"].toInt(1);
    m_version = o["version"].toString();
    updateLabel();
    emit stateChanged();
    goPage(0, true);
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
    m_downloading = -1;
    m_loading = false;
    loadLocal(nums.at(index));
    updateLabel();
    emit stateChanged();
}

void PageStore::refresh()
{
    saveInkNow();
    m_version.clear();
    // 重新拉取 status：更新 totalPages / version 后由 onStatus 进入第 0 页
    fetchStatus();
}

void PageStore::next()
{
    if (m_loading || m_downloading >= 0) {
        qInfo() << "next ignored: busy";
        return;
    }
    if (m_mode == FavMode) {
        if (m_favIndex + 1 < favCount()) {
            enterFav(m_favIndex + 1);
        } else {
            qInfo() << "fav last -> feed page 0";
            m_mode = FeedMode;
            goPage(0, false);
        }
        return;
    }
    if (m_feedPage + 1 < m_totalPages) {
        goPage(m_feedPage + 1, false);
    } else {
        qInfo() << "next at last page -> extendPoll";
        extendPoll();
    }
}

void PageStore::prev()
{
    if (m_loading || m_downloading >= 0) {
        qInfo() << "prev ignored: busy";
        return;
    }
    if (m_mode == FavMode) {
        if (m_favIndex > 0)
            enterFav(m_favIndex - 1);
        return;
    }
    if (m_feedPage > 0) {
        goPage(m_feedPage - 1, false);
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

void PageStore::extendPoll()
{
    if (m_extendTarget >= 0)
        return;
    m_extendTarget = m_totalPages;
    m_loading = true;
    setStatus("已是最后一页，正在续抓更早内容…");
    emit stateChanged();
    // 请求当前最后一页（服务端会顺带触发续抓）
    QNetworkRequest req(QUrl(m_relay + "/page?p=" + QString::number(m_feedPage)));
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    m_pollTimer->start();
}

void PageStore::pollStatus(int attempts)
{
    QNetworkRequest req(QUrl(m_relay + "/api/status"));
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempts]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
            const int pages = o["pages"].toInt(m_totalPages);
            if (pages > m_totalPages && m_extendTarget >= 0) {
                m_pollTimer->stop();
                const int target = m_extendTarget;
                m_extendTarget = -1;
                m_totalPages = pages;
                setStatus("");
                goPage(target, false);
                return;
            }
        }
        if (attempts <= 0) {
            m_pollTimer->stop();
            m_extendTarget = -1;
            m_loading = false;
            setStatus("");
            emit stateChanged();
        }
    });
}

void PageStore::goPage(int n, bool force)
{
    saveInkNow();
    m_mode = FeedMode;
    m_feedPage = n;
    m_downloading = n;
    m_loading = true;
    updateLabel();
    setStatus(force ? "正在从 X 抓取最新内容…" : "正在加载…");
    emit stateChanged();
    qInfo() << "goPage" << n << "force" << force << "version" << m_version;

    int idx = entryIndex(m_version, n);
    if (idx >= 0 && !force) {
        const QJsonObject e = m_entries.at(idx).toObject();
        const QString number = e["number"].toString();
        loadLocal(number);
        return;
    }
    downloadPage(n, force);
}

int PageStore::entryIndex(const QString &version, int feedPage) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        const QJsonObject e = m_entries.at(i).toObject();
        if (e["version"].toString() == version
                && e["feed_page"].toInt() == feedPage)
            return i;
    }
    return -1;
}

void PageStore::loadLocal(const QString &number)
{
    // 缓存被启动清理掉的页面：回退为按该条目记录的 feed 页重新下载
    const QString base = m_bookDir + "/" + number;
    if (!QFile::exists(base + ".png")) {
        for (int i = 0; i < m_entries.size(); ++i) {
            const QJsonObject e = m_entries.at(i).toObject();
            if (e["number"].toString() == number) {
                qInfo() << "loadLocal: cache missing for" << number
                        << "-> re-download feed page" << e["feed_page"].toInt();
                m_mode = FeedMode;
                downloadPage(e["feed_page"].toInt(), false);
                return;
            }
        }
    }
    m_currentNumber = number;
    m_currentFile = "file://" + base + ".png";
    m_downloading = -1;
    m_loading = false;
    setStatus("");
    if (m_ink) {
        if (!m_ink->loadDraw(base + ".draw.png"))
            m_ink->clear();
    }
    emit currentFileChanged();
    emit stateChanged();
}

void PageStore::downloadPage(int n, bool force)
{
    QUrl url(m_relay + "/page?p=" + QString::number(n));
    if (force)
        url.setQuery(url.query() + "&force=1");
    QNetworkRequest req(url);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, n]() {
        onPageDownloaded(reply, n);
    });
}

void PageStore::onPageDownloaded(QNetworkReply *reply, int n)
{
    reply->deleteLater();
    qInfo() << "onPageDownloaded" << n << "err=" << reply->error();
    if (reply->error() != QNetworkReply::NoError) {
        m_loading = false;
        m_downloading = -1;
        m_error = "页面下载失败：" + reply->errorString();
        emit errorChanged();
        emit stateChanged();
        return;
    }
    const QByteArray bytes = reply->readAll();

    // 分配书页编号（YYYYMMDD + 当日序号）
    const QString today = QDate::currentDate().toString("yyyyMMdd");
    if (m_date != today) {
        m_date = today;
        m_seq = 0;
    }
    m_seq += 1;
    const QString number = m_date + QString::number(m_seq).rightJustified(3, '0');
    persistState();

    const QString pagePath = m_bookDir + "/" + number + ".png";
    QFile pf(pagePath);
    if (pf.open(QIODevice::WriteOnly)) {
        pf.write(bytes);
        pf.close();
    }

    // 记录 book.json 条目
    QJsonObject entry;
    entry["number"] = number;
    entry["date"] = QDate::currentDate().toString("yyyy-MM-dd");
    entry["feed_page"] = n;
    entry["version"] = m_version;
    entry["posts"] = QJsonArray();
    m_entries.append(entry);
    persistBook();

    m_currentNumber = number;
    m_currentFile = "file://" + pagePath;
    m_downloading = -1;
    m_loading = false;
    setStatus("");
    if (m_ink)
        m_ink->clear();
    emit currentFileChanged();
    emit stateChanged();

    // 异步取该页帖 ID 补进索引
    QNetworkRequest req(QUrl(m_relay + "/api/layout?p=" + QString::number(n)));
    QNetworkReply *lr = m_nam.get(req);
    connect(lr, &QNetworkReply::finished, this, [this, lr, number]() {
        onLayoutDownloaded(lr, number);
    });
}

void PageStore::onLayoutDownloaded(QNetworkReply *reply, const QString &number)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
        return;
    const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
    // layout 响应自带总页数：每次下载页面后校正标签，避免陈旧值卡住
    const int pages = o["pages"].toInt(-1);
    if (pages > 0 && pages != m_totalPages) {
        qInfo() << "totalPages corrected:" << m_totalPages << "->" << pages;
        m_totalPages = pages;
        updateLabel();
        emit stateChanged();
    }
    const QJsonArray cards = o["cards"].toArray();
    QJsonArray ids;
    for (const QJsonValue &c : cards)
        ids.append(c.toObject()["id"].toString());
    for (int i = 0; i < m_entries.size(); ++i) {
        QJsonObject e = m_entries.at(i).toObject();
        if (e["number"].toString() == number) {
            e["posts"] = ids;
            m_entries.replace(i, e);
            break;
        }
    }
    persistBook();
}

void PageStore::saveInkNow()
{
    if (!m_ink || !m_ink->hasInk() || m_currentNumber.isEmpty())
        return;
    const QString base = m_bookDir + "/" + m_currentNumber;
    if (m_ink->saveDraw(base + ".draw.png")) {
        // 更新 book.json 的 has_draw 标记
        for (int i = 0; i < m_entries.size(); ++i) {
            QJsonObject e = m_entries.at(i).toObject();
            if (e["number"].toString() == m_currentNumber) {
                e["has_draw"] = true;
                m_entries.replace(i, e);
                break;
            }
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
