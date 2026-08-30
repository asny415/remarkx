#pragma once

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QQuickImageProvider>
#include <QSet>
#include <QVariantList>
#include <QVector>

#include "renderer.h"
#include "xclient.h"

class InkItem;
class QNetworkReply;
class QQuickWindow;
class QTimer;
class PageStore;
class Telegram;

// 为 QML 提供当前页基础位图（image://pages/base）
class PageImageProvider : public QQuickImageProvider {
public:
    explicit PageImageProvider(PageStore *store)
        : QQuickImageProvider(QQuickImageProvider::Image),
          m_store(store)
    {
    }
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    PageStore *m_store;
};
// 页面状态机：抓取（XClient）→ 排版渲染（Renderer）→ 基础页 + 图片槽位。
// 笔迹即收藏：写字后用笔迹起始位置锁定对应帖子，渲染"帖+笔迹"独立图，
// 连同原始链接存入 book/（favs.json 索引），可推送 Telegram；不在 UI 展示。
class PageStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(QVariantList imageSlots READ imageSlots NOTIFY imageSlotsChanged)
    Q_PROPERTY(int feedPage READ feedPage NOTIFY stateChanged)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY stateChanged)
    Q_PROPERTY(int pageKey READ pageKey NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString bookLabel READ bookLabel NOTIFY stateChanged)
public:
    explicit PageStore(QObject *parent = nullptr);

    void configure(const QString &baseDir);
    void setWindow(QQuickWindow *window);
    Q_INVOKABLE void setInk(InkItem *ink);
    QQuickImageProvider *provider() const { return m_provider; }

    QString currentFile() const { return m_currentFile; }
    QVariantList imageSlots() const { return m_imageSlots; }
    int feedPage() const { return m_feedPage; }
    int totalPages() const { return m_totalPages; }
    int pageKey() const { return m_pageKey; }
    bool loading() const { return m_loading; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString bookLabel() const { return m_bookLabel; }
    QImage currentBaseImage() const { return m_currentBase; }

public slots:
    void start();
    void refresh();
    void next();
    void prev();
    void quit();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void suspendNow();
    Q_INVOKABLE void menuExit(int code);
    Q_INVOKABLE void setCalib(const QString &file);
    // 页面展示后请求一次全屏强制刷新，清除墨水屏残影
    Q_INVOKABLE void requestFullRefresh();
    // 手指点按命中图片槽位 → 返回槽位索引（-1 = 未命中）
    Q_INVOKABLE int hitSlot(int x, int y);
    // 手指点按命中"显示全文"按钮 → 返回推文 id（"" = 未命中）
    Q_INVOKABLE QString hitFullText(int x, int y);
    // 全文全屏：总页数
    Q_INVOKABLE int fullTextPages(const QString &tweetId);
    // 槽位对应媒体的全部本地文件（全屏分页浏览；未下载完的条目为空）
    Q_INVOKABLE QStringList slotFiles(int slotIndex);
    // 供 image://pages/text/... provider 渲染全文页（公开给 Provider 调用）
    QImage textPageImage(const QString &tweetId, int page);

signals:
    void currentFileChanged();
    void stateChanged();
    void errorChanged();
    void imageSlotsChanged();

private:
    void goPage(int n);
    void maybePrefetchOlder();
    void rebuildPages(bool resetPageNumbers);
    void syncFeed();
    void renderCurrent(bool force = false);
    void buildSlotList();
    void requestSlotMedia();
    void onHomeReady();
    void onOlderReady();
    void onFetchError(const QString &msg);
    void onMediaReady(const QString &tweetId);
    void saveInkNow();
    void persistState();
    void persistFavs();
    // 按 (feed_page, tweet_id) 更新或新增收藏，返回该收藏编号；
    // 新收藏返回时 *fresh=true（触发 Telegram 推送）
    QString upsertFav(const XTweet &t, const QString &pageNum, bool *fresh);
    QString allocNumber();
    void updateFavImage(const QString &number, const QString &tweetId);
    void onFavSent(const QString &number);
    void setStatus(const QString &s);
    void updateLabel();
    void doFullRefresh();
    void forceEpdFullRefresh();
    // 笔迹像素命中的当前页所有帖子（返回 m_feed 下标，去重、按 feed 顺序）
    void hitTweets(const QImage *ink, QVector<int> *out);
    // 笔迹起始位置命中当前页的哪个帖子（返回 m_feed 下标；未命中取最近块）
    int hitTweetIndex(int x, int y);
    void cleanupOnStartup();

    XClient *m_client = nullptr;
    Renderer *m_renderer = nullptr;
    Telegram *m_telegram = nullptr;
    QQuickImageProvider *m_provider = nullptr;

    QVector<XTweet> m_feed;
    QVector<RenderPage> m_pages;
    QImage m_currentBase;
    QVariantList m_imageSlots;
    QSet<QString> m_avatarWanted;     // 正在等头像下载的推文
    QHash<int, QString> m_pageNumbers; // feedPage -> 已分配笔迹编号
    // 已渲染页面位图 LRU 缓存：翻页回看直接复用，不再每次重排版渲染
    QHash<int, QImage> m_pageCache;
    QVector<int> m_pageCacheOrder;    // 访问顺序（末尾=最近）
    quint64 m_feedRev = ~0ull;        // 上次同步的 feed 版本（~0 强制首次拷贝）

    QString m_baseDir;
    QString m_bookDir;
    QString m_stateFile;
    QString m_favsJsonFile;
    QString m_calibFile;
    InkItem *m_ink = nullptr;
    QQuickWindow *m_window = nullptr;
    QMetaObject::Connection m_refreshConn;
    bool m_refreshArmed = false;

    QJsonArray m_favs;         // 收藏索引：{number,tweet_id,url,feed_page,created}
    QString m_date;
    int m_seq = 0;
    int m_feedPage = 0;
    int m_totalPages = 1;
    QString m_currentFile;
    QString m_currentNumber;
    bool m_loading = false;
    QString m_status;
    QString m_error;
    QString m_bookLabel;
    bool m_waitingOlder = false;
    bool m_prefetchOlder = false;      // 后台预抓更早内容（不阻塞翻页）
    bool m_lastPrefetchEmpty = false;  // 上次预抓无新内容（时间线已到头）
    bool m_extendErrorWas = false;   // 上次错误是否来自续抓（重试走续抓而非首页刷新）
    int m_baseRev = 0;
    int m_pageKey = 0;
    QString m_lastDisplayKey;
    QTimer *m_avatarTimer = nullptr;
    bool m_avatarRefreshPending = false;
};
