#pragma once

#include <QHash>
#include <QImage>
#include <QJsonArray>
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
// 保留 book/ 收藏（带笔迹页）、断点续读、休眠/退出逻辑。
class PageStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(QVariantList imageSlots READ imageSlots NOTIFY imageSlotsChanged)
    Q_PROPERTY(int feedPage READ feedPage NOTIFY stateChanged)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool favMode READ favMode NOTIFY stateChanged)
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
    bool loading() const { return m_loading; }
    bool favMode() const { return m_mode == FavMode; }
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
    // 收藏页长按删除当前页（笔迹+缓存+索引一并移除）
    Q_INVOKABLE void deleteCurrentFav();
    // 页面展示后请求一次全屏强制刷新，清除墨水屏残影
    Q_INVOKABLE void requestFullRefresh();
    // 手指点按命中图片槽位 → 返回槽位索引（-1 = 未命中）
    Q_INVOKABLE int hitSlot(int x, int y);
    // 槽位对应媒体的全部本地文件（全屏分页浏览；未下载完的条目为空）
    Q_INVOKABLE QStringList slotFiles(int slotIndex);

signals:
    void currentFileChanged();
    void stateChanged();
    void errorChanged();
    void imageSlotsChanged();

private:
    enum Mode { FeedMode, FavMode };

    void goPage(int n);
    void loadLocal(const QString &number);
    void rebuildPages();
    void syncFeed();
    void renderCurrent(bool force = false);
    void insertCache(int n, const QImage &img);
    void buildSlotList();
    void requestSlotMedia();
    void onHomeReady();
    void onOlderReady();
    void onFetchError(const QString &msg);
    void onMediaReady(const QString &tweetId);
    void saveInkNow();
    void persistState();
    void persistBook();
    void setStatus(const QString &s);
    void updateLabel();
    void doFullRefresh();
    void forceEpdFullRefresh();
    QList<QString> favNumbers() const;
    int favCount() const;
    void enterFav(int index);
    void cleanupOnStartup();

    XClient *m_client = nullptr;
    Renderer *m_renderer = nullptr;
    QQuickImageProvider *m_provider = nullptr;

    QVector<XTweet> m_feed;
    QVector<RenderPage> m_pages;
    QImage m_currentBase;
    QHash<int, QImage> m_pageCache;   // feedPage -> 基础图（LRU）
    QVector<int> m_cacheOrder;
    QVariantList m_imageSlots;
    QSet<QString> m_avatarWanted;     // 正在等头像下载的推文
    QHash<int, QString> m_pageNumbers; // feedPage -> 已分配收藏页编号

    QString m_baseDir;
    QString m_bookDir;
    QString m_stateFile;
    QString m_bookJsonFile;
    QString m_calibFile;
    InkItem *m_ink = nullptr;
    QQuickWindow *m_window = nullptr;
    QMetaObject::Connection m_refreshConn;
    bool m_refreshArmed = false;

    QJsonArray m_entries;
    QString m_date;
    int m_seq = 0;
    int m_feedPage = 0;
    int m_totalPages = 1;
    int m_sessionSeq = 0;
    QString m_version;
    QString m_currentFile;
    QString m_currentNumber;
    bool m_loading = false;
    QString m_status;
    QString m_error;
    QString m_bookLabel;
    bool m_waitingOlder = false;
    Mode m_mode = FeedMode;
    int m_favIndex = 0;
    int m_baseRev = 0;
};
