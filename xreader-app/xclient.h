#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
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
    QString text;
    QString createdAt;
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
    QString text;         // 译文优先，无译文为原文
    QString originalText;
    QString comment;      // 引用推文时：引用者的评论
    QString rtHandle;     // 纯转推时：转发者
    QString url;
    QString avatar;       // 相对 media_dir 的路径（下载后）或原始 URL
    QString lang;
    QString sourceLang;
    QString destLang;
    bool isRetweet = false;
    QVector<XMedia> media;
    XQuoted quoted;
    int reposts = 0;
    int likes = 0;
    int replies = 0;
    int views = 0;
};

// X (Twitter) 直连客户端：带 Cookie 的 GraphQL 抓取 + 媒体懒加载下载。
// 内存保留当前阅读内容（feed），刷新整体重建、续抓往尾部追加。
// 不做任何后台定时抓取：App 打开实时抓取，翻到书尾才续抓。
class XClient : public QObject {
    Q_OBJECT
public:
    explicit XClient(QObject *parent = nullptr);

    // 读 baseDir/config.json（proxy / cookies 路径），构造客户端
    void configure(const QString &baseDir);

    const QVector<XTweet> &feed() const { return m_tweets; }
    int count() const { return m_tweets.size(); }
    bool hasSession() const;
    bool fetching() const { return m_fetching; }
    QString lastError() const { return m_lastError; }
    QString mediaDir() const { return m_mediaDir; }
    QString mediaPath(const QString &relative) const;
    // 某推文某张媒体是否下载失败（槽位显示"加载失败"）
    bool mediaFailed(const QString &tweetId, int mediaIndex,
                     bool quoted) const;

public slots:
    void start();          // 抓取首页（重建 feed）
    void refresh();        // 同 start()，语义为"刷新"
    void fetchOlder();     // 用 cursor 续抓更早内容（追加到尾部）
    void ensureMediaFor(const QString &tweetId);   // 下载该推文媒体+头像

signals:
    void homeReady();                    // 首页/刷新完成，feed 已重建
    void olderReady();                   // 续抓完成，feed 已追加
    void mediaReady(const QString &tweetId);
    void errorOccurred(const QString &message);
    void fetchingChanged(bool fetching);

private:
    void fetchHome();
    void loadSession();
    QNetworkRequest apiRequest(const QString &op, const QJsonObject &variables);
    void handleHomeReply(const QString &which, QNetworkReply *reply);
    void maybeMergeHome();
    void handleOlderReply(const QString &which, QNetworkReply *reply);
    void maybeMergeOlder();
    void ingest(const QVector<XTweet> &batch, bool append);
    static QJsonObject unwrapResult(const QJsonObject &result);
    static void parseTimeline(const QJsonObject &data, QVector<XTweet> *items,
                              QString *cursor);
    static XTweet *normalize(const QJsonObject &result);
    static void authorInfo(const QJsonObject &r, QString *name,
                           QString *handle, QString *avatar);
    static QVector<XMedia> mediaList(const QJsonObject &tweet);
    static XStats statsOf(const QJsonObject &r);
    static QString noteText(const QJsonObject &tweet);
    static QString rawText(const QJsonObject &r);
    static QString textOf(const QJsonObject &r);
    static QVector<XTweet> mergeInterleave(const QVector<XTweet> &fy,
                                           const QVector<XTweet> &fl);

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

    QVector<XTweet> m_tweets;
    QSet<QString> m_seen;
    QString m_cursor;            // For You 向后翻页游标
    QString m_cursorFollowing;   // Following 向后翻页游标

    bool m_fetching = false;
    // 首页并发抓取状态
    bool m_homePending = false;
    int m_homeLeft = 0;
    QVector<XTweet> m_fy, m_fl;
    // 续抓并发状态
    bool m_olderPending = false;
    int m_olderLeft = 0;
    QVector<XTweet> m_oy, m_ol;

    QString m_lastError;
    QSet<QString> m_inflightMedia;
    QSet<QString> m_failedMedia;   // "tweetId:q?idx" → 下载失败
};
