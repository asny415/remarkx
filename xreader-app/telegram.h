#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QVector>

class QTimer;

// 收藏通知：把带笔迹的帖子图片异步发到配置的 Telegram 聊天。
// config.json 可选字段：
//   "telegram_bot":  Bot Token（形如 123456:AAH...）
//   "telegram_chat": 目标 chat id（数字或 @频道名）
// 未配置 bot/chat 时静默（不本地保存待发队列）。
//
// 发送全程后台：QNAM 异步上传 + 串行待发队列，不阻塞 UI。
// 可靠性：待发消息先持久化到 baseDir/pending.json，成功才出队；
// 失败按指数退避定时重试；程序异常退出/重启后从队列恢复继续补发。
// 直接走 HTTP POST multipart 到 api.telegram.org；配置了 proxy 则复用。
class Telegram : public QObject {
    Q_OBJECT
public:
    explicit Telegram(QObject *parent = nullptr);

    // 读 baseDir/config.json 的 telegram_bot / telegram_chat / proxy，
    // 并加载待发队列 pending.json。
    void configure(const QString &baseDir);

    bool enabled() const { return !m_bot.isEmpty() && !m_chat.isEmpty(); }

    // 入队一条收藏通知（按 number 去重），随后后台尝试发送。
    // caption 为原始帖子完整链接；图片取 book/<number>.png。
    void enqueue(const QString &number, const QString &url);

    // 应用启动后调用：重置退避、立即补发所有未完成的消息。
    void flush();

signals:
    // 某条收藏已成功推送（接收方据此删除本地图片）
    void sent(const QString &number);

private:
    struct Pending {
        QString number;
        QString url;
        int attempts = 0;
        qint64 nextTryMs = 0;   // 0 = 立即可发
    };

    void pump();
    void sendOne(const Pending &item);
    void onReplyFinished(QNetworkReply *reply, const QString &number);
    void markFailed(const QString &number);
    void saveQueue();
    void loadQueue();
    void scheduleNext();
    static qint64 backoffMs(int attempts);

    QString m_baseDir;
    QString m_bot;
    QString m_chat;
    QNetworkAccessManager m_nam;
    QVector<Pending> m_pending;
    bool m_sending = false;        // 串行：一次只发一个
    QTimer *m_retryTimer = nullptr;
};
