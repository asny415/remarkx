#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>

class InkItem;
class QNetworkReply;
class QTimer;

class PageStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(int feedPage READ feedPage NOTIFY stateChanged)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString bookLabel READ bookLabel NOTIFY stateChanged)
public:
    explicit PageStore(QObject *parent = nullptr);

    void configure(const QString &relayBase, const QString &baseDir);
    Q_INVOKABLE void setInk(InkItem *ink);
    void loadCalib(const QString &file);

    QString currentFile() const { return m_currentFile; }
    int feedPage() const { return m_feedPage; }
    int totalPages() const { return m_totalPages; }
    bool loading() const { return m_loading; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString bookLabel() const { return m_bookLabel; }

public slots:
    void start();
    void refresh();
    void next();
    void prev();
    void quit();
    Q_INVOKABLE void retry() { if (!m_error.isEmpty()) { m_error.clear(); emit errorChanged(); goPage(0, true); } }
    Q_INVOKABLE void suspendNow();
    Q_INVOKABLE void menuExit(int code);
    Q_INVOKABLE void setCalib(const QString &file);

signals:
    void currentFileChanged();
    void stateChanged();
    void errorChanged();

private:
    enum Mode { FeedMode, FavMode };

    void goPage(int n, bool force);
    void loadLocal(const QString &number);
    void downloadPage(int n, bool force);
    void onPageDownloaded(QNetworkReply *reply, int n);
    void onLayoutDownloaded(QNetworkReply *reply, const QString &number);
    void fetchStatus();
    void onStatus(QNetworkReply *reply);
    void extendPoll();
    void pollStatus(int attempts);
    void saveInkNow();
    void persistState();
    void persistBook();
    QString version() const { return m_version; }
    int entryIndex(const QString &version, int feedPage) const;
    void setStatus(const QString &s);
    void updateLabel();
    // 收藏（带笔迹）页相关
    QList<QString> favNumbers() const;
    int favCount() const;
    void enterFav(int index);
    void cleanupOnStartup();

    QString m_relay;
    QString m_bookDir;
    QString m_stateFile;
    QString m_bookJsonFile;
    QString m_calibFile;
    QNetworkAccessManager m_nam;
    InkItem *m_ink = nullptr;
    QTimer *m_pollTimer = nullptr;

    QJsonArray m_entries;
    QString m_date;
    int m_seq = 0;
    int m_feedPage = 0;
    int m_totalPages = 1;
    QString m_version;
    QString m_currentFile;
    QString m_currentNumber;
    int m_downloading = -1;
    bool m_loading = false;
    QString m_status;
    QString m_error;
    QString m_bookLabel;
    int m_extendTarget = -1;
    Mode m_mode = FeedMode;
    int m_favIndex = 0;
};
