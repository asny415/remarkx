#include "telegram.h"

#include <QDateTime>
#include <QFile>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

// 与 XClient 的 logReq 共用 reqdebug.log；bot token 是密钥，URL 一律打码。
static void logTelegram(const QString &baseDir, const QString &number,
                        bool ok, int status, int attempts,
                        const QByteArray &body)
{
    QFile f(baseDir + "/reqdebug.log");
    if (!f.open(QIODevice::Append))
        return;
    QTextStream ts(&f);
    ts << "\n[" << QDateTime::currentDateTime().toString("MM-dd HH:mm:ss")
       << "] telegram:sendPhoto " << (ok ? "OK" : "FAIL")
       << " status=" << status << " number=" << number
       << " attempts=" << attempts
       << "\n  url=https://api.telegram.org/bot<redacted>/sendPhoto"
       << "\n  body=" << QString::fromUtf8(body).left(300) << "\n";
    f.close();
}

Telegram::Telegram(QObject *parent) : QObject(parent)
{
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &Telegram::pump);
    // 挂起连接也要能失败重试，不能卡死队列
    m_nam.setTransferTimeout(60000);
}

void Telegram::configure(const QString &baseDir)
{
    m_baseDir = baseDir;

    QFile cf(m_baseDir + "/config.json");
    if (cf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(cf.readAll()).object();
        m_bot = o["telegram_bot"].toString().trimmed();
        m_chat = o["telegram_chat"].toString().trimmed();
        const QString proxy = o["proxy"].toString().trimmed();
        cf.close();

        if (!proxy.isEmpty()) {
            QString proxyStr = proxy;
            if (!proxyStr.contains("://"))
                proxyStr.prepend("http://");
            const QUrl pu(proxyStr);
            const QString scheme = pu.scheme().toLower();
            QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
            if (scheme == "socks5" || scheme == "socks5h")
                type = QNetworkProxy::Socks5Proxy;
            QNetworkProxy p(type, pu.host(), pu.port());
            if (!pu.userName().isEmpty())
                p.setUser(pu.userName());
            if (!pu.password().isEmpty())
                p.setPassword(pu.password());
            if (pu.host().isEmpty() || pu.port() <= 0) {
                qWarning() << "Telegram: invalid proxy" << proxy;
            } else {
                m_nam.setProxy(p);
                qInfo() << "Telegram: proxy" << scheme << pu.host() << pu.port();
            }
        }
    }

    if (!enabled()) {
        // 未配置：清掉历史遗留队列，不发送
        m_pending.clear();
        qInfo() << "Telegram: not configured";
        return;
    }
    loadQueue();
    scheduleNext();
    qInfo() << "Telegram: enabled, pending" << m_pending.size();
}

// 失败重试间隔：10s → 20s → 40s → … 上限 1 小时
qint64 Telegram::backoffMs(int attempts)
{
    const int exp = qMin(qMax(attempts, 1) - 1, 8);
    const qint64 ms = qint64(10000) * (qint64(1) << exp);
    return qMin(ms, qint64(3600) * 1000);
}

void Telegram::saveQueue()
{
    QFile f(m_baseDir + "/pending.json");
    if (!f.open(QIODevice::WriteOnly))
        return;
    QJsonArray arr;
    for (const Pending &it : m_pending) {
        QJsonObject o;
        o["number"] = it.number;
        o["tweet_id"] = it.tweetId;
        o["url"] = it.url;
        o["attempts"] = it.attempts;
        o["next_try"] = it.nextTryMs;
        arr.append(o);
    }
    QJsonObject root;
    root["queue"] = arr;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
}

// 从帖子的完整链接里取 mid（https://x.com/<handle>/status/<mid>）；
// 老版本队列只存了 url，用它做跨编号去重的兜底。
static QString midFromUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return {};
    return trimmed.mid(slash + 1);
}

void Telegram::loadQueue()
{
    m_pending.clear();
    QFile f(m_baseDir + "/pending.json");
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll())
                               .object()["queue"].toArray();
    f.close();
    QSet<QString> seenMids;   // 同一帖子只留队列里第一条，避免重复补发
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Pending p;
        p.number = o["number"].toString();
        p.tweetId = o["tweet_id"].toString();
        p.url = o["url"].toString();
        if (p.url.isEmpty())
            p.url = o["tweet_id"].toString();   // 兼容旧队列
        if (p.tweetId.isEmpty())
            p.tweetId = midFromUrl(p.url);
        p.attempts = o["attempts"].toInt();
        p.nextTryMs = qint64(o["next_try"].toDouble());
        if (p.number.isEmpty() || p.url.isEmpty())
            continue;
        if (seenMids.contains(p.tweetId))
            continue;   // 同帖重复项丢弃（取第一条）
        seenMids.insert(p.tweetId);
        m_pending.append(p);
    }
}

void Telegram::enqueue(const QString &number, const QString &tweetId,
                       const QString &url)
{
    if (!enabled())
        return;
    // 同帖（mid）无论编号如何都只发一次：翻页/刷新/重排产生的
    // 新编号（新帖图）也不再入队，避免重复消息
    for (const Pending &it : m_pending)
        if (it.number == number || it.tweetId == tweetId)
            return;
    Pending p;
    p.number = number;
    p.tweetId = tweetId;
    p.url = url;
    m_pending.append(p);
    saveQueue();
    // 后台发送：延后到事件循环，避免翻页瞬间被文件读取/组包抢占
    QMetaObject::invokeMethod(this, &Telegram::pump, Qt::QueuedConnection);
}

void Telegram::flush()
{
    if (!enabled())
        return;
    // 重启后立即可重试：清退避但保留 attempts 计数
    for (Pending &it : m_pending)
        it.nextTryMs = 0;
    pump();
}

void Telegram::pump()
{
    if (!enabled() || m_sending)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int idx = -1;
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending.at(i).nextTryMs <= now) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        scheduleNext();
        return;
    }
    sendOne(m_pending.at(idx));
}

void Telegram::sendOne(const Pending &item)
{
    m_sending = true;
    const QString full = m_baseDir + "/book/" + item.number + ".png";
    QFile f(full);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "Telegram: image missing, will retry" << full;
        logTelegram(m_baseDir, item.number, false, 0, item.attempts + 1,
                    "image missing");
        m_sending = false;
        markFailed(item.number);
        pump();   // 继续处理队列里其他就绪项
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        qWarning() << "Telegram: empty image, will retry" << full;
        logTelegram(m_baseDir, item.number, false, 0, item.attempts + 1,
                    "empty image");
        m_sending = false;
        markFailed(item.number);
        pump();   // 继续处理队列里其他就绪项
        return;
    }

    auto *mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    // Telegram sendPhoto 必需字段：chat_id 是目标聊天，缺了会 400
    QHttpPart chat;
    chat.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant(QStringLiteral("form-data; name=\"chat_id\"")));
    chat.setBody(m_chat.toUtf8());
    mp->append(chat);

    QHttpPart photo;
    photo.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QVariant(QStringLiteral("form-data; name=\"photo\"; "
                                            "filename=\"%1\"").arg(item.number + ".png")));
    photo.setHeader(QNetworkRequest::ContentTypeHeader,
                    QVariant(QStringLiteral("image/png")));
    photo.setBody(data);
    mp->append(photo);

    QHttpPart cap;
    cap.setHeader(QNetworkRequest::ContentDispositionHeader,
                  QVariant(QStringLiteral("form-data; name=\"caption\"")));
    cap.setBody(item.url.toUtf8());
    mp->append(cap);

    QNetworkRequest req(
        QUrl(QStringLiteral("https://api.telegram.org/bot%1/sendPhoto")
                 .arg(m_bot)));
    // multipart 的 Content-Type 由 QNAM 依据 boundary 自动生成
    QNetworkReply *reply = m_nam.post(req, mp);
    mp->setParent(reply);   // 生命周期绑定 reply，上传完随 reply 释放
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, number = item.number]() {
                onReplyFinished(reply, number);
            });
}

void Telegram::onReplyFinished(QNetworkReply *reply, const QString &number)
{
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    m_sending = false;
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
    int attempts = 0;
    for (const Pending &it : m_pending)
        if (it.number == number)
            attempts = it.attempts + 1;
    logTelegram(m_baseDir, number, ok, status, attempts, body);
    if (ok) {
        qInfo() << "Telegram sendPhoto ok" << number;
        QString sentId;
        for (int i = m_pending.size() - 1; i >= 0; --i) {
            if (m_pending.at(i).number == number) {
                sentId = m_pending.at(i).tweetId;
                m_pending.removeAt(i);
            }
        }
        saveQueue();
        emit sent(number, sentId);   // 已投递：让 PageStore 记 mid + 删本地图腾空间
    } else {
        qWarning() << "Telegram sendPhoto failed" << number
                   << reply->errorString() << "HTTP" << status;
        markFailed(number);
    }
    pump();
}

void Telegram::markFailed(const QString &number)
{
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending.at(i).number == number) {
            m_pending[i].attempts += 1;
            m_pending[i].nextTryMs =
                QDateTime::currentMSecsSinceEpoch()
                + backoffMs(m_pending[i].attempts);
            break;
        }
    }
    saveQueue();
    scheduleNext();
}

void Telegram::scheduleNext()
{
    m_retryTimer->stop();
    if (m_pending.isEmpty())
        return;
    qint64 next = -1;
    for (const Pending &it : m_pending)
        if (next < 0 || it.nextTryMs < next)
            next = it.nextTryMs;
    qint64 delay = next - QDateTime::currentMSecsSinceEpoch();
    if (delay < 500)
        delay = 500;
    m_retryTimer->start(int(delay));
}
