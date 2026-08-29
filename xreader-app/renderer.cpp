#include "renderer.h"

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
                               qreal maxWidth, int maxLines) const
{
    QStringList lines;
    static const QRegularExpression unitRe(QStringLiteral(
        "[A-Za-z0-9]+(?:['\\x{2019}\\-][A-Za-z0-9]+)*|\\s+|\\S"));
    static const QString closing(
        QStringLiteral("，。、；：！？）》〉」』】〕”’…％℃"));
    static const QRegularExpression trailingWs(QStringLiteral("\\s+$"));

    auto rstrip = [&](QString s) {
        s.replace(trailingWs, QString());
        return s;
    };
    auto emitLine = [&](QString line) {
        line = rstrip(line);
        if (!line.isEmpty())
            lines.append(line);
        else if (lines.isEmpty() || !lines.last().isEmpty())
            lines.append(QString());
    };

    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            emitLine(QString());
            continue;
        }
        QString line;
        auto it = unitRe.globalMatch(raw);
        while (it.hasNext()) {
            const QString u = it.next().captured(0);
            if (textWidth(f, line + u) <= maxWidth) {
                line += u;
                continue;
            }
            if (u.at(0).isSpace())
                continue;  // 行尾空白悬挂丢弃
            if (closing.contains(u) && !line.isEmpty()) {
                line += u;  // 闭合标点挂到行尾
                continue;
            }
            if (!line.trimmed().isEmpty()) {
                emitLine(line);
                line.clear();
                if (textWidth(f, u) <= maxWidth) {
                    line = u;
                    continue;
                }
            }
            // 超长原子（长英文单词）逐字符硬拆
            for (const QChar &ch : u) {
                if (!line.isEmpty() && textWidth(f, line + ch) > maxWidth) {
                    emitLine(line);
                    line.clear();
                }
                line += ch;
            }
        }
        emitLine(line);
        if (lines.size() >= maxLines + 1)
            break;
    }
    if (lines.size() > maxLines) {
        lines = lines.mid(0, maxLines);
        lines[maxLines - 1] = rstrip(lines[maxLines - 1]) + QStringLiteral(" …");
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

QVector<Renderer::Atom> Renderer::buildAtoms(const QVector<XTweet> &feed,
                                             int ti)
{
    const XTweet &t = feed[ti];
    QVector<Atom> atoms;

    Atom head;
    head.kind = Op::Head;
    head.h = HEAD_H;
    head.tweetIndex = ti;
    atoms.append(head);

    const bool hasQuoted = !t.quoted.authorName.isEmpty()
                           || !t.quoted.text.trimmed().isEmpty()
                           || !t.quoted.media.isEmpty();
    if (hasQuoted) {
        if (!t.isRetweet) {
            const QString text = cleanText(t.text);
            if (!text.isEmpty()) {
                const QStringList lns = wrapText(font(30), text, TEXT_W, 100000);
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
            const QStringList lns =
                wrapText(font(26), qtext, TEXT_W - QIND, 100000);
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
            const QStringList lns = wrapText(font(30), text, TEXT_W, 100000);
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

    for (int ti = 0; ti < feed.size(); ++ti) {
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

        auto placeAtom = [&]() -> bool {
            Atom &a = atoms[ai];
            const int extra = (chunkIdx >= 0) ? 0 : openContNeeded();
            if (freeSpace() < a.h + extra)
                return false;
            if (chunkIdx < 0) {
                const bool cont = chunkWasSplit
                                  && (a.kind == Op::Line || a.kind == Op::QLine);
                openChunk(cont, ti);
            }
            RenderChunk &ch = pages[p].chunks[chunkIdx];
            Op op;
            op.kind = a.kind;
            switch (a.kind) {
            case Op::Head:
                op.y = grow(HEAD_H);
                break;
            case Op::Line:
            case Op::QLine:
                op.y = grow(a.h);
                op.text = a.text;
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
            lastKind = a.kind;
            ++ai;
            return true;
        };

        while (ai < atoms.size()) {
            if (!placeAtom()) {
                close();
                jump();
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
    if (top) {
        p.drawLine(l, y0, r, y0);
        p.drawArc(QRectF(l, y0, 2 * rad, 2 * rad), 180 * 16, 90 * 16);
        p.drawArc(QRectF(r - 2 * rad, y0, 2 * rad, 2 * rad), 270 * 16, 90 * 16);
    }
    if (bottom) {
        p.drawLine(l, y1, r, y1);
        p.drawArc(QRectF(l, y1 - 2 * rad, 2 * rad, 2 * rad), 90 * 16, 90 * 16);
        p.drawArc(QRectF(r - 2 * rad, y1 - 2 * rad, 2 * rad, 2 * rad), 0 * 16,
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
    if (it != m_avatarCache.constEnd())
        return it.value();
    QImage src(full);
    if (src.isNull())
        return {};
    QImage img = src.convertToFormat(QImage::Format_RGB32)
                     .scaled(AVATAR_D, AVATAR_D, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
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
    m_avatarCache.insert(full, out);
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
    QImage photo = m_photoCache.value(key);
    if (photo.isNull()) {
        QImage src(full);
        if (src.isNull()) {
            drawPlaceholder(p, ix, py, dw, dh);
            return;
        }
        photo = src.convertToFormat(QImage::Format_RGB32)
                    .scaled(dw, dh, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
        m_photoCache.insert(key, photo);
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
        case Op::Line:
            drawLine(p, op.text, px, op.y, font(30), FG);
            break;
        case Op::QLine:
            drawQuotedBar(p, r.x() + 10, op.y - 2, op.y + TEXT_LH_Q + 2);
            drawLine(p, op.text, px + QIND, op.y, font(26), FG_DIM);
            break;
        case Op::Img:
            drawPhoto(p, t, page.images.at(op.slotIndex), withPhotos);
            break;
        case Op::QImg: {
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
        }
    }
}
