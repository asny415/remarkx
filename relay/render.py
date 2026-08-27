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

_FONT_CANDIDATES = [
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
    for p in _FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    return ""


def clean_text(text: str) -> str:
    """隐藏链接、剥离 emoji，规整空白。"""
    text = _URL_RE.sub("", text or "")
    text = _EMOJI_RE.sub("", text)
    text = re.sub(r"[ \t]+", " ", text)
    return text.strip()


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

    def wrap_text(self, draw, text: str, font, max_width: float,
                  max_lines: int) -> list:
        """按像素宽度折行；CJK 逐字断行，ASCII 优先按空格断行。"""
        lines = []
        for raw in text.splitlines():
            if not raw:
                lines.append("")
                continue
            line = ""
            for token in re.findall(r"\S+|\s+", raw):
                if re.match(r"^\s+$", token):
                    line += token
                    continue
                if self._text_width(draw, token, font) > max_width:
                    for ch in token:
                        if line and self._text_width(draw, line + ch, font) \
                                > max_width:
                            lines.append(line)
                            line = ""
                        line += ch
                    continue
                if self._text_width(draw, line + token, font) <= max_width:
                    line += token
                    continue
                r = line.rstrip().rsplit(" ", 1)
                if len(r) == 2:
                    lines.append(r[0])
                    line = r[1] + " "
                else:
                    lines.append(line.rstrip())
                    line = ""
                if line and self._text_width(draw, line + token, font) \
                        <= max_width:
                    line += token
                else:
                    lines.append(token)
                    line = ""
            if line.rstrip():
                lines.append(line.rstrip())
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
            return clean_text(t["translated"])
        return clean_text(t.get("text", ""))

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
    # ops 按序存放，渲染时从 y0 开始顺序消耗竖直空间。

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
                     "is_cont": is_cont, "ops": []}
            # 顶部留白；续排块额外给“┆续”标记留一行高
            chunk["h"] += PAD_TOP + (24 if is_cont else 0)
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
            if not (t.get("text") or self._card_media(t)):
                continue

            # ---- 预生成原子与度量 ----
            lines = []
            text = self._card_text(t)
            if text:
                lines = self.wrap_text(draw, text, self.font(30),
                                       TEXT_W, 100000)

            img_payload = None
            img_h = 0
            media = self._card_media(t)
            if media:
                img = self._load_photo(media[0])
                if img:
                    rz, dwp, dhp = self._fit(img, TEXT_W, IMG_MAX_H)
                    img_h = dhp + IMG_GAP
                    img_payload = {
                        "photo": rz, "dw": dwp, "dh": dhp,
                        "is_video": media[0].get("type") == "video",
                        "n_media": len(media),
                        "y": 0,
                    }

            # ---- 放置：head(不可拆) → img(不可拆)? → 文本行逐行 ----
            # 需求必须包含“若需新开块”的顶部留白（续排块含┆续标记行高）
            chunk = None
            li = 0
            chunk_was_split = [False]

            def close():
                nonlocal chunk
                if chunk is not None:
                    chunk_was_split[0] = True
                chunk = None

            def open_cont_needed():
                return PAD_TOP + (24 if chunk_was_split[0] else 0)

            def place(kind):
                """返回 True 表示成功放置；False 表示需先跳槽重试。"""
                nonlocal chunk, li
                extra = 0 if chunk is not None else open_cont_needed()
                if kind == "head":
                    need = HEAD_H + extra
                    if free() < need:
                        return False
                    if chunk is None:
                        chunk = open_chunk(False, t)
                    chunk["ops"].append(("head", grow(chunk, HEAD_H)))
                    return True
                if kind == "img":
                    need = img_h + extra
                    if free() < need:
                        return False
                    if chunk is None:
                        chunk = open_chunk(False, t)
                    img_payload["y"] = grow(chunk, img_h)
                    chunk["ops"].append(("img", img_payload))
                    return True
                need = TEXT_LH + extra
                if free() < need:
                    return False
                if chunk is None:
                    chunk = open_chunk(bool(chunk_was_split[0]), t)
                yy = grow(chunk, TEXT_LH)
                ln = lines[li]
                chunk["ops"].append(("line", (yy, ln)))
                li += 1
                return True

            # 1) head（首块不会是续排；首次跳槽不设 split 标记）
            while not place("head"):
                jump()
            # 2) img
            if img_payload:
                while not place("img"):
                    close()
                    jump()
            # 3) 文本行
            while li < len(lines):
                if not place("txt"):
                    close()
                    jump()

            # 4) 底部内边距 + 卡间距（计入最后一个块的边界框）
            pad_needed = PAD_BOTTOM + CARD_GAP
            if chunk is not None and cur.y + pad_needed <= BOTTOM_Y:
                grow(chunk, pad_needed)

        return [pg for pg in pages if pg["cards"]]

        return [pg for pg in pages if pg["cards"]]

    # ------------------------------------------------------------------ #
    # 绘制单个 chunk
    # ------------------------------------------------------------------ #

    def draw_card(self, page: Image.Image, draw, card: dict):
        t = card["t"]
        x, y, w, h = card["x"], card["y"], card["w"], card["h"]

        draw.rounded_rectangle([x + 1, y + 1, x + w - 1, y + max(h, MIN_CHUNK_H) - 1],
                               radius=10, outline=CARD_BORDER, width=2)

        px = x + PAD

        if card.get("is_cont"):
            fc = self.font(20)
            draw.text((px, y + PAD_TOP - 18), "┆续", font=fc, fill=FG_FAINT)

        for kind, payload in card["ops"]:
            if kind == "head":
                self._draw_head(page, draw, t, px, payload)
            elif kind == "img":
                self._draw_img(page, draw, payload, px)
            elif kind == "line":
                yy, ln = payload
                if ln:
                    f_text = self.font(30)
                    safe = self._ellipsize(draw, ln, f_text, TEXT_W)
                    draw.text((px, yy), safe, font=f_text, fill=FG)

    def _draw_head(self, page, draw, t, px, py):
        f_name = self.font(26, bold=True)
        f_dim = self.font(24)
        name = t.get("author_name") or "?"
        handle = "@" + (t.get("author_handle") or "")
        prefix = ""
        if t.get("is_retweet"):
            prefix = "RT @" + (t.get("rt_handle") or "?") + " "

        combo_plain = (prefix + name + "  " + handle)
        if self._text_width(draw, combo_plain, f_name) <= TEXT_W:
            wx = 0
            if prefix:
                wp = int(self._text_width(draw, prefix, f_dim))
                draw.text((px + wx, py + 4), prefix, font=f_dim, fill=FG_FAINT)
                wx += wp
            draw.text((px + wx, py), name, font=f_name, fill=FG)
            wn = int(self._text_width(draw, name, f_name))
            draw.text((px + wx + wn + 10, py + 5), handle,
                      font=f_dim, fill=FG_DIM)
        else:
            combo = self._ellipsize(draw, combo_plain, f_dim, TEXT_W)
            draw.text((px, py), combo, font=f_dim, fill=FG_DIM)
        py += NAME_H + 6

        f_meta = self.font(22)
        media = self._card_media(t)
        videos = [m for m in media if m.get("type") == "video"]
        metas = [self.abs_time(t.get("created_at", ""))]
        if videos:
            metas.append("▶ 视频")
        n_imgs = len(media) - len(videos)
        if n_imgs:
            metas.append(f"{n_imgs} 图")
        meta_line = " · ".join(x for x in metas if x)
        draw.text((px, py), self._ellipsize(draw, meta_line, f_meta, TEXT_W),
                  font=f_meta, fill=FG_FAINT)

    def _draw_img(self, page, draw, info, px):
        photo, dw_, dh = info["photo"], info["dw"], info["dh"]
        py = info["y"]
        ix = px + (TEXT_W - dw_) // 2
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
            tw = int(self._text_width(draw, tag, tf))
            tx = ix + dw_ - tw - 22
            ty = py + dh - 36
            draw.rounded_rectangle([tx - 8, ty - 4, tx + tw + 8, ty + 28],
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
