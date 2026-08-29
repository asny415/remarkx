#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPair>
#include <QRect>
#include <QStringList>
#include <QVector>

#include "xclient.h"

class QFontMetricsF;
class QPainter;

// 页面上的一个图片槽位（卡片只显示媒体列表第一张；全屏可浏览全部）
struct ImageSlot {
    int x = 0, y = 0, w = 0, h = 0;
    int tweetIndex = -1;
    bool isQuoted = false;   // true=引用块内的图
    int mediaIndex = 0;      // 该列表里被显示的那张（通常是 0）
    bool video = false;
    int nMedia = 0;          // 该列表含 url 的媒体总数（"共 N 图"）
};

// 单条绘制指令（顺序执行，自上而下消耗竖直空间）
struct Op {
    enum Kind {
        Head, Line, Img, QHead, QLine, QImg, Stats,
        FullText,    // "显示全文" 按钮行
        FloatCard    // 窄图卡片：头部+右上图+文字环绕（原子块，不可拆分）
    };
    int kind = Head;
    int y = 0;
    QString text;                 // Line/QLine/FullText 文本
    QString qname, qhandle;       // QHead
    int slotIndex = -1;           // Img/QImg/FloatCard → RenderPage::slots 下标
    QString statsLeft, statsRight; // Stats
    // FloatCard 专用
    QStringList floatLines;       // 图片旁侧（窄宽）文本行
    QStringList belowLines;       // 图片下方（全宽）文本行
    QString btnLabel;             // 卡片内"显示全文"按钮文案（空=无）
    int imgW = 0, imgH = 0;       // FloatCard 图片显示尺寸
};

// 一个排版卡片（某推文在一页上的连续块；跨栏/跨页时拆成多个 chunk）
struct RenderChunk {
    QRect rect;              // x/w 列内固定；y/h 随排版增长
    bool isCont = false;     // 续排段（无上边框）
    bool hasCont = false;    // 后面还有续排段（无下边框）
    int tweetIndex = -1;
    QVector<Op> ops;
};

// "显示全文" 按钮热区（全屏看全文用）
struct TextButton {
    QRect rect;
    int tweetIndex = -1;
};

struct RenderPage {
    QVector<RenderChunk> chunks;
    QVector<ImageSlot> images;
    QVector<TextButton> buttons;
};

// 整页渲染器：把推文流排版成 1404x1872 整页位图（原 relay/render.py 的移植）。
// 图片采用懒加载——基础页只画占位框，真实图片由 QML 层按槽位贴图。
class Renderer : public QObject {
    Q_OBJECT
public:
    explicit Renderer(QObject *parent = nullptr);

    // 加载字体（baseDir/fonts/remarkx-cjk.ttf）
    void configure(const QString &baseDir);

    // 全文流式双栏分页
    QVector<RenderPage> paginate(const QVector<XTweet> &feed);

    // 渲染第 pageIndex 页。withPhotos=true 时把已下载图片烘焙进页面
    // （保存收藏页/调试用）；false 时图片位置只画占位框。
    QImage renderPage(const QVector<XTweet> &feed,
                      const QVector<RenderPage> &pages, int pageIndex,
                      bool withPhotos);

    // 全文全屏阅读：把某条推文的完整内容（含原文/引用块）单栏排版成若干整页
    // 位图，返回第 pageIndex 页；totalPages 输出总页数。
    QImage renderTextPage(const XTweet &t, int pageIndex, int *totalPages);
    int textPageCount(const XTweet &t);

    // 语言代码 -> 中文名（"译自英语"）
    static QString langName(const QString &code);

    // 文本清洗（隐藏链接、剥离 emoji）
    static QString cleanText(const QString &text);

private:
    struct Atom {
        int kind = Op::Head;
        int h = 0;
        QString text;
        QString qname, qhandle;
        QString statsLeft, statsRight;
        QStringList floatLines, belowLines;   // FloatCard 用
        QString btnLabel;
        int tweetIndex = -1;
        bool isQuoted = false;
        int mediaIndex = 0;
        bool video = false;
        int nMedia = 0;
        int ind = 0;
        int dw = 0, dh = 0;
    };

    // 窄图卡片布局：头部 + 右上图 + 左侧文字环绕 + 统计（原子块）。
    // 不可行（图太宽/总高超过单列）返回 kind=-1，调用方回退普通布局。
    Atom makeFloatCard(const XTweet &t, const Atom &img);

    QFont font(int pixel, bool bold = false) const;
    qreal textWidth(const QFont &f, const QString &text) const;
    QString ellipsize(const QFont &f, const QString &text,
                      qreal maxWidth) const;
    QStringList wrapText(const QFont &f, const QString &text, qreal maxWidth,
                         int maxLines, bool *truncated = nullptr) const;
    QVector<Atom> buildAtoms(const QVector<XTweet> &feed, int ti);
    Atom imgAtom(const QVector<XTweet> &feed, int ti, bool isQuoted, int ind);
    QString statsText(const XTweet &t) const;

    void drawCard(QPainter &p, const QVector<XTweet> &feed,
                  const RenderChunk &chunk, const RenderPage &page,
                  bool withPhotos);
    void drawCardBorder(QPainter &p, int x, int y0, int y1, int w,
                        bool top, bool bottom) const;
    void drawHead(QPainter &p, const XTweet &t, int px, int py);
    void drawLine(QPainter &p, const QString &text, int x, int y,
                  const QFont &f, const QColor &c) const;
    void drawQuotedBar(QPainter &p, int x, int y0, int y1) const;
    void drawPhoto(QPainter &p, const XTweet &t, const ImageSlot &s,
                   bool withPhotos);
    void drawPlaceholder(QPainter &p, int ix, int py, int dw, int dh) const;
    void drawStats(QPainter &p, const Op &op, const RenderChunk &chunk) const;
    QImage avatar(const XTweet &t) const;
    static QString absTime(const QString &createdAt);
    static QString fmtCount(int n);

    QString m_fontPath;
    QString m_mediaDir;
    QString m_family;
    mutable QHash<QPair<int, bool>, QFont> m_fonts;
    mutable QHash<QString, QImage> m_avatarCache;
    mutable QHash<QString, QImage> m_photoCache;
};
