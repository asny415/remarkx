#include "pagestore.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>

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
    setStatus("正在连接中转站…");
    fetchStatus();
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
    m_bookLabel = QString("第 %1 页 · 共 %2 页")
                      .arg(m_feedPage + 1)
                      .arg(m_totalPages);
}

void PageStore::refresh()
{
    saveInkNow();
    m_version.clear();
    goPage(0, true);
}

void PageStore::next()
{
    if (m_loading || m_downloading >= 0) {
        qInfo() << "next ignored: busy";
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
    if (m_feedPage > 0) {
        goPage(m_feedPage - 1, false);
    } else {
        qInfo() << "prev at first page: no-op";
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
    m_currentNumber = number;
    const QString base = m_bookDir + "/" + number;
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
