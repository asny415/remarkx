#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

class QNetworkReply;
class QNetworkRequest;

// 互动数统计
struct XStats {
    int reposts = 0;
    int likes = 0;
    int replies = 0;
    int views = 0;
};

// 推文媒体（一张图/一个视频封面）
struct XMedia {
    QString url;
    int w = 0;
    int h = 0;
    QString path;      // 相对 media_dir 的本地路径（下载完成后填充）
    bool video = false;
};

// 引用/转推的原帖块
struct XQuoted {
    QString authorName;
    QString authorHandle;
    QString text;          // 默认显示文本（译文优先，否则 full_text 预览）
    QString originalText;  // 完整原文（note_tweet 全文或 full_text）
    QString createdAt;
    bool translated = false;   // 该文本显示的是译文
    bool isExpandable = false; // note_tweet.is_expandable：帖子还有更多内容
    QString sourceLang;        // 译自哪种语言
    QVector<XMedia> media;
    int reposts = 0;
    int likes = 0;
    int replies = 0;
    int views = 0;
};

// 归一化后的推文（原 relay/fetcher.py 的 _normalize 输出）
struct XTweet {
    QString id;
    QString createdAt;
    QString authorName;
    QString authorHandle;
    QString text;         // 默认显示文本（译文优先，否则 full_text 预览）
    QString originalText; // 完整原文（note_tweet 全文，或 full_text）
    QString comment;      // 引用推文时：引用者的评论
    QString rtHandle;     // 纯转推时：转发者
    QString url;
    QString avatar;       // 相对 media_dir 的路径（下载后）或原始 URL
    QString lang;
    QString sourceLang;
    QString destLang;
    bool isRetweet = false;
    bool translated = false;   // 当前显示的是译文（grok 翻译存在）
    bool isExpandable = false; // note_tweet.is_expandable：帖子还有更多内容
    QVector<XMedia> media;
    XQuoted quoted;
    int reposts = 0;
    int likes = 0;
    int replies = 0;
    int views = 0;
};

// 首页标签（官方网页版首页标签栏同款）：前两个固定"为你推荐/正在关注"，
// 后面是用户置顶的文件夹（X 列表 List，PinnedTimelines 接口，顺序即网页顺序）
struct XTab {
    QString id;      // "fy" / "fl" / 列表 id_str
    QString name;    // 显示文本
    bool fixed = false;  // 前两个固定标签（文件夹抓取失败时仍可用）
};

// X (Twitter) 直连客户端：带 Cookie 的 GraphQL 抓取 + 媒体懒加载下载。
// 内存保留当前阅读内容（feed），刷新整体重建、续抓往尾部追加。
// 不做任何后台定时抓取：App 打开实时抓取，翻到书尾才续抓。
// 一次只抓一个标签的时间线（setTab 选择），与网页版点标签页的行为一致。
class XClient : public QObject {
    Q_OBJECT
public:
    explicit XClient(QObject *parent = nullptr);

    // 读 baseDir/config.json（proxy / cookies 路径），构造客户端
    void configure(const QString &baseDir);

    const QVector<XTweet> &feed() const { return m_tweets; }
    int count() const { return m_tweets.size(); }
    // 标签列表（fetchFolders 成功后填充：两个固定标签 + 置顶文件夹）
    const QVector<XTab> &tabs() const { return m_tabs; }
    QString currentTabId() const { return m_tabId; }
    QString currentTabName() const;
    // 选择要抓取的时间线（"fy"/"fl"/列表 id_str），start/refresh 之前调用
    void setTab(const QString &tabId);
    // feed 内容每变更一次（追加/重建/媒体路径写回）自增，供 PageStore
    // 判断是否需要重新同步其快照，避免每次翻页都深拷贝整个 feed
    quint64 feedRevision() const { return m_feedRev; }
    bool hasSession() const;
    bool fetching() const { return m_fetching; }
    QString lastError() const { return m_lastError; }
    // 服务器时钟偏移（毫秒，正=本地落后）：最近一次 API 响应 Date 头估算；
    // 系统时钟已被修正时为 0；供渲染端显示兜底
    qint64 timeOffsetMs() const { return m_timeOffsetMs; }
    QString mediaDir() const { return m_mediaDir; }
    QString mediaPath(const QString &relative) const;
    // 某推文某张媒体是否下载失败（槽位显示"加载失败"）
    bool mediaFailed(const QString &tweetId, int mediaIndex,
                     bool quoted) const;

    // 详情页（某帖子的回复，按热度排序）：返回 true=已开始抓取或已有缓存
    //（detailReady 立即发出）；false=正在抓取中（忽略本次请求）。
    // 回复在内存按帖子缓存（DetailSession），刷新 feed 时清空。
    bool fetchDetail(const QString &tweetId);
    bool fetchDetailNext(const QString &tweetId);

    // 在已知容器（feed 或任一详情页会话的回复缓存）中按 id 找推文；
    // 媒体下载/路径写回都经此定位——详情回复不在 feed 里，只查 m_tweets
    // 会让回复的图片永远不下载。找不到返回 nullptr。
    XTweet *findTweet(const QString &tweetId);

public slots:
    void fetchFolders();   // 启动时抓取标签列表（PinnedTimelines → foldersReady）
    void start();          // 抓取当前标签的时间线（重建 feed）
    void refresh();        // 同 start()，语义为"刷新"
    void fetchOlder();     // 用 cursor 续抓更早内容（追加到尾部）
    // 汇报"已读"进度：网页端在翻页/刷新时会把已渲染过的推文 id 以
    // seenTweetIds 附在 HomeTimeline 请求上（模拟阅读过程，下次刷新
    // 更可能拿到新内容）。PageStore 每展示一页调用一次本函数。
    void reportSeen(const QString &tweetId);
    void ensureMediaFor(QString tweetId);   // 下载该推文媒体+头像（值传递：调用方
                                            // 传入的 feed 缓冲区可能在本函数内被
                                            // 同步触发的 mediaReady→syncFeed 释放）

signals:
    void foldersReady();                 // 标签列表抓取完成（tabs() 可取）
    void homeReady();                    // 首页/刷新完成，feed 已重建
    void olderReady();                   // 续抓完成，feed 已追加
    void mediaReady(const QString &tweetId);
    void errorOccurred(const QString &message);
    void fetchingChanged(bool fetching);
    // 详情页回复到达：fresh=本页新增的回复（已按 rest_id 去重）；
    // hasMore=true 还有下一页（fetchDetailNext 续抓）
    void detailReady(const QString &tweetId, const QVector<XTweet> &fresh,
                     bool hasMore);
    // 详情页抓取失败（错误文本见 lastError()）
    void detailFailed(const QString &tweetId);
    // 时钟校准（每次 API 响应后偏移有实质变化时发出）：offsetMs=服务器−本地
    //（系统时钟已修正时为 0）；systemUpdated=本次是否成功修正了系统时钟
    void timeSynced(qint64 offsetMs, bool systemUpdated);

private:
    void fetchHome();
    void loadSession();
    QNetworkRequest apiRequest(const QString &op, const QJsonObject &variables,
                               const QString &fieldToggles = QString());
    void handleFoldersReply(QNetworkReply *reply);
    void handleHomeReply(QNetworkReply *reply);
    void handleOlderReply(QNetworkReply *reply);
    // 时钟校准：从响应 Date 头估算服务器时钟偏移（SNTP 式中点），漂移 >2s
    // 时尝试修正系统时钟（root）；失败则保留偏移供渲染端显示补偿。
    // 各 API 响应处理器入口调用（含 HTTP 错误响应——Date 头同样有效）
    void syncTimeFromReply(QNetworkReply *reply);
    void ingest(const QVector<XTweet> &batch, bool append);
    void startDetailFetch(const QString &tweetId, bool first);
    void handleDetailReply(const QString &tweetId, QNetworkReply *reply);
    static void parseDetail(const QJsonObject &data, QVector<XTweet> *replies,
                            QString *cursor);
    static QJsonObject unwrapResult(const QJsonObject &result);
    // kind: "home" = HomeTimeline/HomeLatestTimeline（data.home.home_timeline_urt）；
    //       "list" = ListLatestTweetsTimeline（data.list.tweets_timeline.timeline）
    static void parseTimeline(const QJsonObject &data, const QString &kind,
                              QVector<XTweet> *items, QString *cursor);
    static XTweet *normalize(const QJsonObject &result);
    static void authorInfo(const QJsonObject &r, QString *name,
                           QString *handle, QString *avatar);
    static QVector<XMedia> mediaList(const QJsonObject &tweet);
    static XStats statsOf(const QJsonObject &r);
    static QString noteText(const QJsonObject &tweet);
    static QString rawText(const QJsonObject &r);
    static QString textOf(const QJsonObject &r);
    static bool hasTranslation(const QJsonObject &r);

    // 详情页会话：某帖子的回复（按热度排序），cursor 分页。
    // 回复在内存按帖子缓存，同一帖子再次进入不重抓；refresh() 清空。
    struct DetailSession {
        QString cursor;               // 下一页游标（空=没有更多）
        bool fetching = false;        // 详情页抓取进行中
        bool firstLoaded = false;     // 第一页已回来（之后进入立即回放缓存）
        QSet<QString> seen;           // 回复 rest_id 去重
        QVector<XTweet> replies;      // 已累积回复（热度序，追加式分页）
    };

    // 单个媒体下载任务（快照，跨异步安全）
    struct Job {
        int mediaIndex = -1;       // 主媒体在 tweet.media 的下标
        int quotedMediaIndex = -1; // 引用块媒体下标（-1=不是引用块媒体）
        QString url;
        QString base;              // 相对 media_dir 的文件名（不含扩展名）
        bool isAvatar = false;
    };
    void finishMedia(const QString &tweetId);
    bool cacheHit(const QString &tweetId, const Job &job);
    void saveMedia(const QString &tweetId, const Job &job,
                   QNetworkReply *reply);

    QString m_baseDir;
    QString m_proxy;
    QString m_cookiesFile;
    QString m_mediaDir;
    QNetworkAccessManager m_nam;
    QNetworkAccessManager m_mediaNam;

    QString m_authToken, m_ct0, m_twid, m_guestId;
    QString m_sessionError;

    // 已读推文 id 列表（网页端 seenTweetIds 同款：不查重、按展示顺序追加，
    // 限制长度防请求过大），随 HomeTimeline 请求上送模拟阅读进度
    QStringList m_seenTweetIds;
    QJsonArray seenArray() const;

    // 当前标签：一次只抓一个时间线（"fy"/"fl"/列表 id_str），换标签时
    // setTab() 清游标。m_tabKind 与 m_tabId 同步（"home" 或 "list"），
    // 供 parseTimeline 选数据路径
    QString m_tabId = "fy";
    QString m_tabKind = "home";
    QVector<XTab> m_tabs;        // 标签列表（fetchFolders 后填充）

    QVector<XTweet> m_tweets;
    quint64 m_feedRev = 0;
    QSet<QString> m_seen;
    QString m_cursor;            // 当前标签向后翻页游标

    bool m_fetching = false;
    QHash<QString, DetailSession> m_details;   // tweetId -> 详情页会话

    QString m_lastError;
    qint64 m_extendErrorAt = 0;   // 续抓失败时间（冷却，防风控持续 403）
    QSet<QString> m_inflightMedia;
    QHash<QString, int> m_mediaPending;   // tweetId -> 剩余下载任务数
    QSet<QString> m_failedMedia;   // "tweetId:q?idx" → 下载失败

    // 时钟校准状态：m_timeOffsetMs 是渲染端显示兜底用的偏移（系统时钟已
    // 修正后为 0）；m_lastClockAttemptMs 限修正系统时钟的尝试频率（防无权限
    // 时每次响应都重试刷日志）
    qint64 m_timeOffsetMs = 0;
    qint64 m_lastClockAttemptMs = 0;
};
