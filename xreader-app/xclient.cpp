#include "xclient.h"
#include "crashctx.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
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
#include <QTimeZone>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>

// 自定义请求属性码：发送时刻（本地毫秒 epoch）。QNetworkReply 持有请求副本，
// 响应回调里经 reply->request().attribute() 读回（时钟校准用，Qt 6 自定义
// 属性码必须落在 User=1000 起的区间）
static const QNetworkRequest::Attribute kSentAtAttr =
        static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 1);

// ---- xdiag 请求/响应诊断日志（排查"整页 %22 数字"问题，2026-09 用户要求） ----
// 每个 API 请求写一行 REQ 摘要（op/URL 长度/seen 数/cursor 长度），每个响应
// 写一行 RES 摘要（状态码/错误/正文长度）；出现异常（非 200、JSON 解析失败、
// 顶层 errors 数组、正文被判定为编码数据的"帖子"）追加 ANOM 行带正文片段。
// 日志写到 <baseDir>/xdiag.log（configure() 设置路径），超过 2MB 删档重开
// （设备 flash 有限）。只记请求参数与响应摘要/片段，不含 Cookie/token。
static QString s_diagPath;

// 正文片段：截前 n 字节并压平换行（日志单行）
static QString diagHead(const QByteArray &body, int n)
{
    return QString::fromUtf8(body.left(n))
            .replace(QLatin1Char('\n'), QLatin1Char(' '))
            .replace(QLatin1Char('\r'), QLatin1Char(' '));
}

static void diagWrite(const QString &line)
{
    if (s_diagPath.isEmpty())
        return;
    QFileInfo fi(s_diagPath);
    if (fi.exists() && fi.size() > 2 * 1024 * 1024)
        QFile::remove(s_diagPath);
    QFile f(s_diagPath);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
             + QLatin1Char(' ') + line + QLatin1Char('\n')).toUtf8());
    f.close();
}

static void diagRequest(const QString &op, const QNetworkRequest &req,
                        int seen, const QString &cursor)
{
    diagWrite(QString("REQ op=%1 urlLen=%2 seen=%3 cursorLen=%4")
                .arg(op).arg(req.url().toString().size())
                .arg(seen).arg(cursor.size()));
}

static void diagReply(const QString &op, QNetworkReply *reply,
                      const QByteArray &body)
{
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    diagWrite(QString("RES op=%1 status=%2 err=%3 len=%4")
                .arg(op).arg(status).arg(reply->errorString())
                .arg(body.size()));
    if (status != 200 || reply->error() != QNetworkReply::NoError)
        diagWrite(QString("ANOM op=%1 kind=http status=%2 err=%3 head=%4")
                    .arg(op).arg(status).arg(reply->errorString())
                    .arg(diagHead(body, 512)));
}

// 响应 JSON 顶层 errors 数组（X 风控/参数校验失败时 HTTP 200 也带）：
// 返回第一个错误的 message（空数组/无 errors 返回空串）
static QString topLevelError(const QJsonObject &data)
{
    const QJsonArray errs = data["errors"].toArray();
    if (errs.isEmpty())
        return {};
    return errs.first().toObject()["message"].toString();
}

// "数据帖"识别与过滤（定义在文件后部响应解析区，此处前置声明）
static bool isDataGarbage(const QString &s);
static void filterDataGarbage(const QString &diagTag, QVector<XTweet> *items);

// ---- X 网页端公开常量（2026-08；queryId / Bearer 轮换时从浏览器 DevTools 重新抓包） ----
static const char *const kBearer =
    "Bearer AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs"
    "%3D1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA";
static const char *const kUA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
static const char *const kOpForYou = "wp06oo3fRGU4P1sK8rECqQ/HomeTimeline";
static const char *const kOpFollowing = "BLQWpfVqtgBqAqwRRJcJjA/HomeLatestTimeline";
static const char *const kOpTweetDetail = "XMOz5h24KAZ86qKffKTLdQ/TweetDetail";
// 首页标签栏：置顶文件夹（X 列表）列表。响应 data.pinned_timelines
// .pinned_timelines[]，每项 {__typename:"ListPinnedTimeline", list:{id,id_str,name,…}}，
// 顺序即网页版首页标签顺序（"为你推荐/正在关注"之后的部分）
static const char *const kOpPinned = "-cgbxu1bOQbapAA31bo1mA/PinnedTimelines";
// 文件夹（列表）时间线：variables {listId(数字 id_str), count, cursor?}，
// 响应 data.list.tweets_timeline.timeline.instructions（条目结构与
// HomeTimeline 一致：TimelineTimelineItem 直接 itemContent +
// list-conversation-* 模块内 items + cursor-top/bottom）
static const char *const kOpList = "1LE3u14FJjPZUHKFGzos2g/ListLatestTweetsTimeline";
// TweetDetail 专用 fieldToggles（2026-09 抓包提取，与网页端请求一致）
static const char *const kFieldToggles =
    R"({"withArticleRichContentState":true,"withArticlePlainText":false,)"
    R"("withArticleSummaryText":true,"withArticleVoiceOver":true,)"
    R"("withGrokAnalyze":false,"withDisallowedReplyControls":false})";

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
    s_diagPath = baseDir + "/xdiag.log";   // 请求/响应诊断日志（见文件头部说明）
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
        }
    }
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
                                    const QJsonObject &variables,
                                    const QString &fieldToggles)
{
    QUrl url(QString("https://x.com/i/api/graphql/%1").arg(op));
    QUrlQuery q;
    q.addQueryItem("variables",
                   QString::fromUtf8(
                       QJsonDocument(variables).toJson(QJsonDocument::Compact)));
    q.addQueryItem("features", QString::fromUtf8(kFeatures));
    if (!fieldToggles.isEmpty())
        q.addQueryItem("fieldToggles", fieldToggles);
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
    // 传输超时：代理挂起/无数据时 30s 必触发 finished，
    // 否则 m_fetching 永久卡死 → 之后所有刷新/续抓静默失效（只能退出）
    req.setTransferTimeout(30000);
    // 发送时刻（本地毫秒 epoch，自定义属性随请求带入响应）：与响应 Date 头
    // 配合估算服务器时钟偏移（时钟校准，见 syncTimeFromReply）
    req.setAttribute(kSentAtAttr, qint64(QDateTime::currentMSecsSinceEpoch()));
    return req;
}

// ---- 时钟校准 ----
// X API 响应的 HTTP Date 头带服务器发送时刻（RFC 1123，秒精度；X 服务器
// NTP 授时）。发送/接收时刻取中点做 SNTP 式估算：
//   offset = 服务器时刻 − (tSent + tRecv) / 2   （正=本地落后）
// 漂移 >2s 且本进程有权限（设备端以 root 运行）时直接修正系统时钟
// （clock_settime），右上角时钟/收藏时间戳/Telegram 退避等全部随之正确；
// 无权限则保留 offset，渲染端用它做显示补偿（见 Renderer::setTimeOffset）。
// 每次 API 响应都重估：会话期间设备时钟保持准确，且被人为改错后能自愈。
// 解析 HTTP Date 头（RFC 1123）："ddd, dd MMM yyyy HH:mm:ss zzz"。
// Qt 的 RFC2822Date 解析不认 GMT 这类时区缩写（实测 X 响应恒为 GMT），
// 故手动归一化：拆出时区部分、剩余按 UTC 解析；数字偏移 ±HHMM 换算回 UTC；
// 其他缩写（EST…）宁缺勿错，返回无效
static QDateTime parseHttpDate(const QByteArray &raw)
{
    const QString s = QString::fromLatin1(raw).trimmed();
    const int sp = s.lastIndexOf(QLatin1Char(' '));
    if (sp < 0)
        return {};
    const QString body = s.left(sp);
    const QString zone = s.mid(sp + 1);
    QDateTime dt = QDateTime::fromString(
            body, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"));
    if (!dt.isValid())
        return {};
    dt.setTimeZone(QTimeZone::UTC);
    if (zone.size() == 5 && (zone[0] == QLatin1Char('+')
                             || zone[0] == QLatin1Char('-'))) {
        const int sign = zone[0] == QLatin1Char('-') ? -1 : 1;
        const int hh = zone.mid(1, 2).toInt();
        const int mm = zone.mid(3, 2).toInt();
        dt = dt.addSecs(-sign * (hh * 3600 + mm * 60));
    } else if (!zone.isEmpty() && zone != QLatin1String("GMT")
               && zone != QLatin1String("UTC")
               && zone != QLatin1Char('Z')) {
        return {};
    }
    return dt;
}

void XClient::syncTimeFromReply(QNetworkReply *reply)
{
    const QDateTime server = parseHttpDate(reply->rawHeader("Date"));
    if (!server.isValid())
        return;
    const qint64 tSent = reply->request().attribute(kSentAtAttr).toLongLong();
    const qint64 tRecv = QDateTime::currentMSecsSinceEpoch();
    // 无发送时刻（理论上不会，属性缺失兜底）：偏移被网络耗时低估，只做显示
    // 补偿，不用于修正系统时钟
    const qint64 offset = server.toMSecsSinceEpoch()
                          - (tSent > 0 ? (tSent + tRecv) / 2 : tRecv);
    if (qAbs(offset) < 2000) {
        if (m_timeOffsetMs != 0) {   // 此前无权限用偏移补偿，如今时钟已对上
            m_timeOffsetMs = 0;
            emit timeSynced(0, false);
        }
        return;
    }
    const qint64 now = server.toMSecsSinceEpoch() + (tRecv - tSent) / 2;
    const qint64 tNow = QDateTime::currentMSecsSinceEpoch();
    if (tSent > 0
            && (m_lastClockAttemptMs == 0
                || tNow - m_lastClockAttemptMs >= 300000)) {
        m_lastClockAttemptMs = tNow;
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(now / 1000);
        ts.tv_nsec = static_cast<long>((now % 1000) * 1000000LL);
        if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
            m_timeOffsetMs = 0;   // 系统时钟已修正：显示无需补偿
            qWarning() << "XClient: system clock corrected, offset was"
                       << offset / 60000 << "min";
            emit timeSynced(0, true);
            return;
        }
        qWarning() << "XClient: clock_settime failed:"
                   << strerror(errno);
    }
    // 无权限修正系统时钟（或处于尝试冷却期）：偏移交给渲染端显示补偿
    if (m_timeOffsetMs != offset) {
        m_timeOffsetMs = offset;
        emit timeSynced(offset, false);
    }
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
    case QNetworkReply::OperationCanceledError:
        return "请求超时，请重试";
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

// ---- 首页标签（文件夹）列表 ----

void XClient::setTab(const QString &tabId)
{
    if (tabId.isEmpty())
        return;
    m_tabId = tabId;
    m_tabKind = (tabId == QLatin1String("fy")
                    || tabId == QLatin1String("fl"))
                    ? QStringLiteral("home") : QStringLiteral("list");
    m_cursor.clear();   // 换标签：旧游标作废
}

QString XClient::currentTabName() const
{
    for (const XTab &t : m_tabs)
        if (t.id == m_tabId)
            return t.name;
    // 列表里找不到（如文件夹已被网页端删除）：固定标签兜底，其余回退 id
    if (m_tabId == QLatin1String("fy"))
        return QStringLiteral("为你推荐");
    if (m_tabId == QLatin1String("fl"))
        return QStringLiteral("正在关注");
    return m_tabId;
}

void XClient::fetchFolders()
{
    remarkxSetCtx("xclient:fetchFolders");
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
    QNetworkRequest req = apiRequest(kOpPinned, QJsonObject());
    diagRequest(QStringLiteral("folders"), req, 0, QString());
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { handleFoldersReply(reply); });
}

void XClient::handleFoldersReply(QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleFoldersReply");
    syncTimeFromReply(reply);
    reply->deleteLater();
    m_fetching = false;
    emit fetchingChanged(false);
    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = replyErrorText(reply);
        qWarning() << "XClient folders error:" << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    const QByteArray body = reply->readAll();
    diagReply(QStringLiteral("folders"), reply, body);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull())
        diagWrite(QStringLiteral("ANOM op=folders kind=badjson head=")
                  + diagHead(body, 512));
    const QJsonObject data = doc.object();
    const QJsonArray arr = data["data"].toObject()["pinned_timelines"]
                                .toObject()["pinned_timelines"].toArray();
    QVector<XTab> tabs;
    // 前两个固定（与网页版一致）；文件夹抓取失败时调用方仍可用它们
    tabs.append({"fy", QStringLiteral("为你推荐"), true});
    tabs.append({"fl", QStringLiteral("正在关注"), true});
    QSet<QString> seen;
    for (const QJsonValue &v : arr) {
        const QJsonObject pt = v.toObject();
        // 防御：只认列表类型的置顶时间线（未来若有其他类型直接跳过）
        if (pt["__typename"].toString() != QLatin1String("ListPinnedTimeline"))
            continue;
        const QJsonObject list = pt["list"].toObject();
        const QString id = list["id_str"].toString();
        const QString name = list["name"].toString().trimmed();
        if (id.isEmpty() || name.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        tabs.append({id, name, false});
    }
    m_tabs = tabs;
    emit foldersReady();
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
    m_details.clear();   // 刷新后回复按新 feed 重新抓取
    fetchHome();
}

void XClient::reportSeen(const QString &tweetId)
{
    if (tweetId.isEmpty())
        return;
    // 网页端同款：不查重、按展示顺序追加（抓包确认 seenTweetIds 里会有重复 id）
    m_seenTweetIds.append(tweetId);
    // 限长：请求是 GET 且 variables 整个放在 URL query 里，每个 id 编码后
    // ~28 字节。上限 1000 时请求行可涨到 ~30KB，超出部分代理/边缘节点的
    // 请求行限制（如 nginx 默认 8KB 的 large_client_header_buffers、
    // Cloudflare 的 URL 上限）会被直接拒绝——这是翻页/续抓"有一定概率"
    // 失败的诱因之一。网页端也只上送最近一批已读 id，保留最近 100 个
    // （请求行 ~6KB，同时减弱"我一直在读这些"的推荐信号）
    const int kMaxSeen = 100;
    if (m_seenTweetIds.size() > kMaxSeen)
        m_seenTweetIds = m_seenTweetIds.mid(m_seenTweetIds.size() - kMaxSeen);
}

QJsonArray XClient::seenArray() const
{
    QJsonArray a;
    for (const QString &id : m_seenTweetIds)
        a.append(id);
    return a;
}

// 构造当前标签的首屏/翻页请求参数。网页端标签页各用各的接口：
// 为你推荐=HomeTimeline、正在关注=HomeLatestTimeline（均带 requestContext 与
// seenTweetIds 阅读进度）、文件夹=ListLatestTweetsTimeline（count 只是建议值，
// 实测单页返回 80+ 条，对整页报纸排版无碍）
static const char *opForTab(const QString &tabId, bool isList)
{
    if (isList)
        return kOpList;
    return (tabId == QLatin1String("fl")) ? kOpFollowing : kOpForYou;
}

static QJsonObject timelineVars(const QString &tabId, bool isList,
                                const QString &cursor)
{
    QJsonObject vars;
    if (isList) {
        vars["listId"] = tabId;
        vars["count"] = 20;
    } else {
        vars["count"] = 30;
        vars["includePromotedContent"] = (tabId == QLatin1String("fy"));
        vars["requestContext"] = "launch";
        vars["withCommunity"] = true;
    }
    if (!cursor.isEmpty())
        vars["cursor"] = cursor;
    return vars;
}

void XClient::fetchHome()
{
    remarkxSetCtx("xclient:fetchHome");
    const bool isList = (m_tabKind == QLatin1String("list"));
    QJsonObject vars = timelineVars(m_tabId, isList, QString());
    if (!isList) {
        // 模拟阅读进度：把本次会话已展示过的推文 id 一并上送（同网页端刷新行为）
        if (!m_seenTweetIds.isEmpty())
            vars["seenTweetIds"] = seenArray();
    }
    QNetworkRequest req = apiRequest(opForTab(m_tabId, isList), vars);
    diagRequest(QStringLiteral("home"), req,
                isList ? 0 : m_seenTweetIds.size(), QString());
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { handleHomeReply(reply); });
}

void XClient::handleHomeReply(QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleHomeReply");
    syncTimeFromReply(reply);
    reply->deleteLater();
    m_fetching = false;
    emit fetchingChanged(false);
    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = replyErrorText(reply);
        qWarning() << "XClient home error:" << m_tabId << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    const QByteArray body = reply->readAll();
    diagReply(QStringLiteral("home"), reply, body);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull()) {
        diagWrite(QStringLiteral("ANOM op=home kind=badjson head=")
                  + diagHead(body, 512));
        m_lastError = QStringLiteral("X 响应解析失败，请稍后重试");
        qWarning() << "XClient home bad json, len" << body.size();
        emit errorOccurred(m_lastError);
        return;
    }
    const QJsonObject data = doc.object();
    // HTTP 200 但顶层带 errors 数组（X 风控/参数校验失败）：按抓取失败处理，
    // 不重建 feed——重建会清掉已读内容跳回第 0 页，且 cursor 被清空后
    // 后续翻页/续抓永久失效（读者卡死在一本空书上）
    if (data.contains("errors") && !data["errors"].toArray().isEmpty()) {
        const QString emsg = topLevelError(data);
        m_lastError = emsg.isEmpty()
                          ? QStringLiteral("X 返回错误，请稍后重试") : emsg;
        diagWrite(QStringLiteral("ANOM op=home kind=errors head=")
                  + diagHead(body, 1024));
        qWarning() << "XClient home errors:" << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    QVector<XTweet> items;
    QString cursor;
    parseTimeline(data, m_tabKind, &items, &cursor);
    filterDataGarbage(QStringLiteral("home"), &items);
    if (items.isEmpty() && m_tabKind == QLatin1String("home")) {
        // 主时间线空（空文件夹是正常情况，主时间线空是异常）：不当空书
        // 重建，按错误处理让用户重试
        m_lastError = QStringLiteral("X 返回了空时间线，请稍后重试");
        diagWrite(QStringLiteral("ANOM op=home kind=empty len=")
                  + QByteArray::number(body.size())
                  + QStringLiteral(" head=") + diagHead(body, 512));
        qWarning() << "XClient home empty timeline";
        emit errorOccurred(m_lastError);
        return;
    }
    m_cursor = cursor;   // 空时间线（如空文件夹）也会走这里，cursor 为空即到底
    ingest(items, /*append=*/false);
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
        emit olderReady();   // 空批：读者停留在末页，不弹错误
        return;
    }
    loadSession();
    if (!m_sessionError.isEmpty()) {
        m_lastError = m_sessionError;
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_cursor.isEmpty()) {
        // 无翻页游标（时间线已到尽头）：不刷新、不重建 feed——
        // 重建会让在途媒体下载的任务索引失效（saveMedia 越界）且跳回第 0 页。
        // 直接按"无更多内容"处理，读者停留在末页。
        emit olderReady();
        return;
    }
    m_lastError.clear();
    m_fetching = true;
    emit fetchingChanged(true);
    const bool isList = (m_tabKind == QLatin1String("list"));
    QJsonObject vars = timelineVars(m_tabId, isList, m_cursor);
    if (!isList && !m_seenTweetIds.isEmpty())
        vars["seenTweetIds"] = seenArray();
    QNetworkRequest req = apiRequest(opForTab(m_tabId, isList), vars);
    diagRequest(QStringLiteral("older"), req,
                isList ? 0 : m_seenTweetIds.size(), m_cursor);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { handleOlderReply(reply); });
}

void XClient::handleOlderReply(QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleOlderReply");
    syncTimeFromReply(reply);
    reply->deleteLater();
    m_fetching = false;
    emit fetchingChanged(false);
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
        emit errorOccurred(m_lastError);
        return;
    }
    const QByteArray body = reply->readAll();
    diagReply(QStringLiteral("older"), reply, body);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull()) {
        diagWrite(QStringLiteral("ANOM op=older kind=badjson head=")
                  + diagHead(body, 512));
        m_lastError = QStringLiteral("X 响应解析失败，请稍后重试");
        m_extendErrorAt = QDateTime::currentMSecsSinceEpoch();
        qWarning() << "XClient older bad json, len" << body.size();
        emit errorOccurred(m_lastError);
        return;
    }
    const QJsonObject data = doc.object();
    if (data.contains("errors") && !data["errors"].toArray().isEmpty()) {
        const QString emsg = topLevelError(data);
        m_lastError = emsg.isEmpty()
                          ? QStringLiteral("X 返回错误，请稍后重试") : emsg;
        m_extendErrorAt = QDateTime::currentMSecsSinceEpoch();
        diagWrite(QStringLiteral("ANOM op=older kind=errors head=")
                  + diagHead(body, 1024));
        qWarning() << "XClient older errors:" << m_lastError;
        emit errorOccurred(m_lastError);
        return;
    }
    QVector<XTweet> items;
    QString cursor;
    parseTimeline(data, m_tabKind, &items, &cursor);
    filterDataGarbage(QStringLiteral("older"), &items);
    m_cursor = cursor;   // 空 cursor = 到底，fetchOlder 会按"无更多内容"处理
    ingest(items, /*append=*/true);
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

// ---- 详情页（某帖子的回复，按热度排序） ----
// 网页端帖子详情页同款接口：TweetDetail，rankingMode=Relevance 即"按热度"。
// 响应里 conversationthread-* 模块按热度分组，组内 items 有序；cursor 分页
// （下一页请求带上一次的 cursor-bottom，抓包确认 next==last）。

bool XClient::fetchDetail(const QString &tweetId)
{
    remarkxSetCtx("xclient:fetchDetail");
    if (tweetId.isEmpty())
        return false;
    DetailSession &s = m_details[tweetId];
    if (s.fetching)
        return false;   // 该帖正在抓取（深层翻页/首抓），本次忽略
    if (s.firstLoaded) {
        // 已有缓存：立即回放全部已加载回复（不重抓）
        emit detailReady(tweetId, s.replies, !s.cursor.isEmpty());
        return true;
    }
    startDetailFetch(tweetId, true);
    return true;
}

bool XClient::fetchDetailNext(const QString &tweetId)
{
    remarkxSetCtx("xclient:fetchDetailNext");
    auto it = m_details.find(tweetId);
    if (it == m_details.end() || it->fetching || it->cursor.isEmpty())
        return false;
    startDetailFetch(tweetId, false);
    return true;
}

void XClient::startDetailFetch(const QString &tweetId, bool first)
{
    remarkxSetCtx("xclient:startDetailFetch");
    DetailSession &s = m_details[tweetId];
    s.fetching = true;
    QJsonObject vars;
    vars["focalTweetId"] = tweetId;
    vars["with_rux_injections"] = false;
    vars["rankingMode"] = "Relevance";
    vars["includePromotedContent"] = true;
    vars["withCommunity"] = true;
    vars["withQuickPromoteEligibilityTweetFields"] = true;
    vars["withBirdwatchNotes"] = true;
    vars["withVoice"] = true;
    if (!first) {
        vars["cursor"] = s.cursor;
        vars["referrer"] = "tweet";
    }
    QNetworkRequest req = apiRequest(kOpTweetDetail, vars, kFieldToggles);
    diagRequest(QStringLiteral("detail"), req, 0, s.cursor);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, tweetId, reply]() { handleDetailReply(tweetId, reply); });
}

void XClient::handleDetailReply(const QString &tweetId, QNetworkReply *reply)
{
    remarkxSetCtx("xclient:handleDetailReply");
    syncTimeFromReply(reply);
    reply->deleteLater();
    auto it = m_details.find(tweetId);
    if (it == m_details.end())
        return;   // 会话已被刷新清空（抓取途中刷新了 feed），静默丢弃
    DetailSession *s = &*it;
    s->fetching = false;
    if (reply->error() != QNetworkReply::NoError) {
        // 详情页失败走专用信号（不复用 errorOccurred——那是 feed 级错误，
        // 会弹全局错误页把正在读的 feed 也盖掉）
        m_lastError = replyErrorText(reply);
        qWarning() << "XClient detail error:" << tweetId << m_lastError;
        emit detailFailed(tweetId);
        return;
    }
    const QByteArray body = reply->readAll();
    diagReply(QStringLiteral("detail"), reply, body);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull()) {
        diagWrite(QStringLiteral("ANOM op=detail kind=badjson id=")
                  + tweetId + QStringLiteral(" head=") + diagHead(body, 512));
        m_lastError = QStringLiteral("X 响应解析失败，请稍后重试");
        qWarning() << "XClient detail bad json:" << tweetId;
        emit detailFailed(tweetId);
        return;
    }
    const QJsonObject data = doc.object();
    if (data.contains("errors") && !data["errors"].toArray().isEmpty()) {
        const QString emsg = topLevelError(data);
        m_lastError = emsg.isEmpty()
                          ? QStringLiteral("X 返回错误，请稍后重试") : emsg;
        diagWrite(QStringLiteral("ANOM op=detail kind=errors id=")
                  + tweetId + QStringLiteral(" head=") + diagHead(body, 1024));
        qWarning() << "XClient detail errors:" << tweetId << m_lastError;
        emit detailFailed(tweetId);
        return;
    }
    QVector<XTweet> batch;
    QString cursor;
    parseDetail(data, &batch, &cursor);
    filterDataGarbage(QStringLiteral("detail"), &batch);
    QVector<XTweet> fresh;
    for (const XTweet &t : batch) {
        if (s->seen.contains(t.id))
            continue;
        s->seen.insert(t.id);
        s->replies.append(t);
        fresh.append(t);
    }
    // 本页没有新回复 = 到底了（即使 API 仍给出 cursor），避免死循环
    s->cursor = fresh.isEmpty() ? QString() : cursor;
    s->firstLoaded = true;
    emit detailReady(tweetId, fresh, !s->cursor.isEmpty());
}

void XClient::parseDetail(const QJsonObject &doc, QVector<XTweet> *replies,
                          QString *cursor)
{
    // 顶层文档还包一层 data（与 feed 解析同构，见 HomeTimeline 的
    // doc["data"]["home"]）；漏了这层会拿到空对象 → 回复/游标全丢
    const QJsonObject thread =
        doc["data"].toObject()
             ["threaded_conversation_with_injections_v2"].toObject();
    const QJsonArray ins = thread["instructions"].toArray();
    for (const QJsonValue &iv : ins) {
        const QJsonObject i = iv.toObject();
        if (i["type"].toString() != "TimelineAddEntries")
            continue;
        const QJsonArray entries = i["entries"].toArray();
        for (const QJsonValue &ev : entries) {
            const QJsonObject e = ev.toObject();
            const QJsonObject content = e["content"].toObject();
            const QString et = content["entryType"].toString();
            if (et == "TimelineTimelineCursor") {
                // 续抓只需要底部游标（忽略 cursor-top）
                if (!e["entryId"].toString().contains(QLatin1String("cursor-bottom")))
                    continue;
                *cursor = content["value"].toString();
                continue;
            }
            if (et != "TimelineTimelineModule")
                continue;   // TimelineTimelineItem 是主帖本身，复用 feed 里的副本
            if (e["entryId"].toString().startsWith(
                    QLatin1String("tweetdetailrelatedtweets")))
                continue;   // 相关帖子推荐，跳过
            const QJsonArray items = content["items"].toArray();
            for (const QJsonValue &itv : items) {
                const QJsonObject ic = itv.toObject()["item"].toObject()
                                          ["itemContent"].toObject();
                if (ic["__typename"].toString() != "TimelineTweet")
                    continue;
                if (ic.contains("promotedMetadata"))
                    continue;   // 广告/推广帖，跳过
                const QJsonObject result = ic["tweet_results"].toObject()
                                               ["result"].toObject();
                XTweet *t = normalize(result);
                if (t)
                    replies->append(*t);
                delete t;
            }
        }
    }
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

// ---- "数据帖"过滤（"整页 %22 数字"问题的直接修复） ----
// 此类帖子的正文是一段 URL 编码的纯数字推文 id 数组（%22 是双引号的百分号
// 编码，解码后形如 ["2093…","2093…",…]）。首次出现时经当时保留的请求/响应
// 日志确认为时间线里的真实推文正文（提交 eb4a359，"非错误页"），多为
// 数据/采集类账号所发。危害链：
//   1) 单条最长 25000 字符，一屏批量出现时双栏版面几乎全是这种卡片
//      （卡片截 6 行也整页是数字墙）；点卡片进详情页（全文不截断）则是
//      几十页纯数字，翻页翻不出去；
//   2) 这些 id 被 reportSeen 上送后强化 X 对此类帖子的推送，刷新仍见；
//   3) 若来源是异常响应回显（而非真实垃圾账号），同样被本规则拦下。
// 两条规则都要求长文本，正常帖子（含代码/数据示例帖）无法同时命中：
//   1) ≥10 个 %XX 百分号编码序列，且数字占非空白字符 ≥50% → 编码数据；
//   2) （解码形态）长度 ≥4000、数字占非空白 ≥85%、含数组分隔符 ","
//      → 解码后的纯数字 id 数组。
static bool isDataGarbage(const QString &s)
{
    const QString c = s.trimmed();
    if (c.size() < 200)
        return false;
    auto ishex = [](QChar ch) {
        return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
               || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
               || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
    };
    int digit = 0, nonws = 0, esc = 0;
    for (int i = 0; i < c.size(); ++i) {
        const QChar ch = c.at(i);
        if (ch.isSpace())
            continue;
        ++nonws;
        if (ch.isDigit()) {
            ++digit;
        } else if (ch == QLatin1Char('%') && i + 2 < c.size()
                   && ishex(c.at(i + 1)) && ishex(c.at(i + 2))) {
            ++esc;
        }
    }
    if (nonws == 0)
        return false;
    if (esc >= 10 && digit * 2 >= nonws)
        return true;
    if (c.size() >= 4000 && digit * 20 >= nonws * 17
            && c.contains(QStringLiteral("\",\"")))
        return true;
    return false;
}

// 从解析结果中剔除数据帖（不排版、不可收藏、不进 seen），并写 ANOM 日志
// （作者/id/正文片段）——问题再次出现时据此锁定是哪个账号所发，可去网页版
// 屏蔽该账号
static void filterDataGarbage(const QString &diagTag, QVector<XTweet> *items)
{
    for (int i = items->size() - 1; i >= 0; --i) {
        const XTweet &t = items->at(i);
        const QString *bad = nullptr;
        const QString *fields[5] = {&t.text, &t.originalText, &t.comment,
                                    &t.quoted.text, &t.quoted.originalText};
        for (const QString *s : fields)
            if (isDataGarbage(*s)) {
                bad = s;
                break;
            }
        if (!bad)
            continue;
        diagWrite(QString("ANOM op=%1 kind=data-garbage id=%2 author=%3 len=%4 "
                          "head=%5")
                    .arg(diagTag).arg(t.id)
                    .arg(t.authorHandle.isEmpty() ? t.authorName
                                                  : t.authorHandle)
                    .arg(bad->size())
                    .arg(bad->left(256)
                           .replace(QLatin1Char('\n'), QLatin1Char(' '))
                           .replace(QLatin1Char('\r'), QLatin1Char(' '))));
        items->removeAt(i);
    }
}

void XClient::parseTimeline(const QJsonObject &data, const QString &kind,
                            QVector<XTweet> *items, QString *cursor)
{
    remarkxSetCtx("xclient:parseTimeline");
    // 两种时间线的 instructions 位置不同（条目结构相同，见头文件注释）
    const QJsonObject root =
        (kind == QLatin1String("list"))
            ? data["data"].toObject()["list"].toObject()
                  ["tweets_timeline"].toObject()["timeline"].toObject()
            : data["data"].toObject()["home"].toObject()
                  ["home_timeline_urt"].toObject();
    const QJsonArray instructions = root["instructions"].toArray();
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
    // 推文可能在 feed 或详情会话回复缓存（命中也要写回媒体管线看到的
    // 那份，否则下次仍会重新下载）
    XTweet *t = findTweet(tweetId);
    if (!t)
        return false;
    QVector<XMedia> *list = job.quotedMediaIndex >= 0
                                ? &t->quoted.media : &t->media;
    if (job.isAvatar) {
        const QString rel = job.base + ".jpg";
        const QString full = m_mediaDir + "/" + rel;
        if (QFile::exists(full) && QFileInfo(full).size() > 0) {
            t->avatar = rel;
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
            if (job.quotedMediaIndex < 0 && t->isRetweet
                    && job.mediaIndex < t->quoted.media.size()) {
                t->quoted.media[job.mediaIndex].path = m.path;
            }
            ++m_feedRev;
            return true;
        }
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
    // 把路径写回对应推文（feed 或详情会话回复缓存）里的媒体
    if (XTweet *t = findTweet(tweetId)) {
        if (job.isAvatar) {
            t->avatar = base;
        } else {
            QVector<XMedia> *list = job.quotedMediaIndex >= 0
                                        ? &t->quoted.media : &t->media;
            // 防越界：任务快照的索引可能因 feed 重建而失效
            const int mi = job.mediaIndex >= 0 ? job.mediaIndex
                                               : job.quotedMediaIndex;
            if (mi >= 0 && mi < list->size())
                (*list)[mi].path = base;
            // 纯转推：原帖媒体同时是引用块媒体，路径同步过去
            if (job.quotedMediaIndex < 0 && t->isRetweet
                    && job.mediaIndex < t->quoted.media.size()) {
                t->quoted.media[job.mediaIndex].path = base;
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

XTweet *XClient::findTweet(const QString &tweetId)
{
    for (int i = 0; i < m_tweets.size(); ++i) {
        if (m_tweets[i].id == tweetId)
            return &m_tweets[i];
    }
    // 详情回复：不在 feed，存在详情页会话的回复缓存里（同一条回复可能
    // 属于多个会话，需逐个找）
    for (auto it = m_details.begin(); it != m_details.end(); ++it) {
        for (int i = 0; i < it->replies.size(); ++i) {
            if (it->replies[i].id == tweetId)
                return &it->replies[i];
        }
    }
    return nullptr;
}

void XClient::ensureMediaFor(QString tweetId)
{
    remarkxSetCtx("xclient:ensureMediaFor");
    if (m_inflightMedia.contains(tweetId))
        return;
    m_inflightMedia.insert(tweetId);

    XTweet *found = findTweet(tweetId);
    if (!found) {
        m_inflightMedia.remove(tweetId);
        return;
    }
    // 快照当前条目（异步期间 feed/会话可能被重建/追加，不能持有引用）
    const XTweet t = *found;

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
        req.setTransferTimeout(60000);
        QNetworkReply *reply = m_mediaNam.get(req);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tweetId, job, done]() {
                    reply->deleteLater();
                    saveMedia(tweetId, job, reply);
                    done();
                });
    }
}
