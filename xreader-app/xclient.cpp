#include "xclient.h"
#include "crashctx.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkProxy>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

// ---- X 网页端公开常量（2026-08；queryId / Bearer 轮换时从浏览器 DevTools 重新抓包） ----
static const char *const kBearer =
    "Bearer AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs"
    "%3D1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA";
static const char *const kUA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
static const char *const kOpForYou = "wp06oo3fRGU4P1sK8rECqQ/HomeTimeline";
static const char *const kOpFollowing = "BLQWpfVqtgBqAqwRRJcJjA/HomeLatestTimeline";

static const char *const kFeatures =
    R"({"rweb_video_screen_enabled":false,"rweb_cashtags_enabled":true,)"
    R"("profile_label_improvements_pcf_label_in_post_enabled":true,)"
    R"("responsive_web_profile_redirect_enabled":true,)"
    R"("rweb_tipjar_consumption_enabled":false,"verified_phone_label_enabled":false,)"
    R"("creator_subscriptions_tweet_preview_api_enabled":true,)"
    R"("responsive_web_graphql_timeline_navigation_enabled":true,)"
    R"("premium_content_api_read_enabled":false,)"
    R"("communities_web_enable_tweet_community_results_fetch":true,)"
    R"("c9s_tweet_anatomy_moderator_badge_enabled":true,)"
    R"("responsive_web_grok_analyze_button_fetch_trends_enabled":false,)"
    R"("responsive_web_grok_analyze_post_followups_enabled":true,)"
    R"("rweb_cashtags_composer_attachment_enabled":true,)"
    R"("responsive_web_jetfuel_frame":true,)"
    R"("responsive_web_grok_share_attachment_enabled":true,)"
    R"("responsive_web_grok_annotations_enabled":true,)"
    R"("articles_preview_enabled":true,)"
    R"("responsive_web_edit_tweet_api_enabled":true,)"
    R"("rweb_conversational_replies_downvote_enabled":false,)"
    R"("graphql_is_translatable_rweb_tweet_is_translatable_enabled":true,)"
    R"("view_counts_everywhere_api_enabled":true,)"
    R"("longform_notetweets_consumption_enabled":true,)"
    R"("responsive_web_twitter_article_tweet_consumption_enabled":true,)"
    R"("content_disclosure_indicator_enabled":true,)"
    R"("content_disclosure_ai_generated_indicator_enabled":true,)"
    R"("responsive_web_grok_show_grok_translated_post":true,)"
    R"("responsive_web_grok_analysis_button_from_backend":true,)"
    R"("post_ctas_fetch_enabled":false,)"
    R"("freedom_of_speech_not_reach_fetch_enabled":true,)"
    R"("standardized_nudges_misinfo":true,)"
    R"("tweet_with_visibility_results_prefer_gql_limited_actions_policy_enabled":true,)"
    R"("longform_notetweets_rich_text_read_enabled":true,)"
    R"("longform_notetweets_inline_media_enabled":false,)"
    R"("responsive_web_grok_image_annotation_enabled":true,)"
    R"("responsive_web_grok_imagine_annotation_enabled":true,)"
    R"("responsive_web_grok_community_note_auto_translation_is_enabled":true,)"
    R"("responsive_web_enhance_cards_enabled":false})";

XClient::XClient(QObject *parent) : QObject(parent)
{
    // Qt 默认 QNetworkCookieJar 会把响应里的 set-cookie（如 lang=zh-CN）存入，
    // 之后构造请求时用 jar 里的 Cookie 整体覆盖 apiRequest() 手动设置的
    // auth_token/ct0/twid/guest_id 会话 Cookie，导致续抓请求丢失登录态被 X 拒
    // （403）。改用不存储的 jar，保证手动设置的 Cookie 头每次都原样发送。
    class NoStoreJar : public QNetworkCookieJar {
    public:
        using QNetworkCookieJar::QNetworkCookieJar;
        QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
        bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override
        {
            return false;
        }
    };
    m_nam.setCookieJar(new NoStoreJar(this));
    m_mediaNam.setCookieJar(new NoStoreJar(this));
}

void XClient::configure(const QString &baseDir)
{
    m_baseDir = baseDir;
    m_mediaDir = baseDir + "/media";
    m_cookiesFile = baseDir + "/cookies.json";
    QDir().mkpath(m_mediaDir);
    QDir().mkpath(m_mediaDir + "/avatars");

    // config.json: { "proxy": "...", "cookies": "/path" }
    QFile cf(m_baseDir + "/config.json");
    if (cf.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(cf.readAll()).object();
        m_proxy = o["proxy"].toString();
        if (o.contains("cookies"))
            m_cookiesFile = o["cookies"].toString();
        cf.close();
    }

    if (!m_proxy.isEmpty()) {
        // 容错：无协议头时按 http:// 处理（config 里可能是 "192.168.3.235:7890"）
        QString proxyStr = m_proxy.trimmed();
        if (!proxyStr.contains("://"))
            proxyStr.prepend("http://");
        QUrl pu(proxyStr);
        const QString scheme = pu.scheme().toLower();
        QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
        if (scheme == "socks5" || scheme == "socks5h")
            type = QNetworkProxy::Socks5Proxy;
        QNetworkProxy proxy(type, pu.host(), pu.port());
        if (!pu.userName().isEmpty())
            proxy.setUser(pu.userName());
        if (!pu.password().isEmpty())
            proxy.setPassword(pu.password());
        if (pu.host().isEmpty() || pu.port() <= 0) {
            qWarning() << "XClient: invalid proxy" << m_proxy
                       << "(host=" << pu.host() << "port=" << pu.port() << ")";
        } else {
            m_nam.setProxy(proxy);
            m_mediaNam.setProxy(proxy);
            qInfo() << "XClient: proxy" << scheme << pu.host() << pu.port();
        }
    }
    qInfo() << "XClient: cookies" << m_cookiesFile;
}

bool XClient::hasSession() const
{
    return QFile::exists(m_cookiesFile);
}

QString XClient::mediaPath(const QString &relative) const
{
    return relative.isEmpty() ? QString() : m_mediaDir + "/" + relative;
}

void XClient::loadSession()
{
    QFile f(m_cookiesFile);
    if (!f.open(QIODevice::ReadOnly)) {
        m_sessionError = "尚未配置 X 登录态：请在 PC 运行安装脚本导入 Cookie"
                         "（/home/root/xreader/cookies.json）";
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    QJsonObject o = doc.object();
    if (o.contains("cookies") && o["cookies"].isObject())
        o = o["cookies"].toObject();
    m_authToken = o["auth_token"].toString();
    m_ct0 = o["ct0"].toString();
    m_twid = o["twid"].toString();
    m_guestId = o["guest_id"].toString();
    m_sessionError.clear();
    if (m_authToken.isEmpty() || m_ct0.isEmpty()) {
        m_sessionError = "Cookie 缺少 auth_token/ct0，登录态无效，"
                         "请重新导入 Cookie";
    }
}

QNetworkRequest XClient::apiRequest(const QString &op,
                                    const QJsonObject &variables)
{
    QUrl url(QString("https://x.com/i/api/graphql/%1").arg(op));
    QUrlQuery q;
    q.addQueryItem("variables",
                   QString::fromUtf8(
                       QJsonDocument(variables).toJson(QJsonDocument::Compact)));
    q.addQueryItem("features", QString::fromUtf8(kFeatures));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray(kUA));
    req.setRawHeader("Authorization", QByteArray(kBearer));
    req.setRawHeader("X-Csrf-Token", m_ct0.toUtf8());
    req.setRawHeader("X-Twitter-Auth-Type", "OAuth2Session");
    req.setRawHeader("X-Twitter-Active-User", "yes");
    QStringList cookies;
    if (!m_authToken.isEmpty())
        cookies << "auth_token=" + m_authToken;
    if (!m_ct0.isEmpty())
        cookies << "ct0=" + m_ct0;
    if (!m_twid.isEmpty())
        cookies << "twid=" + m_twid;
    if (!m_guestId.isEmpty())
        cookies << "guest_id=" + m_guestId;
    req.setRawHeader("Cookie", cookies.join("; ").toUtf8());
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Referer", "https://x.com/");
    req.setRawHeader("Origin", "https://x.com");
    return req;
}

void XClient::start()
{
    refresh();
}

void XClient::refresh()
{
    remarkxSetCtx("xclient:refresh");
    if (m_fetching)
        return;
    loadSession();
    if (!m_sessionError.isEmpty()) {
        m_lastError = m_sessionError;
        emit errorOccurred(m_lastError);
        return;
    }
    m_lastError.clear();
    m_fetching = true;
    emit fetchingChanged(true);
    fetchHome();
}

void XClient::fetchHome()
{
    remarkxSetCtx("xclient:fetchHome");
    m_homePending = true;
    m_homeLeft = 2;
    m_fy.clear();
    m_fl.clear();

    QJsonObject vars;
    vars["count"] = 30;
    vars["includePromotedContent"] = true;
    vars["requestContext"] = "launch";
    vars["withCommunity"] = true;

    QNetworkReply *fy = m_nam.get(apiRequest(kOpForYou, vars));
    connect(fy, &QNetworkReply::finished, this,
            [this, fy]() { handleHomeReply("fy", fy); });

    QJsonObject varsFl = vars;
    varsFl["includePromotedContent"] = false;
    QNetworkReply *fl = m_nam.get(apiRequest(kOpFollowing, varsFl));
    connect(fl, &QNetworkReply::finished, this,
            [this, fl]() { handleHomeReply("fl", fl); });
}

// 把 QNetworkReply 错误转成可读中文（设备端错误页直接展示）
static QString replyErrorText(QNetworkReply *reply)
{
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        return "登录态失效 (HTTP " + QString::number(status)
               + ")，请重新导入 Cookie";
    if (status == 429)
        return "被限流 (429)，请稍后再试";
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
        return "无法连接（连接被拒绝）——请检查代理地址与端口";
    case QNetworkReply::RemoteHostClosedError:
        return "远端关闭连接——代理可能拒绝本设备访问";
    case QNetworkReply::HostNotFoundError:
        return "无法解析主机——检查代理地址/DNS";
    case QNetworkReply::TimeoutError:
        return "连接超时——检查代理是否可达";
    case QNetworkReply::SslHandshakeFailedError:
        return "TLS 握手失败——代理或网络拦截了连接";
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyConnectionRefusedError:
        return "代理连接失败——检查代理地址与端口";
    case QNetworkReply::ProxyNotFoundError:
        return "找不到代理主机";
    case QNetworkReply::ProxyTimeoutError:
        return "代理连接超时";
    default:
        return "网络错误：" + reply->errorString() + " (HTTP "
               + QString::number(status) + ")";
    }
}

// 请求/响应调试日志（定位 403 等：完整 URL + Cookie 状态 + 游标 + 响应体）
static void logReq(const QString &baseDir, const QString &tag,
                   const QString &which, QNetworkReply *reply,
                   const QString &auth, const QString &ct0,
                   const QString &cursor, const QByteArray &body)
{
    QFile f(baseDir + "/reqdebug.log");
    if (!f.open(QIODevice::Append))
        return;
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString ok = reply->error() == QNetworkReply::NoError
                           ? "OK" : "FAIL";
    QTextStream ts(&f);
    ts << "\n[" << QDateTime::currentDateTime().toString("MM-dd HH:mm:ss")
       << "] " << tag << ":" << which << " " << ok << " status=" << status
       << "\n  url=" << reply->url().toString()
       << "\n  request_headers:";
    const QNetworkRequest req = reply->request();
    const QList<QByteArray> rhd = req.rawHeaderList();
    for (const QByteArray &h : rhd) {
        // 打码敏感头（authorization/cookie 含会话凭据，reqdebug.log 会上传排查）
        if (h.compare("authorization", Qt::CaseInsensitive) == 0
                || h.compare("cookie", Qt::CaseInsensitive) == 0) {
            ts << "\n    " << h << ": <redacted len="
               << req.rawHeader(h).size() << ">";
            continue;
        }
        ts << "\n    " << h << ": " << req.rawHeader(h);
    }
    ts << "\n  response_headers:";
    const QList<QByteArray> phd = reply->rawHeaderList();
    for (const QByteArray &h : phd)
        ts << "\n    " << h << ": " << reply->rawHeader(h);
    ts << "\n  cookie auth_token=" << (!auth.isEmpty() ? "Y" : "N")
       << " ct0=" << (!ct0.isEmpty() ? "Y" : "N")
       << " cursor=" << cursor.left(60)
       << "\n  body=" << QString::fromUtf8(body).left(300) << "\n";
    f.close();
}

void XClient::handleHomeReply(const QString &which, QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleHomeReply");
    reply->deleteLater();
    --m_homeLeft;
    const QByteArray body = reply->readAll();
    const QString cursorForLog = (which == "fy") ? m_cursor : m_cursorFollowing;
    logReq(m_baseDir, "home", which, reply, m_authToken, m_ct0, cursorForLog,
           body);
    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = replyErrorText(reply);
        qWarning() << "XClient home error:" << which << m_lastError;
    } else {
        const QJsonObject data = QJsonDocument::fromJson(body).object();
        QVector<XTweet> items;
        QString cursor;
        parseTimeline(data, &items, &cursor);
        if (which == "fy") {
            m_fy = items;
            m_cursor = cursor;
        } else {
            m_fl = items;
            m_cursorFollowing = cursor;
        }
    }
    maybeMergeHome();
}

void XClient::maybeMergeHome()
{
    remarkxSetCtx("xclient:maybeMergeHome");
    if (!m_homePending || m_homeLeft > 0)
        return;
    m_homePending = false;
    m_fetching = false;
    emit fetchingChanged(false);
    if (!m_lastError.isEmpty()) {
        qWarning() << "XClient fetch home failed:" << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    QVector<XTweet> merged = mergeInterleave(m_fy, m_fl);
    ingest(merged, /*append=*/false);
    qInfo() << "XClient home ready:" << merged.size() << "tweets,"
            << m_tweets.size() << "in feed";
    emit homeReady();
}

void XClient::fetchOlder()
{
    remarkxSetCtx("xclient:fetchOlder");
    if (m_fetching)
        return;
    // 续抓失败冷却：短时间内不重复触发，避免疯狂重试被 X 风控持续 403
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_extendErrorAt && now - m_extendErrorAt < 10000) {
        qInfo() << "XClient: extend cooling down, skip";
        emit olderReady();   // 空批：读者停留在末页，不弹错误
        return;
    }
    loadSession();
    if (!m_sessionError.isEmpty()) {
        m_lastError = m_sessionError;
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_cursor.isEmpty() && m_cursorFollowing.isEmpty()) {
        // 无翻页游标（时间线已到尽头）：不刷新、不重建 feed——
        // 重建会让在途媒体下载的任务索引失效（saveMedia 越界）且跳回第 0 页。
        // 直接按"无更多内容"处理，读者停留在末页。
        qInfo() << "XClient: no more older content (cursor empty)";
        emit olderReady();
        return;
    }
    m_lastError.clear();
    m_fetching = true;
    emit fetchingChanged(true);
    m_olderPending = true;
    m_olderLeft = 0;
    m_oy.clear();
    m_ol.clear();
    qInfo() << "XClient fetchOlder fy_cursor=" << m_cursor.left(50)
            << "fl_cursor=" << m_cursorFollowing.left(50);

    if (!m_cursor.isEmpty()) {
        QJsonObject vars;
        vars["count"] = 30;
        vars["includePromotedContent"] = true;
        vars["requestContext"] = "launch";
        vars["withCommunity"] = true;
        vars["cursor"] = m_cursor;
        ++m_olderLeft;
        QNetworkReply *fy = m_nam.get(apiRequest(kOpForYou, vars));
        connect(fy, &QNetworkReply::finished, this,
                [this, fy]() { handleOlderReply("fy", fy); });
    }
    if (!m_cursorFollowing.isEmpty()) {
        QJsonObject vars;
        vars["count"] = 30;
        vars["includePromotedContent"] = false;
        vars["requestContext"] = "launch";
        vars["withCommunity"] = true;
        vars["cursor"] = m_cursorFollowing;
        ++m_olderLeft;
        QNetworkReply *fl = m_nam.get(apiRequest(kOpFollowing, vars));
        connect(fl, &QNetworkReply::finished, this,
                [this, fl]() { handleOlderReply("fl", fl); });
    }
    if (m_olderLeft == 0)
        maybeMergeOlder();
}

void XClient::handleOlderReply(const QString &which, QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleOlderReply");
    reply->deleteLater();
    --m_olderLeft;
    const QByteArray body = reply->readAll();
    const QString cursorUsed = (which == "fy") ? m_cursor : m_cursorFollowing;
    logReq(m_baseDir, "older", which, reply, m_authToken, m_ct0, cursorUsed,
           body);
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 403) {
            // 续抓被 403 多为风控对首次翻页请求的拦截，非登录失效。
            m_lastError = "请求被拒绝 (HTTP 403)：访问受限或会话状态异常，"
                          "请稍后重试；若持续出现请重新导入 Cookie";
        } else if (status == 401) {
            m_lastError = "登录态失效 (HTTP 401)，请重新导入 Cookie";
        } else {
            m_lastError = replyErrorText(reply);
        }
        m_extendErrorAt = QDateTime::currentMSecsSinceEpoch();
        qWarning() << "XClient older error:" << m_lastError;
    } else {
        const QJsonObject data = QJsonDocument::fromJson(body).object();
        QVector<XTweet> items;
        QString cursor;
        parseTimeline(data, &items, &cursor);
        if (which == "fy") {
            m_oy = items;
            m_cursor = cursor;
        } else {
            m_ol = items;
            m_cursorFollowing = cursor;
        }
    }
    maybeMergeOlder();
}

void XClient::maybeMergeOlder()
{
    remarkxSetCtx("xclient:maybeMergeOlder");
    if (!m_olderPending || m_olderLeft > 0)
        return;
    m_olderPending = false;
    m_fetching = false;
    emit fetchingChanged(false);
    if (!m_lastError.isEmpty()) {
        qWarning() << "XClient fetch older failed:" << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    QVector<XTweet> merged = mergeInterleave(m_oy, m_ol);
    ingest(merged, /*append=*/true);
    qInfo() << "XClient older ready:" << merged.size() << "tweets appended,"
            << m_tweets.size() << "in feed";
    emit olderReady();
}

void XClient::ingest(const QVector<XTweet> &batch, bool append)
{
    if (!append) {
        m_tweets.clear();
        m_seen.clear();
    }
    for (const XTweet &t : batch) {
        if (m_seen.contains(t.id))
            continue;
        m_seen.insert(t.id);
        m_tweets.append(t);
    }
    ++m_feedRev;
}

// 两个时间线按推文 id 去重后 1:1 交错合并，同一条只出现一次。
QVector<XTweet> XClient::mergeInterleave(const QVector<XTweet> &fy,
                                         const QVector<XTweet> &fl)
{
    QVector<XTweet> out;
    QSet<QString> seen;
    int i = 0, j = 0;
    while (i < fy.size() || j < fl.size()) {
        if (i < fy.size()) {
            if (!seen.contains(fy[i].id)) {
                seen.insert(fy[i].id);
                out.append(fy[i]);
            }
            ++i;
        }
        if (j < fl.size()) {
            if (!seen.contains(fl[j].id)) {
                seen.insert(fl[j].id);
                out.append(fl[j]);
            }
            ++j;
        }
    }
    return out;
}

// ---- 响应解析 ----

QJsonObject XClient::unwrapResult(const QJsonObject &result)
{
    if (result.isEmpty())
        return {};
    if (result["__typename"].toString() == "TweetWithVisibilityResults")
        return result["tweet"].toObject();
    if (result["__typename"].toString() == "Tweet")
        return result;
    return {};
}

QString XClient::rawText(const QJsonObject &r)
{
    QString n = noteText(r);
    if (!n.isEmpty())
        return n.trimmed();
    return r["legacy"].toObject()["full_text"].toString().trimmed();
}

QString XClient::noteText(const QJsonObject &tweet)
{
    return tweet["note_tweet"].toObject()["note_tweet_results"]
        .toObject()["result"].toObject()["text"].toString();
}

bool XClient::hasTranslation(const QJsonObject &r)
{
    const QJsonObject g =
        r["grok_translated_post_with_availability"].toObject();
    return !g["data"].toObject()["translation"].toString().trimmed().isEmpty();
}

// note_tweet.is_expandable：API 明确标记帖子还有更多内容（长文）
static bool noteExpandable(const QJsonObject &r)
{
    return r["note_tweet"].toObject()["is_expandable"].toBool();
}

// 默认显示文本：X 网页端 Grok 译文优先，否则用 legacy.full_text。
// 注意：长推文(note_tweet)的 full_text 就是卡片上显示的预览，
// 完整原文在 note_tweet.text（见 rawText），不要在这里回退到 note_tweet 全文。
QString XClient::textOf(const QJsonObject &r)
{
    const QJsonObject g =
        r["grok_translated_post_with_availability"].toObject();
    const QString t = g["data"].toObject()["translation"].toString().trimmed();
    if (!t.isEmpty())
        return t;
    return r["legacy"].toObject()["full_text"].toString().trimmed();
}

void XClient::authorInfo(const QJsonObject &r, QString *name,
                         QString *handle, QString *avatar)
{
    const QJsonObject core = r["core"].toObject()["user_results"]
                                 .toObject()["result"].toObject();
    const QJsonObject ucore = core["core"].toObject();
    const QJsonObject ulegacy = core["legacy"].toObject();
    *name = ucore["name"].toString().isEmpty()
                ? ulegacy["name"].toString() : ucore["name"].toString();
    QString h = ucore["screen_name"].toString().isEmpty()
                    ? ulegacy["screen_name"].toString()
                    : ucore["screen_name"].toString();
    while (h.startsWith('@'))
        h.remove(0, 1);
    *handle = h;
    *avatar = core["avatar"].toObject()["image_url"].toString().isEmpty()
                  ? ulegacy["profile_image_url_https"].toString()
                  : core["avatar"].toObject()["image_url"].toString();
}

XStats XClient::statsOf(const QJsonObject &r)
{
    const QJsonObject lg = r["legacy"].toObject();
    const QJsonObject views = r["views"].isObject()
                                  ? r["views"].toObject() : QJsonObject();

    auto i = [](const QJsonValue &v) {
        if (v.isDouble())
            return int(v.toDouble());
        if (v.isString())
            return v.toString().toInt();
        return 0;
    };
    XStats s;
    s.reposts = i(lg["retweet_count"]);
    s.likes = i(lg["favorite_count"]);
    s.replies = i(lg["reply_count"]);
    s.views = i(views["count"]);
    return s;
}

QVector<XMedia> XClient::mediaList(const QJsonObject &tweet)
{
    QVector<XMedia> out;
    const QJsonArray arr = tweet["legacy"].toObject()["extended_entities"]
                               .toObject()["media"].toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject m = v.toObject();
        const QString type = m["type"].toString();
        if (type != "photo" && type != "video" && type != "animated_gif")
            continue;
        const QJsonObject size = m["original_info"].toObject();
        XMedia mm;
        mm.url = m["media_url_https"].toString();
        if (mm.url.isEmpty())
            mm.url = m["media_url"].toString();
        mm.w = int(size["width"].toDouble());
        mm.h = int(size["height"].toDouble());
        mm.video = (type != "photo");
        out.append(mm);
    }
    return out;
}

XTweet *XClient::normalize(const QJsonObject &result)
{
    remarkxSetCtx("xclient:normalize");
    const QJsonObject outer = unwrapResult(result);
    if (outer.isEmpty())
        return nullptr;
    const QJsonObject legacy = outer["legacy"].toObject();

    const QJsonValue rtVal = legacy["retweeted_status_result"];
    const QJsonValue qrVal = outer.contains("quoted_status_result")
                                 ? outer["quoted_status_result"]
                                 : legacy["quoted_status_result"];
    const bool hasRt = rtVal.isObject() && !rtVal.toObject().isEmpty();
    const bool hasQuoted = qrVal.isObject() && !qrVal.toObject().isEmpty();

    QJsonObject orig;
    QJsonObject quotedSrc;
    if (hasRt) {
        orig = unwrapResult(rtVal.toObject()["result"].toObject());
        quotedSrc = orig;
    } else if (hasQuoted) {
        quotedSrc = unwrapResult(qrVal.toObject()["result"].toObject());
    }

    QString name, handle, avatar;
    authorInfo(outer, &name, &handle, &avatar);

    // 持有一条，稍后校验后交还调用方
    std::unique_ptr<XTweet> t(new XTweet);

    QString comment, mainText, mainRaw;
    QVector<XMedia> media, qMedia;
    XStats stats, qStats;
    QString qCreated;
    bool mainTranslated = false;

    if (hasRt) {
        comment.clear();
        mainText = textOf(orig);
        mainRaw = rawText(orig);
        mainTranslated = hasTranslation(orig);
        media = mediaList(orig);
        stats = statsOf(orig);
        qMedia = media;
        qStats = stats;
        qCreated = orig["legacy"].toObject()["created_at"].toString();
    } else if (!quotedSrc.isEmpty()) {
        comment = textOf(outer);
        mainText = comment;
        mainRaw = rawText(outer);
        mainTranslated = hasTranslation(outer);
        media = mediaList(outer);
        stats = statsOf(outer);
        qMedia = mediaList(quotedSrc);
        qStats = statsOf(quotedSrc);
        qCreated = quotedSrc["legacy"].toObject()["created_at"].toString();
    } else {
        comment.clear();
        mainText = textOf(outer);
        mainRaw = rawText(outer);
        mainTranslated = hasTranslation(outer);
        media = mediaList(outer);
        stats = statsOf(outer);
    }

    QString oid;
    if (hasRt) {
        oid = orig["rest_id"].toString();
        if (oid.isEmpty())
            oid = orig["legacy"].toObject()["id_str"].toString();
        if (oid.isEmpty())
            oid = outer["rest_id"].toString();
        if (oid.isEmpty())
            oid = legacy["id_str"].toString();
    } else {
        oid = outer["rest_id"].toString();
        if (oid.isEmpty())
            oid = legacy["id_str"].toString();
    }
    if (mainText.isEmpty() && comment.isEmpty())
        return nullptr;
    t->id = oid;
    t->createdAt = legacy["created_at"].toString();
    t->authorName = name;
    t->authorHandle = handle;
    t->text = mainText;
    t->originalText = mainRaw;
    t->comment = comment;
    t->isRetweet = hasRt;
    t->translated = mainTranslated;
    t->isExpandable = hasRt ? noteExpandable(orig)
                            : noteExpandable(outer);
    t->rtHandle = hasRt ? handle : QString();
    t->media = media;
    t->reposts = stats.reposts;
    t->likes = stats.likes;
    t->replies = stats.replies;
    t->views = stats.views;
    t->url = QString("https://x.com/%1/status/%2").arg(handle, oid);
    t->avatar = avatar.replace("_normal.", "_bigger.");
    if (hasRt)
        t->lang = orig["legacy"].toObject()["lang"].toString();
    else
        t->lang = legacy["lang"].toString();
    t->sourceLang = outer["grok_translated_post_with_availability"]
                        .toObject()["data"].toObject()["source_language"]
                        .toString();
    t->destLang = outer["grok_translated_post_with_availability"]
                      .toObject()["data"].toObject()["destination_language"]
                      .toString();

    if (!quotedSrc.isEmpty()) {
        QString qn, qh, qa;
        authorInfo(quotedSrc, &qn, &qh, &qa);
        t->quoted.authorName = qn;
        t->quoted.authorHandle = qh;
        t->quoted.text = textOf(quotedSrc);
        t->quoted.originalText = rawText(quotedSrc);
        t->quoted.translated = hasTranslation(quotedSrc);
        t->quoted.isExpandable = noteExpandable(quotedSrc);
        t->quoted.sourceLang =
            quotedSrc["grok_translated_post_with_availability"]
                .toObject()["data"].toObject()["source_language"].toString();
        t->quoted.createdAt = qCreated;
        t->quoted.media = qMedia;
        t->quoted.reposts = qStats.reposts;
        t->quoted.likes = qStats.likes;
        t->quoted.replies = qStats.replies;
        t->quoted.views = qStats.views;
    }
    return t.release();
}

void XClient::parseTimeline(const QJsonObject &data, QVector<XTweet> *items,
                            QString *cursor)
{
    remarkxSetCtx("xclient:parseTimeline");
    const QJsonArray instructions = data["data"].toObject()["home"]
                                        .toObject()["home_timeline_urt"]
                                        .toObject()["instructions"].toArray();
    for (const QJsonValue &insv : instructions) {
        const QJsonObject ins = insv.toObject();
        if (ins["type"].toString() != "TimelineAddEntries")
            continue;
        const QJsonArray entries = ins["entries"].toArray();
        for (const QJsonValue &ev : entries) {
            const QJsonObject entry = ev.toObject();
            const QString eid = entry["entryId"].toString();
            const QJsonObject content = entry["content"].toObject();
            if (content["entryType"].toString() == "TimelineTimelineCursor") {
                if (eid.contains("cursor-bottom"))
                    *cursor = content["value"].toString();
                continue;
            }
            if (eid.startsWith("promoted"))
                continue;  // 广告

            QList<QJsonObject> contents;
            if (content["itemContent"].isObject())
                contents.append(content["itemContent"].toObject());
            const QJsonArray inner = content["items"].toArray();
            if (contents.isEmpty() && !inner.isEmpty()) {
                for (const QJsonValue &iv : inner) {
                    const QJsonObject ic = iv.toObject()["item"]
                                               .toObject()["itemContent"]
                                               .toObject();
                    if (ic.isEmpty())
                        continue;
                    contents.append(ic);
                }
            }
            for (const QJsonObject &ic : contents) {
                if (ic["__typename"].toString() != "TimelineTweet")
                    continue;
                XTweet *t = normalize(ic["tweet_results"]
                                          .toObject()["result"].toObject());
                if (t) {
                    items->append(*t);
                    delete t;
                }
            }
        }
    }
}

bool XClient::mediaFailed(const QString &tweetId, int mediaIndex,
                          bool quoted) const
{
    const QString key = tweetId + QLatin1Char(':')
                        + (quoted ? QLatin1Char('q') : QLatin1Char('m'))
                        + QString::number(mediaIndex);
    return m_failedMedia.contains(key);
}

static QString mediaKey(const QString &tweetId, int mediaIndex, bool quoted)
{
    return tweetId + QLatin1Char(':')
           + (quoted ? QLatin1Char('q') : QLatin1Char('m'))
           + QString::number(mediaIndex);
}

// ---- 媒体下载 ----

// 缓存命中：media_dir/base.jpg 或 .png 已存在 → 把路径写回推文并返回 true
bool XClient::cacheHit(const QString &tweetId, const Job &job)
{
    remarkxSetCtx("xclient:cacheHit");
    for (int idx = 0; idx < m_tweets.size(); ++idx) {
        if (m_tweets[idx].id != tweetId)
            continue;
        QVector<XMedia> *list = job.quotedMediaIndex >= 0
                                    ? &m_tweets[idx].quoted.media
                                    : &m_tweets[idx].media;
        if (job.isAvatar) {
            const QString rel = job.base + ".jpg";
            const QString full = m_mediaDir + "/" + rel;
            if (QFile::exists(full) && QFileInfo(full).size() > 0) {
                m_tweets[idx].avatar = rel;
                ++m_feedRev;
                return true;
            }
            return false;
        }
        if (list->isEmpty())
            return false;
        // 防越界：任务快照的索引可能因 feed 重建而失效
        const int mi = job.mediaIndex >= 0 ? job.mediaIndex
                                           : job.quotedMediaIndex;
        if (mi < 0 || mi >= list->size())
            return false;
        XMedia &m = (*list)[mi];
        for (const char *ext : {".jpg", ".png"}) {
            const QString full = m_mediaDir + "/" + job.base + ext;
            if (QFile::exists(full) && QFileInfo(full).size() > 0) {
                m.path = job.base + ext;
                // 纯转推：引用块媒体同步
                if (job.quotedMediaIndex < 0
                        && m_tweets[idx].isRetweet
                        && job.mediaIndex < m_tweets[idx].quoted.media.size()) {
                    m_tweets[idx].quoted.media[job.mediaIndex].path = m.path;
                }
                ++m_feedRev;
                return true;
            }
        }
        return false;
    }
    return false;
}

void XClient::saveMedia(const QString &tweetId, const Job &job,
                        QNetworkReply *reply)
{
    remarkxSetCtx("xclient:saveMedia");
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        qWarning() << "media download failed" << job.url << status
                   << reply->errorString();
        if (!job.isAvatar)
            m_failedMedia.insert(mediaKey(tweetId, job.quotedMediaIndex >= 0
                                                          ? job.quotedMediaIndex
                                                          : job.mediaIndex,
                                          job.quotedMediaIndex >= 0));
        return;  // 失败：path 留空，渲染端保持占位图
    }
    const QByteArray data = reply->readAll();
    if (data.isEmpty())
        return;
    const QString ctype = reply->header(
        QNetworkRequest::ContentTypeHeader).toString().toLower();
    QString base = job.base;
    if (ctype.contains("png"))
        base += ".png";
    else
        base += ".jpg";
    const QString full = m_mediaDir + "/" + base;
    QFile f(full);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.close();
    } else {
        qWarning() << "cannot write media" << full;
        return;
    }
    // 把路径写回 feed 里的对应媒体
    for (int idx = 0; idx < m_tweets.size(); ++idx) {
        if (m_tweets[idx].id != tweetId)
            continue;
        if (job.isAvatar) {
            m_tweets[idx].avatar = base;
        } else {
            QVector<XMedia> *list = job.quotedMediaIndex >= 0
                                        ? &m_tweets[idx].quoted.media
                                        : &m_tweets[idx].media;
            // 防越界：任务快照的索引可能因 feed 重建而失效
            const int mi = job.mediaIndex >= 0 ? job.mediaIndex
                                               : job.quotedMediaIndex;
            if (mi >= 0 && mi < list->size())
                (*list)[mi].path = base;
            // 纯转推：原帖媒体同时是引用块媒体，路径同步过去
            if (job.quotedMediaIndex < 0 && m_tweets[idx].isRetweet
                    && job.mediaIndex < m_tweets[idx].quoted.media.size()) {
                m_tweets[idx].quoted.media[job.mediaIndex].path = base;
            }
        }
    }
    ++m_feedRev;
    if (!job.isAvatar)
        m_failedMedia.remove(mediaKey(tweetId,
                                      job.quotedMediaIndex >= 0
                                          ? job.quotedMediaIndex
                                          : job.mediaIndex,
                                      job.quotedMediaIndex >= 0));
}

void XClient::finishMedia(const QString &tweetId)
{
    m_inflightMedia.remove(tweetId);
    emit mediaReady(tweetId);
}

void XClient::ensureMediaFor(QString tweetId)
{
    remarkxSetCtx("xclient:ensureMediaFor");
    if (m_inflightMedia.contains(tweetId))
        return;
    m_inflightMedia.insert(tweetId);

    int idx = -1;
    for (int i = 0; i < m_tweets.size(); ++i) {
        if (m_tweets[i].id == tweetId) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        m_inflightMedia.remove(tweetId);
        return;
    }
    // 快照当前条目（异步期间 feed 可能被重建/追加，不能持有引用）
    const XTweet t = m_tweets.at(idx);

    QVector<Job> jobs;
    for (int i = 0; i < t.media.size(); ++i) {
        if (t.media[i].url.isEmpty() || !t.media[i].path.isEmpty())
            continue;
        jobs.append({i, -1, t.media[i].url,
                     t.id + "_" + QString::number(i), false});
    }
    // 引用块媒体：纯转推时 qMedia 与 media 同内容（C++ 是两份拷贝），
    // 只下主媒体一份，路径在 saveMedia/cacheHit 里镜像过去
    if (!t.isRetweet) {
        for (int i = 0; i < t.quoted.media.size(); ++i) {
            if (t.quoted.media[i].url.isEmpty()
                    || !t.quoted.media[i].path.isEmpty())
                continue;
            jobs.append({-1, i, t.quoted.media[i].url,
                         t.id + "_q" + QString::number(i), false});
        }
    }
    if (!t.avatar.isEmpty() && !t.avatar.startsWith("avatars/")) {
        jobs.append({-1, -1, t.avatar,
                     "avatars/" + (t.authorHandle.isEmpty()
                                       ? "unknown" : t.authorHandle),
                     true});
    }

    if (jobs.isEmpty()) {
        finishMedia(tweetId);
        return;
    }

    QDir().mkpath(m_mediaDir);
    // 成员计数器代替 shared_ptr：避免 lambda 捕获引用计数在异步回调里被破坏
    m_mediaPending[tweetId] = jobs.size();
    auto done = [this, tweetId]() {
        auto it = m_mediaPending.find(tweetId);
        if (it == m_mediaPending.end())
            return;
        if (--it.value() <= 0) {
            m_mediaPending.erase(it);
            finishMedia(tweetId);
        }
    };

    for (const Job &job : jobs) {
        if (cacheHit(tweetId, job)) {
            done();
            continue;
        }
        QNetworkRequest req(QUrl(job.url));
        req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray(kUA));
        QNetworkReply *reply = m_mediaNam.get(req);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tweetId, job, done]() {
                    reply->deleteLater();
                    saveMedia(tweetId, job, reply);
                    done();
                });
    }
}
