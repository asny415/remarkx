"""页面渲染器：把推文流排版成 1404x1872 整页 PNG（电子墨水阅读页）。

双栏卡片流式排版：
- 正文按行原子化，可跨栏/跨页接续；头部与图片块不可拆分。
- 放置阶段产出绝对坐标绘制指令（ops），绘制阶段只执行指令，
  保证测量与渲染一致、永不越界。
- 单页尽量填满：底部仅留 MARGIN 空白（设备端无控制条）。

链接从正文隐藏；emoji 剥离；超宽行/名字/元信息均以省略号截断。
"""

import logging
import os
import re
from datetime import datetime

from PIL import Image, ImageDraw, ImageFont

log = logging.getLogger("remarkx.render")

W, H = 1404, 1872
MARGIN = 48
COL_GUTTER = 28
CARD_GAP = 24
PAD = 22
CONTENT_W = W - 2 * MARGIN
COL_W = (CONTENT_W - COL_GUTTER) // 2
TEXT_W = COL_W - 2 * PAD
TOP_Y = MARGIN
BOTTOM_Y = H - MARGIN

NAME_H = 36
META_H = 26
TEXT_LH = 44
IMG_MAX_H = 400
IMG_GAP = 12
PAD_TOP = 18
PAD_BOTTOM = 18
HEAD_H = NAME_H + 6 + META_H + 8        # 头部块内容高（不含顶部留白）
MIN_CHUNK_H = 24
QIND = 30                               # 引用块缩进
QHEAD_H = 36                            # 引用块作者行高
TEXT_LH_Q = 38                          # 引用块文本行高（26 号字）
STATS_GAP_TOP = 28                      # 统计行与正文的距离
STATS_H = 30                            # 统计行文字行高

BG = "#ffffff"
FG = "#1a1a1a"
FG_DIM = "#5a5a5a"
FG_FAINT = "#8a8a8a"
CARD_BORDER = "#d9d9d9"

_EMOJI_RE = re.compile(
    "["
    "\U0001F000-\U0001FAFF"
    "\u2600-\u27BF"
    "\u2B00-\u2BFF"
    "\uFE0F"
    "\u200D"
    "]"
)
_URL_RE = re.compile(r"https?://\S+")
# 行首禁则：闭合标点不出现在行首 → 悬挂到上一行行尾（允许轻微超宽）
_CLOSING_PUNCT = "，。、；：！？）》〉」』】〕”’…％℃"
_CJK_CHAR_RE = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")

_FONT_CANDIDATES = [
    # 设备同款字体（方正书宋，GBK 全覆盖），与 reMarkable 阅读体验一致；
    # 放 relay/fonts/（不入库，见 .gitignore），由 device 上复制而来
    "fonts/fangzhengshusong.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/System/Library/Fonts/PingFang.ttc",
    "C:/Windows/Fonts/msyh.ttc",
]


def find_cjk_font() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    for p in _FONT_CANDIDATES:
        if os.path.exists(p) or os.path.exists(os.path.join(here, p)):
            return p if os.path.isabs(p) else os.path.join(here, p)
    return ""


def clean_text(text: str) -> str:
    """隐藏链接、剥离 emoji，规整空白。"""
    text = _URL_RE.sub("", text or "")
    text = _EMOJI_RE.sub("", text)
    text = re.sub(r"[ \t]+", " ", text)
    return text.strip()


def _cjk_dominant(text: str) -> bool:
    """中文字符占比是否足以采用两端对齐排版。"""
    letters = [c for c in text if c.isalpha()]
    if len(letters) < 8:
        return False
    cjk = sum(1 for c in letters if _CJK_CHAR_RE.match(c))
    return cjk / max(len(letters), 1) >= 0.3


class Renderer:
    def __init__(self, media_dir: str, title: str = "X · Following",
                 font_path: str = "", prefer_zh: bool = False):
        self.media_dir = media_dir
        self.title = title
        self.prefer_zh = prefer_zh      # 有译文时显示中文
        self.font_path = font_path or find_cjk_font()
        if not self.font_path:
            log.warning("未找到中文字体，中文将显示为方块。"
                        "Ubuntu: sudo apt install fonts-noto-cjk")
        self._fonts = {}
        self._measure_img = Image.new("RGB", (2, 2))
        self._measure_draw = ImageDraw.Draw(self._measure_img)
        self._avatar_cache = {}          # path -> (circle_img, size)
        self._resize_cache = {}          # (path,dw,dh) -> resized photo
        self._atom_cache = {}            # tweet id -> (lines, img_payload, img_h)
        self.avatar_d = 56               # 头像直径
        self.avatar_gap = 14             # 头像与文字间距
        self._coverage = self._load_coverage(self.font_path)

    def font(self, size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
        key = (size, bold)
        if key not in self._fonts:
            path = self.font_path
            if not path:
                return ImageFont.load_default()
            if bold:
                b = path.replace("Regular", "Bold")
                if os.path.exists(b):
                    path = b
            self._fonts[key] = ImageFont.truetype(path, size)
        return self._fonts[key]

    # ------------------------------------------------------------------ #
    # 文本工具
    # ------------------------------------------------------------------ #

    @staticmethod
    def _load_coverage(font_path: str):
        """用 fontTools 读取字体 cmap，返回支持的码点集合；失败返回 None（不过滤）。"""
        try:
            from fontTools.ttLib import TTFont
            f = TTFont(font_path, lazy=True, fontNumber=0)
            cov = set()
            for table in f["cmap"].tables:
                if table.isUnicode():
                    cov |= set(table.cmap.keys())
            f.close()
            log.info("字体覆盖集: %s (%d 码点)", font_path, len(cov))
            return cov
        except Exception as e:
            log.warning("字体覆盖集加载失败，跳过缺字过滤: %s", e)
            return None

    def _filter_glyphs(self, text: str) -> str:
        """剥离当前字体不支持的字形，杜绝方块。"""
        if self._coverage is None or not text:
            return text
        cov = self._coverage
        return "".join(
            ch for ch in text
            if ch in "\n\t " or ord(ch) in cov)

    @staticmethod
    def _text_width(draw, text, font) -> float:
        try:
            return draw.textlength(text, font=font)
        except AttributeError:
            return draw.textsize(text, font=font)[0]

    def _ellipsize(self, draw, text: str, font, max_width: float) -> str:
        """超宽文本截断并追加省略号，保证不越出容器。"""
        if self._text_width(draw, text, font) <= max_width:
            return text
        while text and self._text_width(draw, text + "…", font) > max_width:
            text = text[:-1]
        return text + "…"

    @staticmethod
    def _wrap_units(text: str) -> list:
        """把文本拆成断行原子：
        - 连续英文字母/数字（含 don't、e-mail 这类内联符号）为一个原子，
          保证英文单词永不截断；
        - CJK 汉字及标点逐字成原子（中文允许任意断行，填满每一行）；
        - 空白串为一个原子（行尾悬挂丢弃，不留到下一行行首）。
        """
        units = []
        for m in re.finditer(r"[A-Za-z0-9]+(?:['’\-][A-Za-z0-9]+)*|\s+|\S",
                             text):
            units.append(m.group(0))
        return units

    def wrap_text(self, draw, text: str, font, max_width: float,
                  max_lines: int) -> list:
        """按像素宽度贪心折行。

        中英混排策略：英文单词作为整体不可拆；汉字逐字断行，
        因此行尾遇到放不下的英文单词时，前面的中文仍会继续
        填充当前行，不留大片空白。
        """
        lines = []

        def emit(line):
            line = line.rstrip()
            if line:
                lines.append(line)
            elif not lines or lines[-1] != "":
                # 保留有意的空行（段落间隔），但不叠加
                lines.append("")

        for raw in text.splitlines():
            if not raw.strip():
                emit("")
                continue
            line = ""
            for u in self._wrap_units(raw):
                if self._text_width(draw, line + u, font) <= max_width:
                    line += u
                    continue
                # 放不下该原子
                if u.isspace():
                    continue                       # 行尾空格直接悬挂丢弃
                # 行首禁则：闭合标点悬挂到本行行尾（略超宽也保留）
                if u in _CLOSING_PUNCT and line:
                    line += u
                    continue
                if line.rstrip():
                    emit(line)
                    line = ""
                    # 新行放得下则直接放（整词/CJK 单字都适用）
                    if self._text_width(draw, u, font) <= max_width:
                        line = u
                        continue
                # 空行仍放不下（超长英文单词）→ 逐字符硬拆
                for ch in u:
                    if line and self._text_width(draw, line + ch,
                                                 font) > max_width:
                        emit(line)
                        line = ""
                    line += ch
            emit(line)
            if len(lines) >= max_lines + 1:
                break
        if len(lines) > max_lines:
            lines = lines[:max_lines]
            lines[-1] = lines[-1].rstrip() + " …"
        return lines

    @staticmethod
    def parse_created_at(s: str):
        try:
            dt = datetime.strptime(s, "%a %b %d %H:%M:%S %z %Y")
            return dt.astimezone()
        except (ValueError, TypeError):
            return None

    @staticmethod
    def abs_time(s: str) -> str:
        """绝对本地时间；同年省略年份。"""
        dt = Renderer.parse_created_at(s)
        if dt is None:
            return ""
        if dt.year == datetime.now().year:
            return dt.strftime("%m-%d %H:%M")
        return dt.strftime("%Y-%m-%d %H:%M")

    # ------------------------------------------------------------------ #
    # 图片工具
    # ------------------------------------------------------------------ #

    def _load_photo(self, m: dict):
        path = os.path.join(self.media_dir, m["path"])
        try:
            img = Image.open(path)
            img.load()
            if img.mode != "RGB":
                img = img.convert("RGB")
            return img
        except Exception as e:
            log.warning("加载图片失败 %s: %s", path, e)
            return None

    @staticmethod
    def _fit(img, box_w: int, box_h: int):
        w, h = img.size
        scale = min(box_w / w, box_h / h, 1.0)
        dw = max(1, int(w * scale))
        dh = max(1, int(h * scale))
        return img.resize((dw, dh), Image.LANCZOS), dw, dh

    def _card_text(self, t: dict) -> str:
        # 配置开启翻译且该推有中文译文 → 显示译文；否则原文
        if self.prefer_zh and t.get("translated"):
            return clean_text(self._filter_glyphs(t["translated"]))
        return clean_text(self._filter_glyphs(t.get("text", "")))

    def _card_media(self, t: dict) -> list:
        return [m for m in t.get("media", []) if m.get("path")]

    # ------------------------------------------------------------------ #
    # 分页（原子化流式布局）
    # ------------------------------------------------------------------ #
    #
    # 卡片被拆成三类原子：
    #   head  —— 作者行+元信息行（不可拆）
    #   img   —— 封面图（不可拆）
    #   txt   —— 若干文本行（逐行可拆）
    # 放置顺序严格线性：page0.col0 → page0.col1 → page1.col0 → …
    # 每个“块(chunk)”记录自己在某个槽内消耗的区域与绘制指令；
    # 跨槽续排的块打上 is_cont 标记（渲染时加“┆续”提示）。
    #
    # chunk 结构（内部使用）：
    #   {"t":tweet, "x":..., "w":COL_W, "col":..., "y":起始, "h":高度,
    #    "is_cont":bool, "ops":[(kind, payload)...]}
    # op kind: "head" | "img"(payload=dict) | "line"(str)
    #          | "qhead"((name,handle)) | "qline"(str) | "qimg"(payload)
    #          | "stats"(str)
    # ops 按序存放，渲染时从 y0 开始顺序消耗竖直空间。

    def _img_atom(self, media: list, ind: int = 0):
        """媒体列表 -> 单个图片原子 (kind, payload, h)；无可用图返回 None。

        排版阶段零 IO：用元数据宽高推算显示尺寸，真正的图片加载/缩放
        推迟到绘制阶段（带缓存）。ind>0 表示引用块内的缩进图。
        """
        media = [m for m in media if m.get("path")]
        if not media:
            return None
        m0 = media[0]
        tw = TEXT_W - ind
        w0, h0 = int(m0.get("w") or 0), int(m0.get("h") or 0)
        if w0 > 0 and h0 > 0:
            scale = min(tw / w0, IMG_MAX_H / h0, 1.0)
            dwp = max(1, int(w0 * scale))
            dhp = max(1, int(h0 * scale))
        else:
            img = self._load_photo(m0)
            if img:
                _, dwp, dhp = self._fit(img, tw, IMG_MAX_H)
            else:
                dwp = dhp = 0
        if not dhp:
            return None
        payload = {
            "path": m0.get("path", ""), "dw": dwp, "dh": dhp,
            "is_video": m0.get("type") == "video",
            "n_media": len(media), "tw": tw, "ind": ind, "y": 0,
        }
        return ("qimg" if ind else "img", payload, dhp + IMG_GAP)

    @staticmethod
    def _fmt_count(n) -> str:
        n = int(n or 0)
        if n >= 10000:
            return f"{n / 10000:.1f}".rstrip("0").rstrip(".") + "万"
        return str(n)

    def _stats_line(self, t: dict):
        """统计行 -> (左侧文本, 右侧文本)：转/赞/评靠左，阅读数靠右。"""
        s = t.get("stats") or {}
        if not s:
            return ""
        left = self._filter_glyphs(
            f"转 {self._fmt_count(s.get('reposts'))}"
            f" · 赞 {self._fmt_count(s.get('likes'))}"
            f" · 评 {self._fmt_count(s.get('replies'))}")
        right = self._filter_glyphs(f"阅 {self._fmt_count(s.get('views'))}")
        return left, right

    def _build_atoms(self, t: dict, draw) -> list:
        """tweet -> 排版原子列表 [(kind, payload, h)]。

        - 普通推文：head → 主图 → 文本行 → stats
        - 纯转推：head（转发者，meta 行标「转发了」）→ 引用块（原帖
          作者行/文本/图）→ 原帖 stats
        - 引用推文：head → 评论行 → 自己的图 → 引用块 → 自己 stats
        """
        atoms = [("head", None, HEAD_H)]
        q = t.get("quoted")
        if q:
            if not t.get("is_retweet"):
                # 引用推文：转发者的评论 + 自己的媒体
                text = self._card_text(t)
                if text:
                    for ln in self.wrap_text(draw, text, self.font(30),
                                             TEXT_W, 100000):
                        atoms.append(("line", ln, TEXT_LH))
                a = self._img_atom(t.get("media") or [])
                if a:
                    atoms.append(a)
            # 引用块（原帖内容）
            atoms.append(("qhead", (q.get("author_name") or "?",
                                    q.get("author_handle") or ""), QHEAD_H))
            qtext = clean_text(self._filter_glyphs(q.get("text") or ""))
            if qtext:
                for ln in self.wrap_text(draw, qtext, self.font(26),
                                         TEXT_W - QIND, 100000):
                    atoms.append(("qline", ln, TEXT_LH_Q))
            a = self._img_atom(q.get("media") or [], ind=QIND)
            if a:
                atoms.append(a)
        else:
            text = self._card_text(t)
            if text:
                for ln in self.wrap_text(draw, text, self.font(30),
                                         TEXT_W, 100000):
                    atoms.append(("line", ln, TEXT_LH))
            a = self._img_atom(t.get("media") or [])
            if a:
                atoms.append(a)
        st = self._stats_line(t)
        if st:
            atoms.append(("stats", st, STATS_GAP_TOP + STATS_H))
        return atoms

    def paginate(self, tweets: list) -> list:
        pages = [{"cards": []}]

        class Cur:
            __slots__ = ("p", "col", "y")

            def __init__(self):
                self.p = 0
                self.col = 0
                self.y = TOP_Y

        cur = Cur()
        draw = self._measure_draw

        def col_x(col):
            return MARGIN + col * (COL_W + COL_GUTTER)

        def ensure_page():
            while len(pages) <= cur.p:
                pages.append({"cards": []})

        def open_chunk(is_cont, t):
            ensure_page()
            chunk = {"t": t, "x": col_x(cur.col), "w": COL_W,
                     "col": cur.col, "y": cur.y, "h": 0,
                     "is_cont": is_cont, "has_cont": False, "ops": []}
            # 顶部留白（续排块同样只留标准内边距，无标记字符）
            chunk["h"] = PAD_TOP
            cur.y = chunk["y"] + chunk["h"]
            pages[cur.p]["cards"].append(chunk)
            return chunk

        def grow(chunk, delta):
            y0 = cur.y
            cur.y += delta
            chunk["h"] = cur.y - chunk["y"]
            return y0

        def jump():
            """当前槽结束：换列或换页。"""
            cur.y = TOP_Y
            if cur.col == 0:
                cur.col = 1
            else:
                cur.col = 0
                cur.p += 1
            ensure_page()

        def free():
            return BOTTOM_Y - cur.y

        for t in tweets:
            if not (t.get("text") or t.get("comment")
                    or self._card_media(t) or t.get("quoted")):
                continue

            # ---- 原子准备（带 per-tweet 缓存：跨版本只测新推） ----
            key = str(t.get("id") or id(t))
            atoms = self._atom_cache.get(key)
            if atoms is None:
                atoms = self._build_atoms(t, draw)
                self._atom_cache[key] = atoms

            # ---- 放置：逐原子顺序放置；不满足则关闭当前块跳槽重试 ----
            # 需求必须包含“若需新开块”的顶部留白（续排块含┆续标记行高）
            chunk = None
            chunk_was_split = [False]
            last_kind = [""]
            ai = 0

            def close():
                nonlocal chunk
                if chunk is not None:
                    chunk["has_cont"] = True     # 后面还有续排段：无下边框
                    chunk_was_split[0] = True
                chunk = None

            def open_cont_needed():
                return PAD_TOP + (24 if chunk_was_split[0] else 0)

            def place_atom():
                """返回 True 表示成功放置；False 表示需先跳槽重试。"""
                nonlocal chunk, ai
                kind, payload, h = atoms[ai]
                extra = 0 if chunk is not None else open_cont_needed()
                need = h + extra
                if free() < need:
                    return False
                if chunk is None:
                    # 文本类原子可续排；其他原子跳槽后重开完整块
                    chunk = open_chunk(
                        chunk_was_split[0] and kind in ("line", "qline"), t)
                if kind == "head":
                    chunk["ops"].append(("head", grow(chunk, HEAD_H)))
                elif kind in ("img", "qimg"):
                    payload["y"] = grow(chunk, h)
                    chunk["ops"].append((kind, payload))
                elif kind == "stats":
                    chunk["ops"].append(("stats",
                                         (grow(chunk, h), payload)))
                else:  # line / qline
                    yy = grow(chunk, h)
                    chunk["ops"].append((kind, (yy, payload)))
                last_kind[0] = kind
                ai += 1
                return True

            while ai < len(atoms):
                if not place_atom():
                    close()
                    jump()

            # 4) 底部内边距 + 卡间距（计入最后一个块的边界框）
            # 统计行本身贴近底部：其后只留卡间距，不再叠加底部内边距
            pad_needed = (CARD_GAP if last_kind[0] == "stats"
                          else PAD_BOTTOM + CARD_GAP)
            if chunk is not None and cur.y + pad_needed <= BOTTOM_Y:
                grow(chunk, pad_needed)

        return [pg for pg in pages if pg["cards"]]

    # ------------------------------------------------------------------ #
    # 绘制单个 chunk
    # ------------------------------------------------------------------ #

    def _card_border(self, draw, x, y0, y1, w, top: bool, bottom: bool):
        """卡片边框：续接段按需省略上/下边框（左右边线始终保留）。"""
        l, r = x + 1, x + w - 1
        rad = 10
        draw.line([l, y0, l, y1], fill=CARD_BORDER, width=2)
        draw.line([r, y0, r, y1], fill=CARD_BORDER, width=2)
        if top:
            draw.line([l, y0, r, y0], fill=CARD_BORDER, width=2)
            draw.arc([l, y0, l + 2 * rad, y0 + 2 * rad], 180, 270,
                     fill=CARD_BORDER, width=2)
            draw.arc([r - 2 * rad, y0, r, y0 + 2 * rad], 270, 360,
                     fill=CARD_BORDER, width=2)
        if bottom:
            draw.line([l, y1, r, y1], fill=CARD_BORDER, width=2)
            draw.arc([l, y1 - 2 * rad, l + 2 * rad, y1], 90, 180,
                     fill=CARD_BORDER, width=2)
            draw.arc([r - 2 * rad, y1 - 2 * rad, r, y1], 0, 90,
                     fill=CARD_BORDER, width=2)

    def draw_card(self, page: Image.Image, draw, card: dict):
        t = card["t"]
        x, y, w, h = card["x"], card["y"], card["w"], card["h"]
        has_top = not card.get("is_cont")     # 续前段 → 无上边框
        has_bot = not card.get("has_cont")    # 有后续段 → 无下边框
        if y1 := y + max(h, 4):
            self._card_border(draw, x, y, y1, w, has_top, has_bot)

        px = x + PAD

        for kind, payload in card["ops"]:
            if kind == "head":
                self._draw_head(page, draw, t, px, payload)
            elif kind == "img":
                self._draw_img(page, draw, payload, px)
            elif kind == "qimg":
                rx = x + 10
                draw.line([rx, payload["y"] - 2, rx,
                           payload["y"] + payload["dh"] + IMG_GAP + 2],
                          fill=FG_FAINT, width=2)
                self._draw_img(page, draw, payload, px)
            elif kind == "qhead":
                yy, (qname, qhandle) = payload
                rx = x + 10
                draw.line([rx, yy - 2, rx, yy + QHEAD_H + 2],
                          fill=FG_FAINT, width=2)
                qx = px + QIND
                f_n = self.font(24, bold=True)
                f_h = self.font(22)
                combo = qname + "  @" + qhandle
                if self._text_width(draw, combo, f_n) <= TEXT_W - QIND:
                    draw.text((qx, yy + 2), qname, font=f_n, fill=FG)
                    wn = int(self._text_width(draw, qname, f_n))
                    draw.text((qx + wn + 8, yy + 4), "@" + qhandle,
                              font=f_h, fill=FG_FAINT)
                else:
                    combo = self._ellipsize(draw, combo, f_h, TEXT_W - QIND)
                    draw.text((qx, yy + 4), combo, font=f_h, fill=FG_DIM)
            elif kind == "qline":
                yy, ln = payload
                if ln:
                    rx = x + 10
                    draw.line([rx, yy - 2, rx, yy + TEXT_LH_Q + 2],
                              fill=FG_FAINT, width=2)
                    draw.text((px + QIND, yy), ln,
                              font=self.font(26), fill=FG_DIM)
            elif kind == "stats":
                yy, (sleft, sright) = payload
                f_s = self.font(22)
                yy += STATS_GAP_TOP
                if sright:
                    rw = int(self._text_width(draw, sright, f_s))
                    rx = x + w - PAD - rw
                    lw = TEXT_W - rw - 14
                    if lw > 0 and self._text_width(draw, sleft, f_s) > lw:
                        sleft = self._ellipsize(draw, sleft, f_s, lw)
                    if sright:
                        draw.text((rx, yy), sright, font=f_s, fill=FG_FAINT)
                if sleft:
                    draw.text((px, yy), sleft, font=f_s, fill=FG_FAINT)
            elif kind == "line":
                yy, ln = payload[:2]
                if ln:
                    # 行文本由 wrap_text 保证宽度（悬挂标点除外，属有意超宽）
                    draw.text((px, yy), ln, font=self.font(30), fill=FG)

    def _avatar(self, t: dict):
        """加载作者头像并做圆形裁剪；无图返回 None。"""
        rel = t.get("avatar") or ""
        if not rel:
            return None
        if rel in self._avatar_cache:
            return self._avatar_cache[rel]
        path = os.path.join(self.media_dir, rel)
        try:
            img = Image.open(path).convert("RGB")
            d = self.avatar_d
            img = img.resize((d, d), Image.LANCZOS)
            mask = Image.new("L", (d, d), 0)
            ImageDraw.Draw(mask).ellipse([0, 0, d - 1, d - 1], fill=255)
            out = Image.new("RGB", (d, d), "#ffffff")
            out.paste(img, (0, 0), mask)
            self._avatar_cache[rel] = out
            return out
        except Exception as e:
            log.warning("头像加载失败 %s: %s", rel, e)
            return None

    def _draw_head(self, page, draw, t, px, py):
        f_name = self.font(26, bold=True)
        f_dim = self.font(24)
        name = self._filter_glyphs(t.get("author_name") or "?") or "?"
        handle = "@" + self._filter_glyphs(t.get("author_handle") or "")

        # 头像（有图用图，无图画名字首字占位），文字整体右移
        av = self._avatar(t)
        tx = px
        if av:
            page.paste(av, (px, py + 2))
            tx = px + self.avatar_d + self.avatar_gap
        else:
            d = self.avatar_d
            cy = py + 2 + d // 2
            draw.ellipse([px, py + 2, px + d, py + 2 + d],
                         fill="#e8e8e8")
            ch = (name or "?")[0]
            f_av = self.font(30, bold=True)
            w = self._text_width(draw, ch, f_av)
            draw.text((px + (d - w) / 2, cy - 18), ch,
                      font=f_av, fill="#666666")
            tx = px + d + self.avatar_gap

        # 名字行从头像右侧开始；单行可用宽度相应收缩
        name_w = TEXT_W - self.avatar_d - self.avatar_gap
        combo_plain = (name + "  " + handle)
        if self._text_width(draw, combo_plain, f_name) <= name_w:
            wx = tx
            draw.text((wx, py + 2), name, font=f_name, fill=FG)
            wn = int(self._text_width(draw, name, f_name))
            draw.text((wx + wn + 10, py + 7), handle,
                      font=f_dim, fill=FG_DIM)
        else:
            combo = self._ellipsize(draw, combo_plain, f_dim, name_w)
            draw.text((tx, py + 2), combo, font=f_dim, fill=FG_DIM)
        py += NAME_H + 6

        f_meta = self.font(22)
        media = self._card_media(t)
        videos = [m for m in media if m.get("type") == "video"]
        metas = []
        if t.get("is_retweet"):
            metas.append("转发了")
        metas.append(self.abs_time(t.get("created_at", "")))
        if videos:
            metas.append("▶ 视频")
        n_imgs = len(media) - len(videos)
        if n_imgs:
            metas.append(f"{n_imgs} 图")
        meta_line = self._filter_glyphs(" · ".join(x for x in metas if x))
        draw.text((tx, py),
                  self._ellipsize(draw, meta_line, f_meta, name_w),
                  font=f_meta, fill=FG_FAINT)

    def _draw_img(self, page, draw, info, px):
        dw_, dh = info["dw"], info["dh"]
        py = info["y"]
        tw = info.get("tw") or TEXT_W
        ind = info.get("ind") or 0
        key = (info["path"], dw_, dh)
        photo = self._resize_cache.get(key)
        if photo is None:
            img = self._load_photo({"path": info["path"]})
            if img is None:
                return
            rz, dw_, dh = self._fit(img, tw, IMG_MAX_H)
            photo = rz
            self._resize_cache[key] = photo
        ix = px + ind + (tw - dw_) // 2
        draw.rectangle([ix - 3, py - 3, ix + dw_ + 3, py + dh + 3],
                       outline=CARD_BORDER, width=2)
        page.paste(photo, (ix, py))
        if info["is_video"]:
            cx, cy = ix + dw_ // 2, py + dh // 2
            rr = 40
            draw.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill="#000000")
            draw.polygon([(cx - 13, cy - 22), (cx - 13, cy + 22),
                          (cx + 26, cy)], fill="#ffffff")
        if info["n_media"] > 1:
            tag = f"共 {info['n_media']} 图"
            tf = self.font(20, bold=True)
            tgw = int(self._text_width(draw, tag, tf))
            tx = ix + dw_ - tgw - 22
            ty = py + dh - 36
            draw.rounded_rectangle([tx - 8, ty - 4, tx + tgw + 8, ty + 28],
                                   radius=8, fill="#333333")
            draw.text((tx, ty), tag, font=tf, fill="#ffffff")

    # ------------------------------------------------------------------ #
    # 整页
    # ------------------------------------------------------------------ #

    def render_page(self, pages: list, page_idx: int) -> Image.Image:
        img = Image.new("RGB", (W, H), BG)
        draw = ImageDraw.Draw(img)
        if not pages:
            self._draw_empty(draw)
            return img
        page_idx = min(max(page_idx, 0), len(pages) - 1)
        for card in pages[page_idx]["cards"]:
            self.draw_card(img, draw, card)
        return img

    def card_rects(self, pages: list) -> list:
        """每页卡片矩形（供设备端索引用）。"""
        return [[{
            "id": c["t"]["id"],
            "x": c["x"], "y": c["y"], "w": c["w"], "h": c["h"],
        } for c in pg["cards"]] for pg in pages]

    def _draw_empty(self, draw):
        f = self.font(32)
        msg = "还没有内容"
        w = self._text_width(draw, msg, f)
        draw.text(((W - w) / 2, 800), msg, font=f, fill=FG_DIM)
