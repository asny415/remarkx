#include "renderer.h"
#include "crashctx.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

// ---- 版面参数（原 relay/render.py 一致） ----
static const int W = 1404;
static const int H = 1872;
static const int MARGIN = 48;
static const int COL_GUTTER = 28;
static const int CARD_GAP = 24;
static const int PAD = 22;
static const int CONTENT_W = W - 2 * MARGIN;
static const int COL_W = (CONTENT_W - COL_GUTTER) / 2;
static const int TEXT_W = COL_W - 2 * PAD;
static const int TOP_Y = MARGIN;
static const int BOTTOM_Y = H - MARGIN;
static const int NAME_H = 36;
static const int META_H = 26;
static const int TEXT_LH = 44;
static const int IMG_MAX_H = 400;
static const int IMG_GAP = 12;
static const int PAD_TOP = 18;
static const int PAD_BOTTOM = 18;
static const int HEAD_H = NAME_H + 6 + META_H + 8;
static const int QIND = 30;
static const int QHEAD_H = 36;
static const int TEXT_LH_Q = 38;
static const int STATS_GAP_TOP = 28;
static const int STATS_H = 30;
static const int AVATAR_D = 56;
static const int AVATAR_GAP = 14;

// 卡片文本最大行数：超出截断并显示"显示全文"按钮（长文/长译文都适用）
static const int BODY_MAX_LINES = 6;
static const int QUOTED_MAX_LINES = 4;

// 窄图卡片（FloatCard）：图片与文字的间距、总高上限（超限回退普通布局）
static const int FLOAT_GAP = 14;
static const int FLOAT_MAX_H = 1600;

// 栏底补位：图片卡在剩余高度内放不下时，若空白较大，把后面某个纯文本帖整卡
// 提前填入空白，避免大片留白（图片仍不截断、整体下移）。
static const int FILL_MIN_H = 240;    // 空白高度达到该值才考虑补位
static const int FILL_MIN_CARD = 200; // 补位卡至少要有这个高度（避免小卡填大洞）
static const int FILL_SCAN = 6;       // 最多向前扫描这么多条找纯文本帖

// 有界 LRU 缓存：插入并把最旧的一条淘汰掉。整清缓存会让翻页回看时
// 头像/图片重新从磁盘解码缩放，是"省内存伤翻页"的过度优化。
static void lruInsert(QHash<QString, QImage> &cache, QList<QString> &order,
                      const QString &key, const QImage &img, int cap)
{
    order.removeAll(key);
    cache.insert(key, img);
    order.append(key);
    while (order.size() > cap)
        cache.remove(order.takeFirst());
}

QString Renderer::langName(const QString &code)
{
    static const QHash<QString, QString> m = {
        {"en", "英语"}, {"ja", "日语"}, {"ko", "韩语"},
        {"fr", "法语"}, {"de", "德语"}, {"es", "西班牙语"},
        {"ru", "俄语"}, {"it", "意大利语"}, {"pt", "葡萄牙语"},
        {"nl", "荷兰语"}, {"tr", "土耳其语"}, {"th", "泰语"},
        {"vi", "越南语"}, {"id", "印尼语"}, {"ar", "阿拉伯语"},
        {"hi", "印地语"}, {"pl", "波兰语"}, {"uk", "乌克兰语"},
        {"sv", "瑞典语"}, {"el", "希腊语"}, {"he", "希伯来语"},
        {"zh", "中文"}, {"zh-CN", "中文"}, {"zh-cn", "中文"},
        {"zh-TW", "繁体中文"}, {"zh-tw", "繁体中文"},
    };
    const QString k = code.trimmed();
    return m.value(k, k.isEmpty() ? QString() : k);
}

static const QColor BG("#ffffff");
static const QColor FG("#1a1a1a");
static const QColor FG_DIM("#5a5a5a");
static const QColor FG_FAINT("#8a8a8a");
static const QColor CARD_BORDER("#d9d9d9");

Renderer::Renderer(QObject *parent) : QObject(parent)
{
}

void Renderer::configure(const QString &baseDir)
{
    m_mediaDir = baseDir + "/media";
    const QStringList candidates = {
        baseDir + "/fonts/remarkx-cjk.ttf",
        baseDir + "/remarkx-cjk.ttf",
        QStringLiteral("fonts/remarkx-cjk.ttf"),
        QStringLiteral("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        QStringLiteral("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"),
    };
    for (const QString &p : candidates) {
        if (QFile::exists(p)) {
            m_fontPath = p;
            break;
        }
    }
    if (m_fontPath.isEmpty()) {
        qWarning() << "Renderer: no CJK font found, Chinese may show as tofu";
        return;
    }
    const int fid = QFontDatabase::addApplicationFont(m_fontPath);
    const QStringList fams = QFontDatabase::applicationFontFamilies(fid);
    if (!fams.isEmpty())
        m_family = fams.first();
    qInfo() << "Renderer: font" << m_fontPath << "family" << m_family;
}

QFont Renderer::font(int pixel, bool bold) const
{
    const QPair<int, bool> key(pixel, bold);
    auto it = m_fonts.constFind(key);
    if (it != m_fonts.constEnd())
        return it.value();
    QFont f;
    if (!m_family.isEmpty())
        f.setFamily(m_family);
    else
        f.setFamily(QStringLiteral("sans-serif"));
    f.setPixelSize(pixel);
    f.setBold(bold);
    m_fonts.insert(key, f);
    return f;
}

qreal Renderer::textWidth(const QFont &f, const QString &text) const
{
    return QFontMetricsF(f).horizontalAdvance(text);
}

QString Renderer::ellipsize(const QFont &f, const QString &text,
                            qreal maxWidth) const
{
    if (textWidth(f, text) <= maxWidth)
        return text;
    QString t = text;
    while (!t.isEmpty() && textWidth(f, t + QStringLiteral("…")) > maxWidth)
        t.chop(1);
    return t + QStringLiteral("…");
}

QString Renderer::cleanText(const QString &text)
{
    static const QRegularExpression urlRe(QStringLiteral("https?://\\S+"));
    static const QRegularExpression emojiRe(QStringLiteral(
        "[\\x{1F000}-\\x{1FAFF}\\x{2600}-\\x{27BF}\\x{2B00}-\\x{2BFF}"
        "\\x{FE0F}\\x{200D}]"));
    static const QRegularExpression spaceRe(QStringLiteral("[ \\t]+"));
    QString s = text;
    s.remove(urlRe);
    s.remove(emojiRe);
    s.replace(spaceRe, QStringLiteral(" "));
    return s.trimmed();
}

QStringList Renderer::wrapText(const QFont &f, const QString &text,
                               qreal maxWidth, int maxLines,
                               bool *truncated, QList<int> *endOffsets) const
{
    QStringList lines;
    if (endOffsets)
        endOffsets->clear();
    if (truncated)
        *truncated = false;
    static const QRegularExpression unitRe(QStringLiteral(
        "[A-Za-z0-9]+(?:['\\x{2019}\\-][A-Za-z0-9]+)*|\\s+|\\S"));
    static const QString closing(
        QStringLiteral("，。、；：！？）》〉」』】〕”’…％℃"));
    static const QRegularExpression trailingWs(QStringLiteral("\\s+$"));

    auto rstrip = [&](QString s) {
        s.replace(trailingWs, QString());
        return s;
    };
    // endOff 是该行在原文 text 中的结束偏移：行尾被悬挂丢弃的空格/标点之外，
    // 下一个字真正开始的位置，续排时 text.mid(endOff) 即为剩余原文。
    auto emitLine = [&](QString line, int endOff) {
        line = rstrip(line);
        if (!line.isEmpty()) {
            lines.append(line);
            if (endOffsets)
                endOffsets->append(endOff);
        } else if (lines.isEmpty() || !lines.last().isEmpty()) {
            lines.append(QString());
            if (endOffsets)
                endOffsets->append(endOff);
        }
    };

    int paraStart = 0;
    const QStringList paras = text.split(QLatin1Char('\n'));
    for (const QString &raw : paras) {
        const int paraEnd = paraStart + raw.length();
        if (raw.trimmed().isEmpty()) {
            emitLine(QString(), paraEnd);
            paraStart = paraEnd + 1;
            if (lines.size() >= maxLines + 1)
                break;
            continue;
        }
        QString line;
        int lineEnd = paraStart;
        auto it = unitRe.globalMatch(raw);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString u = m.captured(0);
            const int uStart = paraStart + m.capturedStart(0);
            const int uEnd = uStart + u.length();
            if (textWidth(f, line + u) <= maxWidth) {
                line += u;
                lineEnd = uEnd;
                continue;
            }
            if (u.at(0).isSpace())
                continue;  // 行尾空白悬挂丢弃（lineEnd 保持本行内容末尾）
            if (closing.contains(u) && !line.isEmpty()) {
                line += u;  // 闭合标点挂到行尾
                lineEnd = uEnd;
                continue;
            }
            if (!line.trimmed().isEmpty()) {
                emitLine(line, lineEnd);
                line.clear();
                if (textWidth(f, u) <= maxWidth) {
                    line = u;
                    lineEnd = uEnd;
                    continue;
                }
            }
            // 超长原子（长英文单词）逐字符硬拆
            for (int ci = 0; ci < u.length(); ++ci) {
                const QChar ch = u.at(ci);
                if (!line.isEmpty() && textWidth(f, line + ch) > maxWidth) {
                    emitLine(line, lineEnd);
                    line.clear();
                }
                line += ch;
                lineEnd = uStart + ci + 1;
            }
        }
        emitLine(line, lineEnd);
        if (lines.size() >= maxLines + 1)
            break;
        paraStart = paraEnd + 1;
    }
    if (lines.size() > maxLines) {
        lines = lines.mid(0, maxLines);
        lines[maxLines - 1] = rstrip(lines[maxLines - 1]) + QStringLiteral(" …");
        if (truncated)
            *truncated = true;
        if (endOffsets && endOffsets->size() > lines.size())
            endOffsets->resize(lines.size());
    }
    return lines;
}

QString Renderer::fmtCount(int n)
{
    if (n >= 10000) {
        QString s = QString::number(n / 10000.0, 'f', 1);
        if (s.endsWith(QLatin1String(".0")))
            s.chop(2);
        return s + QStringLiteral("万");
    }
    return QString::number(n);
}

QString Renderer::statsText(const XTweet &t) const
{
    if (t.reposts == 0 && t.likes == 0 && t.replies == 0 && t.views == 0)
        return QString();
    return QStringLiteral("转 %1 · 赞 %2 · 评 %3")
               .arg(fmtCount(t.reposts))
               .arg(fmtCount(t.likes))
               .arg(fmtCount(t.replies))
           + QChar(1)
           + QStringLiteral("阅 %1").arg(fmtCount(t.views));
}

Renderer::Atom Renderer::imgAtom(const QVector<XTweet> &feed, int ti,
                                 bool isQuoted, int ind)
{
    Atom bad;
    bad.kind = -1;
    const XTweet &t = feed[ti];
    const QVector<XMedia> &media = isQuoted ? t.quoted.media : t.media;
    int mIdx = -1;
    for (int i = 0; i < media.size(); ++i) {
        if (!media[i].url.isEmpty()) {
            mIdx = i;
            break;
        }
    }
    if (mIdx < 0)
        return bad;
    const XMedia &m = media.at(mIdx);
    const int tw = TEXT_W - ind;
    int dw, dh;
    if (m.w > 0 && m.h > 0) {
        const qreal scale = qMin(qMin(qreal(tw) / m.w, qreal(IMG_MAX_H) / m.h),
                                 1.0);
        dw = qMax(1, int(m.w * scale));
        dh = qMax(1, int(m.h * scale));
    } else {
        // 元数据缺失：按 4:3 占位
        dh = IMG_MAX_H;
        dw = qMin(tw, int(IMG_MAX_H * 4 / 3));
    }
    int nMedia = 0;
    for (const XMedia &mm : media)
        if (!mm.url.isEmpty())
            ++nMedia;
    Atom a;
    a.kind = isQuoted ? Op::QImg : Op::Img;
    a.h = dh + IMG_GAP;
    a.tweetIndex = ti;
    a.isQuoted = isQuoted;
    a.mediaIndex = mIdx;
    a.video = m.video;
    a.nMedia = nMedia;
    a.ind = ind;
    a.dw = dw;
    a.dh = dh;
    return a;
}

Renderer::Atom Renderer::makeFloatCard(const XTweet &t, const Atom &img)
{
    remarkxSetCtx("render:makeFloatCard");
    Atom bad;
    bad.kind = -1;
    const QString text = cleanText(t.text);
    if (text.isEmpty())
        return bad;
    const int dw = img.dw, dh = img.dh;
    const int floatTextW = TEXT_W - dw - FLOAT_GAP;
    if (floatTextW < 200)
        return bad;   // 图相对太宽，环绕意义不大

    // 前 K 行按"图旁窄宽"包裹放在图片旁；剩余文字从原文续排，按整卡全宽
    // 包裹（不能把窄行用换行符拼起来再重排——那会按段保留窄行，图下留白）。
    const int K = (dh + TEXT_LH - 1) / TEXT_LH;   // ceil(dh / TEXT_LH)
    QList<int> offs;
    const QStringList allNarrow =
        wrapText(font(30), text, floatTextW, 100000, nullptr, &offs);
    QStringList beside = allNarrow.mid(0, K);
    QStringList below;
    if (allNarrow.size() > K) {
        QString rest = text.mid(offs.value(K - 1));
        // 去掉上一行行尾悬挂的空格（保留段落换行）
        static const QRegularExpression hangWs(QStringLiteral("^[ \\t]+"));
        rest.replace(hangWs, QString());
        below = wrapText(font(30), rest, TEXT_W, 100000);
    }

    // 统计行
    QString statsLeft, statsRight;
    const QString st = statsText(t);
    if (!st.isEmpty()) {
        const int sep = st.indexOf(QChar(1));
        statsLeft = st.left(sep);
        statsRight = st.mid(sep + 1);
    }

    // 卡片内"显示全文"按钮：长文（is_expandable）时内联到最后一个文本行末尾
    const QString btnLabel = t.isExpandable ? QStringLiteral("显示全文")
                                            : QString();
    if (!btnLabel.isEmpty()) {
        if (!below.isEmpty())
            attachInlineBtn(&below.last(), font(30), btnLabel, TEXT_W);
        else if (!beside.isEmpty())
            attachInlineBtn(&beside.last(), font(30), btnLabel, floatTextW);
    }

    const int besideH = beside.size() * TEXT_LH;
    const int belowH = below.size() * TEXT_LH;
    const int statsH = st.isEmpty() ? 0 : (STATS_GAP_TOP + STATS_H);
    const int contentH = qMax(dh, besideH) + belowH + 8;
    const int totalH = HEAD_H + 8 + contentH + statsH;
    if (totalH > FLOAT_MAX_H)
        return bad;   // 太高 → 回退普通布局（可跨页拆分）

    Atom fb;
    fb.kind = Op::FloatCard;
    fb.h = totalH;
    fb.tweetIndex = img.tweetIndex;
    fb.dw = dw;
    fb.dh = dh;
    fb.mediaIndex = img.mediaIndex;
    fb.video = img.video;
    fb.nMedia = img.nMedia;
    fb.floatLines = beside;
    fb.belowLines = below;
    fb.btnLabel = btnLabel;
    fb.statsLeft = statsLeft;
    fb.statsRight = statsRight;
    return fb;
}

Renderer::Atom Renderer::makeQFloatCard(const XTweet &t, const Atom &img)
{
    remarkxSetCtx("render:makeQFloatCard");
    Atom bad;
    bad.kind = -1;
    const QString qtext = cleanText(t.quoted.text);
    if (qtext.isEmpty())
        return bad;
    const int dw = img.dw, dh = img.dh;
    const int qtw = TEXT_W - QIND;
    const int floatTextW = qtw - dw - FLOAT_GAP;
    if (floatTextW < 200)
        return bad;

    // 引用文字：前 K 行按"图旁窄宽"包裹放在图片旁；剩余文字从原文续排，
    // 按引用块全宽包裹落到图片下方（消除图片下面的空白）。
    const int K = (dh + TEXT_LH_Q - 1) / TEXT_LH_Q;
    QList<int> offs;
    const QStringList allNarrow =
        wrapText(font(26), qtext, floatTextW, 100000, nullptr, &offs);
    QStringList beside = allNarrow.mid(0, K);
    QStringList below;
    if (allNarrow.size() > K) {
        QString rest = qtext.mid(offs.value(K - 1));
        static const QRegularExpression hangWs(QStringLiteral("^[ \\t]+"));
        rest.replace(hangWs, QString());
        below = wrapText(font(26), rest, qtw, 100000);
    }

    // 引用者自己的评论（纯转推时为空）
    QStringList comments;
    if (!t.isRetweet) {
        const QString text = cleanText(t.text);
        if (!text.isEmpty())
            comments = wrapText(font(30), text, TEXT_W, 100000);
    }

    // 统计行
    QString statsLeft, statsRight;
    const QString st = statsText(t);
    if (!st.isEmpty()) {
        const int sep = st.indexOf(QChar(1));
        statsLeft = st.left(sep);
        statsRight = st.mid(sep + 1);
    }

    const QString btnLabel = (t.isExpandable || t.quoted.isExpandable)
                                 ? QStringLiteral("显示全文")
                                 : QString();
    if (!btnLabel.isEmpty()) {
        if (!below.isEmpty())
            attachInlineBtn(&below.last(), font(26), btnLabel, qtw);
        else if (!beside.isEmpty())
            attachInlineBtn(&beside.last(), font(26), btnLabel, floatTextW);
    }

    const int commentH = comments.size() * TEXT_LH;
    const int floatH = qMax(dh, beside.size() * TEXT_LH_Q)
                       + below.size() * TEXT_LH_Q;
    const int statsH = st.isEmpty() ? 0 : (STATS_GAP_TOP + STATS_H);
    const int totalH = HEAD_H + commentH + QHEAD_H + 8 + floatH + statsH;
    if (totalH > FLOAT_MAX_H)
        return bad;

    Atom qf;
    qf.kind = Op::QFloatCard;
    qf.h = totalH;
    qf.tweetIndex = img.tweetIndex;
    qf.dw = dw;
    qf.dh = dh;
    qf.mediaIndex = img.mediaIndex;
    qf.video = img.video;
    qf.nMedia = img.nMedia;
    qf.commentLines = comments;
    qf.qname = t.quoted.authorName.isEmpty() ? QStringLiteral("?")
                                             : t.quoted.authorName;
    qf.qhandle = t.quoted.authorHandle;
    qf.floatLines = beside;
    qf.belowLines = below;
    qf.btnLabel = btnLabel;
    qf.statsLeft = statsLeft;
    qf.statsRight = statsRight;
    return qf;
}

QVector<Renderer::Atom> Renderer::buildAtoms(const QVector<XTweet> &feed,
                                             int ti)
{
    remarkxSetCtx("render:buildAtoms");
    const XTweet &t = feed[ti];
    QVector<Atom> atoms;

    const bool hasQuoted = !t.quoted.authorName.isEmpty()
                           || !t.quoted.text.trimmed().isEmpty()
                           || !t.quoted.media.isEmpty();

    // 竖版图卡片（FloatCard）：简单卡片（无引用/转推、有竖版主图、有正文）尝试
    // "右上图 + 文字环绕"布局，消除竖图两侧大片空白。
    // FloatCard 是原子块（头部+图+文字+统计一体），不可拆分，随列整体移动。
    if (!hasQuoted && !t.isRetweet) {
        const Atom img = imgAtom(feed, ti, false, 0);
        if (img.kind == Op::Img && img.dh > img.dw) {
            Atom fb = makeFloatCard(t, img);
            if (fb.kind == Op::FloatCard) {
                atoms.append(fb);
                return atoms;
            }
        }
    }

    // 引用/转推帖（QFloatCard）：引用块带竖版图时整卡
    // "头部+评论+引用作者+右上图环绕"一体排版，消除引用图两侧空白。
    // 纯转推的 t.media 与原帖媒体相同，不视为"自己的媒体"；
    // 引用评论自带媒体时布局复杂，回退普通排版。
    if (hasQuoted) {
        bool hasOwnMedia = false;
        if (!t.isRetweet)
            for (const XMedia &m : t.media)
                if (!m.url.isEmpty()) { hasOwnMedia = true; break; }
        if (!hasOwnMedia) {
            const Atom qimg = imgAtom(feed, ti, true, QIND);
            if (qimg.kind == Op::QImg && qimg.dh > qimg.dw) {
                Atom qf = makeQFloatCard(t, qimg);
                if (qf.kind == Op::QFloatCard) {
                    atoms.append(qf);
                    return atoms;
                }
            }
        }
    }

    Atom head;
    head.kind = Op::Head;
    head.h = HEAD_H;
    head.tweetIndex = ti;
    atoms.append(head);

    // 卡片显示 API 默认文本（译文或 full_text 预览），但统一限制行数
    // （"不需要总是显示全文"，长文/长译文都截断）。
    // "显示全文"触发：卡片被行数截断，或（非译文时）显示的是长推文预览
    // （legacy.full_text）而完整文本在 note_tweet.text。
    bool needFullText = false;

    if (hasQuoted) {
        if (!t.isRetweet) {
            const QString text = cleanText(t.text);
            if (!text.isEmpty()) {
                // 译文没有"预览"字段（translation 永远是完整译文），只能截断；
                // 非译文直接用 full_text（API 自带的长文预览），不再自行截断
                const int maxLines = t.translated ? BODY_MAX_LINES : 100000;
                bool truncated = false;
                const QStringList lns = wrapText(font(30), text, TEXT_W,
                                                 maxLines, &truncated);
                needFullText = needFullText || truncated;
                for (const QString &ln : lns) {
                    Atom a;
                    a.kind = Op::Line;
                    a.h = TEXT_LH;
                    a.text = ln;
                    atoms.append(a);
                }
            }
            Atom a = imgAtom(feed, ti, false, 0);
            if (a.kind == Op::Img)
                atoms.append(a);
        }
        Atom qh;
        qh.kind = Op::QHead;
        qh.h = QHEAD_H;
        qh.qname = t.quoted.authorName.isEmpty() ? QStringLiteral("?")
                                                 : t.quoted.authorName;
        qh.qhandle = t.quoted.authorHandle;
        atoms.append(qh);
        const QString qtext = cleanText(t.quoted.text);
        if (!qtext.isEmpty()) {
            const int qmax = t.quoted.translated ? QUOTED_MAX_LINES : 100000;
            bool truncated = false;
            const QStringList lns = wrapText(font(26), qtext, TEXT_W - QIND,
                                             qmax, &truncated);
            needFullText = needFullText || truncated;
            for (const QString &ln : lns) {
                Atom a;
                a.kind = Op::QLine;
                a.h = TEXT_LH_Q;
                a.text = ln;
                atoms.append(a);
            }
        }
        Atom a = imgAtom(feed, ti, true, QIND);
        if (a.kind == Op::QImg)
            atoms.append(a);
    } else {
        const QString text = cleanText(t.text);
        if (!text.isEmpty()) {
            const int maxLines = t.translated ? BODY_MAX_LINES : 100000;
            bool truncated = false;
            const QStringList lns = wrapText(font(30), text, TEXT_W,
                                             maxLines, &truncated);
            needFullText = needFullText || truncated;
            for (const QString &ln : lns) {
                Atom a;
                a.kind = Op::Line;
                a.h = TEXT_LH;
                a.text = ln;
                atoms.append(a);
            }
        }
        Atom a = imgAtom(feed, ti, false, 0);
        if (a.kind == Op::Img)
            atoms.append(a);
    }

    // API 标记的长文（note_tweet.is_expandable）：完整文本仍在 note_tweet 里
    if (t.isExpandable)
        needFullText = true;
    if (hasQuoted && t.quoted.isExpandable)
        needFullText = true;

    // 有完整文本可看 → "显示全文"内联到最后一个文本行末尾（不另占一行）
    if (needFullText) {
        bool attached = false;
        for (int i = atoms.size() - 1; i >= 0; --i) {
            Atom &a = atoms[i];
            if (a.kind == Op::Line) {
                attachInlineBtn(&a.text, font(30), QStringLiteral("显示全文"),
                                TEXT_W);
                a.btnSuffix = QStringLiteral("显示全文");
                attached = true;
                break;
            }
            if (a.kind == Op::QLine) {
                attachInlineBtn(&a.text, font(26), QStringLiteral("显示全文"),
                                TEXT_W - QIND);
                a.btnSuffix = QStringLiteral("显示全文");
                attached = true;
                break;
            }
        }
        // 兜底：找不到文本行（极端情况）时仍独立成行
        if (!attached) {
            Atom a;
            a.kind = Op::FullText;
            a.h = TEXT_LH;
            a.text = QStringLiteral("显示全文");
            atoms.append(a);
        }
    }

    const QString st = statsText(t);
    if (!st.isEmpty()) {
        Atom a;
        a.kind = Op::Stats;
        a.h = STATS_GAP_TOP + STATS_H;
        const int sep = st.indexOf(QChar(1));
        a.statsLeft = st.left(sep);
        a.statsRight = st.mid(sep + 1);
        atoms.append(a);
    }
    return atoms;
}

QVector<RenderPage> Renderer::paginate(const QVector<XTweet> &feed)
{
    remarkxSetCtx("render:paginate");
    QVector<RenderPage> pages;
    pages.append(RenderPage());

    int p = 0, col = 0, y = TOP_Y;
    auto colX = [](int c) { return MARGIN + c * (COL_W + COL_GUTTER); };
    auto ensurePage = [&]() {
        while (pages.size() <= p)
            pages.append(RenderPage());
    };
    auto freeSpace = [&]() { return BOTTOM_Y - y; };

    int chunkIdx = -1;
    bool chunkWasSplit = false;
    auto close = [&]() {
        if (chunkIdx >= 0) {
            pages[p].chunks[chunkIdx].hasCont = true;
            chunkWasSplit = true;
        }
        chunkIdx = -1;
    };
    auto openContNeeded = [&]() { return PAD_TOP + (chunkWasSplit ? 24 : 0); };
    auto openChunk = [&](bool isCont, int ti) {
        ensurePage();
        RenderChunk c;
        c.rect = QRect(colX(col), y, COL_W, 0);
        c.isCont = isCont;
        c.tweetIndex = ti;
        pages[p].chunks.append(c);
        chunkIdx = pages[p].chunks.size() - 1;
        y += PAD_TOP;
        pages[p].chunks[chunkIdx].rect.setHeight(
            y - pages[p].chunks[chunkIdx].rect.y());
    };
    auto grow = [&](int delta) -> int {
        const int y0 = y;
        y += delta;
        pages[p].chunks[chunkIdx].rect.setHeight(
            y - pages[p].chunks[chunkIdx].rect.y());
        return y0;
    };
    auto jump = [&]() {
        y = TOP_Y;
        if (col == 0) {
            col = 1;
        } else {
            col = 0;
            p += 1;
        }
        ensurePage();
    };

    // 已被补位提前排掉的帖子（主循环跳过，避免重复排版）
    QSet<int> consumed;

    for (int ti = 0; ti < feed.size(); ++ti) {
        if (consumed.contains(ti))
            continue;
        const XTweet &t = feed.at(ti);
        bool hasMedia = false, hasQMedia = false;
        for (const XMedia &m : t.media)
            if (!m.url.isEmpty()) { hasMedia = true; break; }
        for (const XMedia &m : t.quoted.media)
            if (!m.url.isEmpty()) { hasQMedia = true; break; }
        const bool hasQuoted = !t.quoted.authorName.isEmpty()
                               || !t.quoted.text.trimmed().isEmpty()
                               || hasQMedia;
        if (t.text.trimmed().isEmpty() && t.comment.trimmed().isEmpty()
                && !hasMedia && !hasQuoted && !hasQMedia)
            continue;

        QVector<Atom> atoms = buildAtoms(feed, ti);
        chunkIdx = -1;
        chunkWasSplit = false;
        int lastKind = Op::Head;
        int ai = 0;

        auto placeAtom = [&](Atom &a, int pti) -> bool {
            const int extra = (chunkIdx >= 0) ? 0 : openContNeeded();
            if (freeSpace() < a.h + extra)
                return false;
            if (chunkIdx < 0) {
                const bool cont = chunkWasSplit
                                  && (a.kind == Op::Line || a.kind == Op::QLine
                                      || a.kind == Op::FullText);
                openChunk(cont, pti);
            }
            RenderChunk &ch = pages[p].chunks[chunkIdx];
            Op op;
            op.kind = a.kind;
            switch (a.kind) {
            case Op::Head:
                op.y = grow(HEAD_H);
                break;
            case Op::FloatCard: {
                op.y = grow(a.h);
                // 右上图槽位
                const int y0 = op.y + HEAD_H + 8;
                const int imgX = colX(col) + PAD + TEXT_W - a.dw;
                ImageSlot s;
                s.x = imgX;
                s.y = y0;
                s.w = a.dw;
                s.h = a.dh;
                s.tweetIndex = a.tweetIndex;
                s.isQuoted = false;
                s.mediaIndex = a.mediaIndex;
                s.video = a.video;
                s.nMedia = a.nMedia;
                pages[p].images.append(s);
                op.slotIndex = pages[p].images.size() - 1;
                op.imgW = a.dw;
                op.imgH = a.dh;
                op.floatLines = a.floatLines;
                op.belowLines = a.belowLines;
                op.btnLabel = a.btnLabel;
                op.statsLeft = a.statsLeft;
                op.statsRight = a.statsRight;
                // 行尾内联"显示全文"按钮热区（挂在最后一个文本行）
                if (!a.btnLabel.isEmpty()) {
                    const int besideH = a.floatLines.size() * TEXT_LH;
                    const int belowH = a.belowLines.size() * TEXT_LH;
                    const QFont lf = font(30);
                    int bx, btnY;
                    if (!a.belowLines.isEmpty()) {
                        btnY = op.y + HEAD_H + 8 + qMax(a.dh, besideH)
                               + belowH - TEXT_LH;
                        bx = colX(col) + PAD
                             + int(textWidth(lf, a.belowLines.last())) + 10;
                    } else {
                        btnY = op.y + HEAD_H + 8 + besideH - TEXT_LH;
                        bx = colX(col) + PAD
                             + int(textWidth(lf, a.floatLines.last())) + 10;
                    }
                    TextButton btn;
                    btn.rect = QRect(bx, btnY,
                                     int(textWidth(font(26, true), a.btnLabel)),
                                     TEXT_LH);
                    btn.tweetIndex = pti;
                    pages[p].buttons.append(btn);
                }
                break;
            }
            case Op::QFloatCard: {
                op.y = grow(a.h);
                // 引用图槽位（右上，位于引用作者行之后）
                const int commentH = a.commentLines.size() * TEXT_LH;
                const int imgY = op.y + HEAD_H + commentH + QHEAD_H + 8;
                const int imgX = colX(col) + PAD + TEXT_W - a.dw;
                ImageSlot s;
                s.x = imgX;
                s.y = imgY;
                s.w = a.dw;
                s.h = a.dh;
                s.tweetIndex = a.tweetIndex;
                s.isQuoted = true;
                s.mediaIndex = a.mediaIndex;
                s.video = a.video;
                s.nMedia = a.nMedia;
                pages[p].images.append(s);
                op.slotIndex = pages[p].images.size() - 1;
                op.imgW = a.dw;
                op.imgH = a.dh;
                op.commentLines = a.commentLines;
                op.qname = a.qname;
                op.qhandle = a.qhandle;
                op.floatLines = a.floatLines;
                op.belowLines = a.belowLines;
                op.btnLabel = a.btnLabel;
                op.statsLeft = a.statsLeft;
                op.statsRight = a.statsRight;
                // 行尾内联"显示全文"按钮热区（挂在最后一个引用文本行）
                if (!a.btnLabel.isEmpty()) {
                    const int besideH = a.floatLines.size() * TEXT_LH_Q;
                    const int belowH = a.belowLines.size() * TEXT_LH_Q;
                    const QFont lf = font(26);
                    int bx, btnY;
                    if (!a.belowLines.isEmpty()) {
                        btnY = imgY + qMax(a.dh, besideH) + belowH
                               - TEXT_LH_Q;
                        bx = colX(col) + PAD + QIND
                             + int(textWidth(lf, a.belowLines.last())) + 10;
                    } else {
                        btnY = imgY + besideH - TEXT_LH_Q;
                        bx = colX(col) + PAD + QIND
                             + int(textWidth(lf, a.floatLines.last())) + 10;
                    }
                    TextButton btn;
                    btn.rect = QRect(bx, btnY,
                                     int(textWidth(font(26, true), a.btnLabel)),
                                     TEXT_LH_Q);
                    btn.tweetIndex = pti;
                    pages[p].buttons.append(btn);
                }
                break;
            }
            case Op::Line:
            case Op::QLine:
                op.y = grow(a.h);
                op.text = a.text;
                op.btnSuffix = a.btnSuffix;
                // 行尾内联"显示全文"按钮热区
                if (!a.btnSuffix.isEmpty()) {
                    const bool isQ = (a.kind == Op::QLine);
                    const QFont lf = isQ ? font(26) : font(30);
                    const int tx = colX(col) + PAD + (isQ ? QIND : 0);
                    const int bx = tx + int(textWidth(lf, a.text)) + 10;
                    TextButton btn;
                    btn.rect = QRect(bx, op.y,
                                     int(textWidth(font(26, true), a.btnSuffix)),
                                     a.h);
                    btn.tweetIndex = pti;
                    pages[p].buttons.append(btn);
                }
                break;
            case Op::FullText:
                op.y = grow(a.h);
                op.text = a.text;
                // 记录"显示全文"按钮热区（用于点按打开全文全屏）
                {
                    TextButton btn;
                    btn.rect = QRect(colX(col) + PAD, op.y,
                                     int(textWidth(font(26, true), a.text)),
                                     a.h);
                    btn.tweetIndex = pti;
                    pages[p].buttons.append(btn);
                }
                break;
            case Op::QHead:
                op.y = grow(a.h);
                op.qname = a.qname;
                op.qhandle = a.qhandle;
                break;
            case Op::Img:
            case Op::QImg: {
                op.y = grow(a.h);
                const int tw = TEXT_W - a.ind;
                const int ix = colX(col) + PAD + a.ind + (tw - a.dw) / 2;
                ImageSlot s;
                s.x = ix;
                s.y = op.y;
                s.w = a.dw;
                s.h = a.dh;
                s.tweetIndex = a.tweetIndex;
                s.isQuoted = a.isQuoted;
                s.mediaIndex = a.mediaIndex;
                s.video = a.video;
                s.nMedia = a.nMedia;
                pages[p].images.append(s);
                op.slotIndex = pages[p].images.size() - 1;
                break;
            }
            case Op::Stats:
                op.y = grow(a.h);
                op.statsLeft = a.statsLeft;
                op.statsRight = a.statsRight;
                break;
            }
            ch.ops.append(op);
            lastKind = (a.kind == Op::FloatCard || a.kind == Op::QFloatCard)
                           ? Op::Stats
                           : a.kind;
            return true;
        };

        // 栏底补位：当前帖在剩余高度内放不下（多为图片卡）时，若空白较大，
        // 把后面某个纯文本帖整卡提前填入空白；当前帖其余部分照常续排下一栏。
        // 不回退当前帖已排的头部/文本（否则把它整体搬去下一栏会挤压下一栏，
        // 可能在那里留下更大的新空白）。
        auto tryFillGap = [&](int curTi) -> bool {
            // 只在该帖真正留下的可见空白较大时才补位（文本截断的小缝不值得调序）
            if (freeSpace() < FILL_MIN_H)
                return false;
            const int gap = freeSpace();
            const int minNeed = qMax(FILL_MIN_CARD, gap * 3 / 5);
            for (int fi = curTi + 1; fi < feed.size() && fi - curTi <= FILL_SCAN;
                 ++fi) {
                if (consumed.contains(fi))
                    continue;
                const XTweet &ft = feed.at(fi);
                bool hasMedia = false, hasQMedia = false;
                for (const XMedia &m : ft.media)
                    if (!m.url.isEmpty()) { hasMedia = true; break; }
                for (const XMedia &m : ft.quoted.media)
                    if (!m.url.isEmpty()) { hasQMedia = true; break; }
                const bool hasQuoted = !ft.quoted.authorName.isEmpty()
                                       || !ft.quoted.text.trimmed().isEmpty()
                                       || hasQMedia;
                if (ft.text.trimmed().isEmpty() && ft.comment.trimmed().isEmpty()
                        && !hasMedia && !hasQuoted && !hasQMedia)
                    continue;
                QVector<Atom> fats = buildAtoms(feed, fi);
                // 只补纯文本/纯引用文本卡（可整卡放入，不涉及不可拆分的图片块）
                bool splittable = true;
                for (const Atom &a : fats) {
                    if (a.kind == Op::Img || a.kind == Op::QImg
                        || a.kind == Op::FloatCard || a.kind == Op::QFloatCard) {
                        splittable = false;
                        break;
                    }
                }
                if (!splittable)
                    continue;
                const int pad = (fats.last().kind == Op::Stats)
                                    ? CARD_GAP : (PAD_BOTTOM + CARD_GAP);
                int need = PAD_TOP + pad;
                for (const Atom &a : fats)
                    need += a.h;
                if (need < minNeed || need > gap)
                    continue;

                // 补位帖整卡放入空白（先验证过需要高度 ≤ 空白，不会再失败）。
                // 保存/恢复 chunkWasSplit：补位卡是全新卡，但当前帖续排仍需它。
                const bool savedSplit = chunkWasSplit;
                chunkIdx = -1;
                chunkWasSplit = false;
                int fai = 0;
                while (fai < fats.size()) {
                    if (!placeAtom(fats[fai], fi))
                        break;
                    ++fai;
                }
                if (chunkIdx >= 0 && y + pad <= BOTTOM_Y)
                    grow(pad);
                consumed.insert(fi);
                chunkIdx = -1;
                chunkWasSplit = savedSplit;
                return true;
            }
            return false;
        };

        while (ai < atoms.size()) {
            if (!placeAtom(atoms[ai], ti)) {
                close();
                tryFillGap(ti);
                jump();
            } else {
                ++ai;
            }
        }
        const int pad = (lastKind == Op::Stats) ? CARD_GAP
                                                : (PAD_BOTTOM + CARD_GAP);
        if (chunkIdx >= 0 && y + pad <= BOTTOM_Y)
            grow(pad);
    }

    QVector<RenderPage> out;
    for (RenderPage &pg : pages)
        if (!pg.chunks.isEmpty())
            out.append(pg);
    return out;
}

QImage Renderer::renderPage(const QVector<XTweet> &feed,
                            const QVector<RenderPage> &pages, int pageIndex,
                            bool withPhotos)
{
    remarkxSetCtx("render:renderPage");
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(BG);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (pages.isEmpty() || pageIndex < 0 || pageIndex >= pages.size()) {
        p.setFont(font(32));
        p.setPen(FG_DIM);
        p.drawText(QRectF(0, 0, W, H), Qt::AlignCenter,
                   QStringLiteral("还没有内容"));
        p.end();
        return img;
    }
    const RenderPage &page = pages.at(pageIndex);
    for (const RenderChunk &chunk : page.chunks)
        drawCard(p, feed, chunk, page, withPhotos);
    p.end();
    return img;
}

void Renderer::drawCardBorder(QPainter &p, int x, int y0, int y1, int w,
                              bool top, bool bottom) const
{
    const int l = x + 1, r = x + w - 1, rad = 10;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(CARD_BORDER, 2));
    p.drawLine(l, y0, l, y1);
    p.drawLine(r, y0, r, y1);
    // QPainter 角度约定：0°=右、90°=上、180°=左、270°=下（逆时针为正）。
    if (top) {
        p.drawLine(l, y0, r, y0);
        p.drawArc(QRectF(l, y0, 2 * rad, 2 * rad), 90 * 16, 90 * 16);
        p.drawArc(QRectF(r - 2 * rad, y0, 2 * rad, 2 * rad), 0 * 16, 90 * 16);
    }
    if (bottom) {
        p.drawLine(l, y1, r, y1);
        p.drawArc(QRectF(l, y1 - 2 * rad, 2 * rad, 2 * rad), 180 * 16, 90 * 16);
        p.drawArc(QRectF(r - 2 * rad, y1 - 2 * rad, 2 * rad, 2 * rad), 270 * 16,
                  90 * 16);
    }
}

QString Renderer::absTime(const QString &createdAt)
{
    QString s = createdAt;
    s.remove(QRegularExpression(QStringLiteral("\\+\\d{4} ")));
    QDateTime dt = QDateTime::fromString(s, QStringLiteral("ddd MMM d HH:mm:ss yyyy"));
    dt.setTimeZone(QTimeZone::UTC);
    const QDateTime local = dt.toLocalTime();
    if (!local.isValid())
        return QString();
    if (local.date().year() == QDate::currentDate().year())
        return local.toString(QStringLiteral("MM-dd HH:mm"));
    return local.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QImage Renderer::avatar(const XTweet &t) const
{
    if (t.avatar.isEmpty())
        return {};
    const QString full = m_mediaDir + "/" + t.avatar;
    auto it = m_avatarCache.constFind(full);
    if (it != m_avatarCache.constEnd()) {
        // 命中即移到末尾（LRU）
        m_avatarOrder.removeAll(full);
        m_avatarOrder.append(full);
        return it.value();
    }
    QImage src(full);
    if (src.isNull())
        return {};
    QImage img = src.scaled(AVATAR_D, AVATAR_D, Qt::IgnoreAspectRatio,
                            Qt::FastTransformation)
                     .convertToFormat(QImage::Format_RGB32);
    QImage mask(AVATAR_D, AVATAR_D, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setBrush(Qt::white);
        mp.setPen(Qt::NoPen);
        mp.drawEllipse(0, 0, AVATAR_D - 1, AVATAR_D - 1);
    }
    QImage out(AVATAR_D, AVATAR_D, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::white);
    {
        QPainter op(&out);
        op.setRenderHint(QPainter::Antialiasing, true);
        op.drawImage(0, 0, img);
        op.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        op.drawImage(0, 0, mask);
    }
    lruInsert(m_avatarCache, m_avatarOrder, full, out, 512);
    return out;
}

void Renderer::drawHead(QPainter &p, const XTweet &t, int px, int py)
{
    const QFont fn = font(26, true), fd = font(24), fm = font(22);
    const QFont fav = font(30, true);
    const QString name = t.authorName.isEmpty() ? QStringLiteral("?")
                                                : t.authorName;
    const QString handle = QStringLiteral("@") + t.authorHandle;
    int tx = px;
    const QImage av = avatar(t);
    if (!av.isNull()) {
        p.drawImage(px, py + 2, av);
        tx = px + AVATAR_D + AVATAR_GAP;
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e8e8e8"));
        p.drawEllipse(px, py + 2, AVATAR_D, AVATAR_D);
        p.setFont(fav);
        p.setPen(QColor("#666666"));
        p.drawText(QRectF(px, py + 2, AVATAR_D, AVATAR_D), Qt::AlignCenter,
                   name.left(1));
        tx = px + AVATAR_D + AVATAR_GAP;
    }
    const int nameW = TEXT_W - AVATAR_D - AVATAR_GAP;
    const QString combo = name + QStringLiteral("  ") + handle;
    if (textWidth(fn, combo) <= nameW) {
        p.setFont(fn);
        p.setPen(FG);
        p.drawText(QRectF(tx, py + 2, nameW, NAME_H), Qt::AlignLeft | Qt::AlignTop,
                   name);
        const int wn = int(textWidth(fn, name));
        p.setFont(fd);
        p.setPen(FG_DIM);
        p.drawText(QRectF(tx + wn + 10, py + 7, nameW - wn - 10, META_H),
                   Qt::AlignLeft | Qt::AlignTop, handle);
    } else {
        p.setFont(fd);
        p.setPen(FG_DIM);
        p.drawText(QRectF(tx, py + 2, nameW, NAME_H), Qt::AlignLeft | Qt::AlignTop,
                   ellipsize(fd, combo, nameW));
    }

    const int py2 = py + NAME_H + 6;
    QStringList metas;
    if (t.isRetweet)
        metas << QStringLiteral("转发了");
    const QString at = absTime(t.createdAt);
    if (!at.isEmpty())
        metas << at;
    int videos = 0, imgs = 0;
    for (const XMedia &m : t.media) {
        if (m.url.isEmpty())
            continue;
        if (m.video)
            ++videos;
        else
            ++imgs;
    }
    if (videos)
        metas << QStringLiteral("▶ 视频");
    if (imgs)
        metas << QString::number(imgs) + QStringLiteral(" 图");
    // 翻译提示：显示的是译文时标注"译自 XX"
    if (t.translated && !t.sourceLang.isEmpty()) {
        const QString zh = langName(t.sourceLang);
        if (!zh.isEmpty())
            metas << QStringLiteral("译自 %1").arg(zh);
    }
    p.setFont(fm);
    p.setPen(FG_FAINT);
    p.drawText(QRectF(tx, py2, nameW, META_H), Qt::AlignLeft | Qt::AlignTop,
               ellipsize(fm, metas.join(QStringLiteral(" · ")), nameW));
}

void Renderer::drawLine(QPainter &p, const QString &text, int x, int y,
                        const QFont &f, const QColor &c) const
{
    p.setFont(f);
    p.setPen(c);
    p.drawText(QRectF(x, y, TEXT_W, 4000), Qt::AlignLeft | Qt::AlignTop, text);
}

void Renderer::attachInlineBtn(QString *line, const QFont &lineFont,
                               const QString &btn, qreal lineMaxWidth) const
{
    const QFont bf = font(26, true);
    const int gap = 10;
    const qreal avail = lineMaxWidth - textWidth(bf, btn) - gap;
    if (avail <= 0)
        return;
    if (textWidth(lineFont, *line) > avail)
        *line = ellipsize(lineFont, *line, avail);
}

void Renderer::drawInlineBtn(QPainter &p, int x, int y, const QString &btn,
                             int lineH) const
{
    const QFont bf = font(26, true);
    p.setFont(bf);
    p.setPen(QColor("#1a6b9c"));
    p.drawText(QRectF(x, y, TEXT_W, lineH), Qt::AlignLeft | Qt::AlignTop, btn);
    const int tw = int(textWidth(bf, btn));
    p.drawLine(x, y + lineH - 8, x + tw, y + lineH - 8);
}

void Renderer::drawQuotedBar(QPainter &p, int x, int y0, int y1) const
{
    p.setPen(QPen(FG_FAINT, 2));
    p.drawLine(x, y0, x, y1);
}

void Renderer::drawPlaceholder(QPainter &p, int ix, int py, int dw, int dh) const
{
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#ececec"));
    p.drawRect(QRect(ix, py, dw, dh));
}

void Renderer::drawPhoto(QPainter &p, const XTweet &t, const ImageSlot &s,
                         bool withPhotos)
{
    const QVector<XMedia> *list = s.isQuoted ? &t.quoted.media : &t.media;
    const XMedia *m = (list && s.mediaIndex < list->size())
                          ? &(*list).at(s.mediaIndex)
                          : nullptr;
    const int ix = s.x, py = s.y, dw = s.w, dh = s.h;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(CARD_BORDER, 2));
    p.drawRect(QRect(ix - 3, py - 3, dw + 6, dh + 6));
    if (!withPhotos || !m || m->path.isEmpty()) {
        drawPlaceholder(p, ix, py, dw, dh);
        return;
    }
    const QString full = m_mediaDir + "/" + m->path;
    const QString key = full + QLatin1Char('@') + QString::number(dw)
                        + QLatin1Char('x') + QString::number(dh);
    auto it = m_photoCache.constFind(key);
    QImage photo;
    if (it != m_photoCache.constEnd()) {
        photo = it.value();
        m_photoOrder.removeAll(key);
        m_photoOrder.append(key);
    } else {
        QImage src(full);
        if (src.isNull()) {
            drawPlaceholder(p, ix, py, dw, dh);
            return;
        }
        photo = src.scaled(dw, dh, Qt::IgnoreAspectRatio,
                           Qt::FastTransformation)
                     .convertToFormat(QImage::Format_RGB32);
        lruInsert(m_photoCache, m_photoOrder, key, photo, 128);
    }
    p.drawImage(ix, py, photo);
    if (s.video) {
        const int cx = ix + dw / 2, cy = py + dh / 2, rr = 40;
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawEllipse(cx - rr, cy - rr, rr * 2, rr * 2);
        p.setBrush(Qt::white);
        QPolygonF tri;
        tri << QPointF(cx - 13, cy - 22) << QPointF(cx - 13, cy + 22)
            << QPointF(cx + 26, cy);
        p.drawPolygon(tri);
    }
    if (s.nMedia > 1) {
        const QFont tf = font(20, true);
        const QString tag = QStringLiteral("共 %1 图").arg(s.nMedia);
        const int tgw = int(textWidth(tf, tag));
        const int tx = ix + dw - tgw - 22, ty = py + dh - 36;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#333333"));
        p.drawRoundedRect(QRectF(tx - 8, ty - 4, tgw + 16, 32), 8, 8);
        p.setFont(tf);
        p.setPen(Qt::white);
        p.drawText(QRectF(tx, ty, tgw + 2, 24), Qt::AlignLeft | Qt::AlignTop,
                   tag);
    }
}

void Renderer::drawStats(QPainter &p, const Op &op,
                         const RenderChunk &chunk) const
{
    const int yy = op.y + STATS_GAP_TOP;
    const QFont fs = font(22);
    p.setFont(fs);
    p.setPen(FG_FAINT);
    const int px = chunk.rect.x() + PAD;
    if (!op.statsRight.isEmpty()) {
        const int rw = int(textWidth(fs, op.statsRight));
        const int rx = chunk.rect.x() + chunk.rect.width() - PAD - rw;
        const int lw = TEXT_W - rw - 14;
        QString left = op.statsLeft;
        if (lw > 0 && textWidth(fs, left) > lw)
            left = ellipsize(fs, left, lw);
        p.drawText(QRectF(rx, yy, rw + 4, STATS_H), Qt::AlignLeft | Qt::AlignTop,
                   op.statsRight);
        if (lw > 0)
            p.drawText(QRectF(px, yy, lw, STATS_H), Qt::AlignLeft | Qt::AlignTop,
                       left);
    } else {
        p.drawText(QRectF(px, yy, TEXT_W, STATS_H), Qt::AlignLeft | Qt::AlignTop,
                   op.statsLeft);
    }
}

void Renderer::drawCard(QPainter &p, const QVector<XTweet> &feed,
                        const RenderChunk &chunk, const RenderPage &page,
                        bool withPhotos)
{
    // 防越界：feed 重建后可能残留旧索引
    if (chunk.tweetIndex < 0 || chunk.tweetIndex >= feed.size())
        return;
    const QRect &r = chunk.rect;
    const XTweet &t = feed.at(chunk.tweetIndex);
    const int px = r.x() + PAD;
    drawCardBorder(p, r.x(), r.y(), r.y() + qMax(r.height(), 4), r.width(),
                   !chunk.isCont, !chunk.hasCont);
    for (const Op &op : chunk.ops) {
        switch (op.kind) {
        case Op::Head:
            drawHead(p, t, px, op.y);
            break;
        case Op::Line: {
            drawLine(p, op.text, px, op.y, font(30), FG);
            if (!op.btnSuffix.isEmpty())
                drawInlineBtn(p, px + int(textWidth(font(30), op.text)) + 10,
                              op.y, op.btnSuffix, TEXT_LH);
            break;
        }
        case Op::QLine: {
            drawQuotedBar(p, r.x() + 10, op.y - 2, op.y + TEXT_LH_Q + 2);
            drawLine(p, op.text, px + QIND, op.y, font(26), FG_DIM);
            if (!op.btnSuffix.isEmpty())
                drawInlineBtn(p, px + QIND
                                    + int(textWidth(font(26), op.text)) + 10,
                              op.y, op.btnSuffix, TEXT_LH_Q);
            break;
        }
        case Op::Img:
            if (op.slotIndex < 0 || op.slotIndex >= page.images.size())
                break;
            drawPhoto(p, t, page.images.at(op.slotIndex), withPhotos);
            break;
        case Op::QImg: {
            if (op.slotIndex < 0 || op.slotIndex >= page.images.size())
                break;
            const ImageSlot &s = page.images.at(op.slotIndex);
            drawQuotedBar(p, r.x() + 10, s.y - 2, s.y + s.h + IMG_GAP + 2);
            drawPhoto(p, t, s, withPhotos);
            break;
        }
        case Op::QHead: {
            drawQuotedBar(p, r.x() + 10, op.y - 2, op.y + QHEAD_H + 2);
            const int qx = px + QIND;
            const QFont fn = font(24, true), fh = font(22);
            const QString combo = op.qname + QStringLiteral("  @") + op.qhandle;
            if (textWidth(fn, combo) <= TEXT_W - QIND) {
                p.setFont(fn);
                p.setPen(FG);
                p.drawText(QRectF(qx, op.y + 2, TEXT_W - QIND, QHEAD_H),
                           Qt::AlignLeft | Qt::AlignTop, op.qname);
                const int wn = int(textWidth(fn, op.qname));
                p.setFont(fh);
                p.setPen(FG_FAINT);
                p.drawText(QRectF(qx + wn + 8, op.y + 4, TEXT_W - QIND - wn - 8,
                                  QHEAD_H),
                           Qt::AlignLeft | Qt::AlignTop,
                           QStringLiteral("@") + op.qhandle);
            } else {
                p.setFont(fh);
                p.setPen(FG_DIM);
                p.drawText(QRectF(qx, op.y + 4, TEXT_W - QIND, QHEAD_H),
                           Qt::AlignLeft | Qt::AlignTop,
                           ellipsize(fh, combo, TEXT_W - QIND));
            }
            break;
        }
        case Op::Stats:
            drawStats(p, op, chunk);
            break;
        case Op::FloatCard: {
            // 头部
            drawHead(p, t, px, op.y);
            const int y0 = op.y + HEAD_H + 8;
            // 右上图（占位/已下载）
            if (op.slotIndex >= 0 && op.slotIndex < page.images.size())
                drawPhoto(p, t, page.images.at(op.slotIndex), withPhotos);
            // 图片旁侧窄行（左侧）
            int ly = y0;
            for (const QString &ln : op.floatLines) {
                drawLine(p, ln, px, ly, font(30), FG);
                ly += TEXT_LH;
            }
            // 下方全宽行（超过图片高度后扩展环绕）
            int by = y0 + qMax(op.imgH, int(op.floatLines.size()) * TEXT_LH);
            for (const QString &ln : op.belowLines) {
                drawLine(p, ln, px, by, font(30), FG);
                by += TEXT_LH;
            }
            // "显示全文"内联在最后一个文本行末尾（不另占一行）
            if (!op.btnLabel.isEmpty()) {
                const QFont lf = font(30);
                if (!op.belowLines.isEmpty())
                    drawInlineBtn(p, px + int(textWidth(lf, op.belowLines.last()))
                                          + 10,
                                  by - TEXT_LH, op.btnLabel, TEXT_LH);
                else if (!op.floatLines.isEmpty())
                    drawInlineBtn(p, px + int(textWidth(lf, op.floatLines.last()))
                                          + 10,
                                  ly - TEXT_LH, op.btnLabel, TEXT_LH);
            }
            // 统计行
            if (!op.statsLeft.isEmpty()) {
                const int yy = by + STATS_GAP_TOP;
                const QFont fs = font(22);
                p.setFont(fs);
                p.setPen(FG_FAINT);
                if (!op.statsRight.isEmpty()) {
                    const int rw = int(textWidth(fs, op.statsRight));
                    const int rx = r.x() + r.width() - PAD - rw;
                    const int lw = TEXT_W - rw - 14;
                    QString left = op.statsLeft;
                    if (lw > 0 && textWidth(fs, left) > lw)
                        left = ellipsize(fs, left, lw);
                    p.drawText(QRectF(rx, yy, rw + 4, STATS_H),
                               Qt::AlignLeft | Qt::AlignTop, op.statsRight);
                    if (lw > 0)
                        p.drawText(QRectF(px, yy, lw, STATS_H),
                                   Qt::AlignLeft | Qt::AlignTop, left);
                } else {
                    p.drawText(QRectF(px, yy, TEXT_W, STATS_H),
                               Qt::AlignLeft | Qt::AlignTop, op.statsLeft);
                }
            }
            break;
        }
        case Op::QFloatCard: {
            // 头部
            drawHead(p, t, px, op.y);
            int y = op.y + HEAD_H;
            // 引用者自己的评论（全宽）
            for (const QString &ln : op.commentLines) {
                drawLine(p, ln, px, y, font(30), FG);
                y += TEXT_LH;
            }
            // 引用块整体左竖线（作者行 + 图旁 + 图下连续一条）
            const int qbarY0 = y - 2;
            const int qx = px + QIND;
            {
                const QFont fn = font(24, true), fh = font(22);
                const QString combo = op.qname + QStringLiteral("  @")
                                      + op.qhandle;
                if (textWidth(fn, combo) <= TEXT_W - QIND) {
                    p.setFont(fn);
                    p.setPen(FG);
                    p.drawText(QRectF(qx, y + 2, TEXT_W - QIND, QHEAD_H),
                               Qt::AlignLeft | Qt::AlignTop, op.qname);
                    const int wn = int(textWidth(fn, op.qname));
                    p.setFont(fh);
                    p.setPen(FG_FAINT);
                    p.drawText(QRectF(qx + wn + 8, y + 4,
                                      TEXT_W - QIND - wn - 8, QHEAD_H),
                               Qt::AlignLeft | Qt::AlignTop,
                               QStringLiteral("@") + op.qhandle);
                } else {
                    p.setFont(fh);
                    p.setPen(FG_DIM);
                    p.drawText(QRectF(qx, y + 4, TEXT_W - QIND, QHEAD_H),
                               Qt::AlignLeft | Qt::AlignTop,
                               ellipsize(fh, combo, TEXT_W - QIND));
                }
            }
            y += QHEAD_H + 8;
            // 右上引用图（占位/已下载）
            if (op.slotIndex >= 0 && op.slotIndex < page.images.size())
                drawPhoto(p, t, page.images.at(op.slotIndex), withPhotos);
            const int besideH = int(op.floatLines.size()) * TEXT_LH_Q;
            const int belowH = int(op.belowLines.size()) * TEXT_LH_Q;
            drawQuotedBar(p, r.x() + 10, qbarY0,
                          y + qMax(op.imgH, besideH) + belowH + 2);
            // 图片旁侧窄行（左侧）
            int ly = y;
            for (const QString &ln : op.floatLines) {
                drawLine(p, ln, qx, ly, font(26), FG_DIM);
                ly += TEXT_LH_Q;
            }
            // 图片下方全宽行（超过图片高度后扩展环绕）
            int by = y + qMax(op.imgH, besideH);
            for (const QString &ln : op.belowLines) {
                drawLine(p, ln, qx, by, font(26), FG_DIM);
                by += TEXT_LH_Q;
            }
            // "显示全文"内联在最后一个引用文本行末尾（不另占一行）
            if (!op.btnLabel.isEmpty()) {
                const QFont lf = font(26);
                if (!op.belowLines.isEmpty())
                    drawInlineBtn(p, qx + int(textWidth(lf, op.belowLines.last()))
                                         + 10,
                                  by - TEXT_LH_Q, op.btnLabel, TEXT_LH_Q);
                else if (!op.floatLines.isEmpty())
                    drawInlineBtn(p, qx + int(textWidth(lf, op.floatLines.last()))
                                         + 10,
                                  ly - TEXT_LH_Q, op.btnLabel, TEXT_LH_Q);
            }
            // 统计行
            if (!op.statsLeft.isEmpty()) {
                const int yy = by + STATS_GAP_TOP;
                const QFont fs = font(22);
                p.setFont(fs);
                p.setPen(FG_FAINT);
                if (!op.statsRight.isEmpty()) {
                    const int rw = int(textWidth(fs, op.statsRight));
                    const int rx = r.x() + r.width() - PAD - rw;
                    const int lw = TEXT_W - rw - 14;
                    QString left = op.statsLeft;
                    if (lw > 0 && textWidth(fs, left) > lw)
                        left = ellipsize(fs, left, lw);
                    p.drawText(QRectF(rx, yy, rw + 4, STATS_H),
                               Qt::AlignLeft | Qt::AlignTop, op.statsRight);
                    if (lw > 0)
                        p.drawText(QRectF(px, yy, lw, STATS_H),
                                   Qt::AlignLeft | Qt::AlignTop, left);
                } else {
                    p.drawText(QRectF(px, yy, TEXT_W, STATS_H),
                               Qt::AlignLeft | Qt::AlignTop, op.statsLeft);
                }
            }
            break;
        }
        case Op::FullText: {
            const QFont fb = font(26, true);
            p.setFont(fb);
            p.setPen(QColor("#1a6b9c"));
            p.drawText(QRectF(px, op.y, TEXT_W, TEXT_LH),
                       Qt::AlignLeft | Qt::AlignTop, op.text);
            // 下划线链接样式，提示可点
            const int tw = int(textWidth(fb, op.text));
            p.drawLine(px, op.y + TEXT_LH - 8, px + tw, op.y + TEXT_LH - 8);
            break;
        }
        }
    }
}

// ---- 全文全屏阅读（单栏，跨页排版） ----

static bool tweetHasQuoted(const XTweet &t)
{
    return !t.quoted.authorName.isEmpty()
           || !t.quoted.text.trimmed().isEmpty()
           || !t.quoted.media.isEmpty();
}

QImage Renderer::renderTextPage(const XTweet &t, int pageIndex, int *totalPages)
{
    remarkxSetCtx("render:renderTextPage");
    struct Ln {
        int page = 0;
        int y = 0;
        QString text;
        int pixel = 30;
        bool bold = false;
        QColor color;
        bool divider = false;   // 画分隔横线
        bool bar = false;       // 引用块左竖线
    };
    QVector<Ln> lns;
    const int x = MARGIN;
    const int w = W - 2 * MARGIN;
    int page = 0, y = TOP_Y;
    auto take = [&](int lh) {
        if (y + lh > BOTTOM_Y) {
            ++page;
            y = TOP_Y;
        }
        const int r = y;
        y += lh;
        return r;
    };
    auto pushWrapped = [&](const QString &s, int pixel, bool bold,
                           const QColor &c, int lh, bool bar = false) {
        const QStringList ls = wrapText(font(pixel, bold), s, w, 100000);
        for (const QString &ln : ls)
            lns.append({page, take(lh), ln, pixel, bold, c, false, bar});
    };

    const bool translated = t.translated && !t.sourceLang.isEmpty();

    // 头部
    {
        const QString name = t.authorName.isEmpty() ? QStringLiteral("?")
                                                    : t.authorName;
        lns.append({page, take(TEXT_LH), name + QStringLiteral("  @")
                                           + t.authorHandle, 30, true, FG,
                    false, false});
        if (t.isRetweet) {
            lns.append({page, take(30), QStringLiteral("转发了"), 22, false,
                        FG_DIM, false, false});
        }
        QStringList ms;
        const QString at = absTime(t.createdAt);
        if (!at.isEmpty())
            ms << at;
        if (translated) {
            const QString zh = langName(t.sourceLang);
            if (!zh.isEmpty())
                ms << QStringLiteral("译自 %1").arg(zh);
        }
        if (!ms.isEmpty())
            lns.append({page, take(30), ms.join(QStringLiteral(" · ")), 22,
                        false, FG_FAINT, false, false});
        lns.append({page, take(24), QString(), 22, false, CARD_BORDER, true,
                    false});
    }

    if (!t.isRetweet) {
        // 完整文本：译文已覆盖全文则显示译文，否则显示原文全文
        // （长推文的 note_tweet.text / 普通推文的 full_text）
        const QString full = t.translated ? cleanText(t.text)
                                          : cleanText(t.originalText);
        if (!full.isEmpty())
            pushWrapped(full, 30, false, FG, TEXT_LH);
    }

    // 引用块
    if (tweetHasQuoted(t)) {
        lns.append({page, take(24), QString(), 22, false, CARD_BORDER, true,
                    false});
        const QString qn = t.quoted.authorName.isEmpty()
                               ? QStringLiteral("?") : t.quoted.authorName;
        lns.append({page, take(30), qn + QStringLiteral("  @")
                                       + t.quoted.authorHandle, 26, true, FG,
                    false, true});
        if (t.quoted.translated && !t.quoted.sourceLang.isEmpty()) {
            const QString zh = langName(t.quoted.sourceLang);
            if (!zh.isEmpty())
                lns.append({page, take(26),
                            QStringLiteral("译自 %1").arg(zh), 22, false,
                            FG_FAINT, false, true});
        }
        const QString qfull = t.quoted.translated
                                  ? cleanText(t.quoted.text)
                                  : cleanText(t.quoted.originalText);
        pushWrapped(qfull, 26, false, FG_DIM, TEXT_LH_Q, true);
    }

    const int total = page + 1;
    if (totalPages)
        *totalPages = total;
    if (pageIndex < 0 || pageIndex >= total)
        return QImage();

    QImage img(W, H, QImage::Format_RGB32);
    img.fill(BG);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    for (const Ln &l : lns) {
        if (l.page != pageIndex)
            continue;
        if (l.divider) {
            p.setPen(QPen(CARD_BORDER, 2));
            p.drawLine(x, l.y, x + w, l.y);
            continue;
        }
        int tx = x;
        if (l.bar) {
            p.setPen(QPen(FG_FAINT, 2));
            p.drawLine(x, l.y - 2, x, l.y + TEXT_LH_Q + 2);
            tx = x + 20;
        }
        p.setFont(font(l.pixel, l.bold));
        p.setPen(l.color);
        p.drawText(QRectF(tx, l.y, w - (tx - x), 4000),
                   Qt::AlignLeft | Qt::AlignTop, l.text);
    }
    if (total > 1) {
        p.setFont(font(22));
        p.setPen(FG_FAINT);
        p.drawText(QRectF(x, BOTTOM_Y - 8, w, 30),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("第 %1/%2 页").arg(pageIndex + 1).arg(total));
    }
    p.end();
    return img;
}

int Renderer::textPageCount(const XTweet &t)
{
    int total = 0;
    renderTextPage(t, 0, &total);
    return total;
}
