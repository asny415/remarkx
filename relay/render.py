"""页面渲染器：把一页推文画成 1404x1872 的 PNG（reMarkable 1 原生分辨率）。

设备端只用 `simple` 显示这张 PNG + 几个 SAS 按钮，所以排版质量全在这里决定。
"""

import logging
import os
import re
from datetime import datetime, timezone

from PIL import Image, ImageDraw, ImageFont

log = logging.getLogger("remarkx.render")

# reMarkable 1 屏幕
W, H = 1404, 1872
MARGIN = 48
HEADER_Y = 44
HEADER_LINE_Y = 108
FOOTER_Y = 1696          # 底部按钮区（SAS 按钮画在这里，PNG 留白）
CONTENT_TOP = 140
CONTENT_BOTTOM = FOOTER_Y - 16

# 颜色（护眼：纯白底 + 深灰字，避免纯黑刺眼）
BG = "#ffffff"
FG = "#1a1a1a"
FG_DIM = "#5a5a5a"
FG_FAINT = "#8a8a8a"
RULE = "#d8d8d8"

PHOTO_MAX_H = 470        # 单图最大高度
PHOTO_ROW_H = 420        # 双图行高
MAX_TEXT_LINES = 8       # 单条推文最多文字行数

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


class Renderer:
    def __init__(self, media_dir: str, title: str = "X · Following",
                 font_path: str = ""):
        self.media_dir = media_dir
        self.title = title
        self.font_path = font_path or find_cjk_font()
        if not self.font_path:
            log.warning("未找到中文字体，中文将显示为方块。"
                        "Ubuntu: sudo apt install fonts-noto-cjk")
        self._fonts = {}

    def font(self, size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
        key = (size, bold)
        if key not in self._fonts:
            path = self.font_path
            if not path:
                return ImageFont.load_default()
            # 有 Bold 变体且要粗体时优先用
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
        except AttributeError:  # 老版本 PIL
            return draw.textsize(text, font=font)[0]

    def wrap_text(self, draw, text: str, font, max_width: float,
                  max_lines: int) -> list:
        """按像素宽度折行；CJK 可在任意字符间断行，ASCII 优先按空格断行。"""
        lines = []
        for raw in text.splitlines():
            if not raw:
                lines.append("")
                continue
            line = ""
            for token in re.findall(r"\S+|\s+", raw):
                if re.match(r"^\s+$", token) and not line:
                    continue
                if line and self._text_width(draw, line + token, font) > max_width:
                    # 尝试按空格断行
                    if " " in line + token:
                        cut = (line + token).rstrip().rsplit(" ", 1)[0]
                        lines.append(cut)
                        line = (line + token).rstrip().rsplit(" ", 1)[1]
                    else:
                        lines.append(line)
                        line = token
                else:
                    line += token
            if line:
                lines.append(line)
            if len(lines) >= max_lines + 1:
                break
        if len(lines) > max_lines:
            lines = lines[:max_lines]
            lines[-1] = lines[-1].rstrip() + " …"
        return lines

    @staticmethod
    def parse_created_at(s: str):
        """X legacy 时间: 'Wed Aug 26 06:00:00 +0000 2026' -> datetime(UTC)"""
        try:
            return datetime.strptime(s, "%a %b %d %H:%M:%S %z %Y")
        except Exception:
            return None

    @staticmethod
    def rel_time(s: str) -> str:
        dt = Renderer.parse_created_at(s)
        if dt is None:
            return s or ""
        now = datetime.now(timezone.utc)
        d = (now - dt).total_seconds()
        if d < 90:
            return "刚刚"
        if d < 3600:
            return f"{int(d // 60)} 分钟前"
        if d < 86400:
            return f"{int(d // 3600)} 小时前"
        if d < 86400 * 7:
            return f"{int(d // 86400)} 天前"
        return dt.astimezone().strftime("%m-%d %H:%M")

    # ------------------------------------------------------------------ #
    # 照片
    # ------------------------------------------------------------------ #

    def _load_photo(self, m: dict):
        if not m.get("path"):
            return None
        p = os.path.join(self.media_dir, m["path"])
        try:
            return Image.open(p).convert("RGB")
        except Exception as e:
            log.warning("打开图片失败 %s: %s", p, e)
            return None

    @staticmethod
    def _fit(img, box_w: int, box_h: int):
        """等比缩放到 box 内，返回 (resized, dw, dh)。"""
        w, h = img.size
        scale = min(box_w / w, box_h / h, 1.0)
        dw = max(1, int(w * scale))
        dh = max(1, int(h * scale))
        return img.resize((dw, dh), Image.LANCZOS), dw, dh

    # ------------------------------------------------------------------ #
    # 单条推文
    # ------------------------------------------------------------------ #

    def measure_tweet(self, draw, t: dict, content_w: int):
        """返回 (text_lines, photo_layout, total_height)。

        photo_layout: [] 或 {'mode': 'one', ...} / {'mode': 'rows', 'rows': [[m,...]]}
        """
        f_text = self.font(30)
        lines = self.wrap_text(draw, t["text"], f_text, content_w,
                               MAX_TEXT_LINES)
        h = 44 + 8  # 作者行 + 间距

        photos = [m for m in t.get("media", []) if m.get("path")]
        photo_layout = None
        if photos:
            if len(photos) == 1:
                photo_layout = {"mode": "one", "photos": photos[:1]}
                h += PHOTO_MAX_H + 12
            else:
                rows = [photos[i:i + 2] for i in range(0, len(photos), 2)]
                photo_layout = {"mode": "rows", "rows": rows[:2]}
                if len(photos) > 4:
                    photo_layout["extra"] = len(photos) - 4
                h += len(rows) * (PHOTO_ROW_H + 12)
        h += 26  # 分隔线间距
        return lines, photo_layout, h

    def draw_tweet(self, page, draw, t: dict, x: int, y: int,
                   content_w: int, lines, photo_layout) -> int:
        """画一条推文（贴图到 page 上），返回结束 y。"""
        f_name = self.font(28, bold=True)
        f_dim = self.font(24)
        f_text = self.font(30)
        line_h = 44

        # 作者行
        if t.get("is_retweet"):
            draw.text((x, y), f"RT @{t.get('rt_handle') or '?'}",
                      font=f_dim, fill=FG_FAINT)
            x_off = int(self._text_width(draw, "RT @?" + (t.get("rt_handle") or ""), f_dim))
            name_x = x + x_off + 12
        else:
            name_x = x
        draw.text((name_x, y), t.get("author_name") or "?",
                  font=f_name, fill=FG)
        w_name = self._text_width(draw, t.get("author_name") or "?", f_name)
        handle = "@" + (t.get("author_handle") or "")
        draw.text((name_x + w_name + 12, y + 4), handle, font=f_dim,
                  fill=FG_DIM)
        w_h = self._text_width(draw, handle, f_dim)
        time_str = self.rel_time(t.get("created_at", ""))
        draw.text((name_x + w_name + 12 + w_h + 12, y + 4),
                  "· " + time_str, font=f_dim, fill=FG_FAINT)
        y += 44 + 8

        # 正文
        for ln in lines:
            if ln:
                draw.text((x, y), ln, font=f_text, fill=FG)
            y += line_h

        # 图片
        if photo_layout:
            y += 12
            if photo_layout["mode"] == "one":
                m = photo_layout["photos"][0]
                img = self._load_photo(m)
                if img:
                    box_w = content_w
                    box_h = PHOTO_MAX_H
                    rz, dw, dh = self._fit(img, box_w, box_h)
                    px = x + (box_w - dw) // 2
                    draw.rectangle([px, y, px + dw, y + dh], outline=RULE)
                    page.paste(rz, (px, y))
                y += PHOTO_MAX_H
            else:
                col_w = (content_w - 24) // 2
                for row in photo_layout["rows"]:
                    for i, m in enumerate(row):
                        img = self._load_photo(m)
                        slot_x = x + i * (col_w + 24)
                        if img:
                            rz, dw, dh = self._fit(img, col_w, PHOTO_ROW_H)
                            px = slot_x + (col_w - dw) // 2
                            py = y + (PHOTO_ROW_H - dh) // 2
                            draw.rectangle([slot_x, y, slot_x + col_w,
                                            y + PHOTO_ROW_H], outline=RULE)
                            page.paste(rz, (px, py))
                    y += PHOTO_ROW_H + 12
                if photo_layout.get("extra"):
                    draw.text((x, y - 4), f"另有 {photo_layout['extra']} 张图…",
                              font=f_dim, fill=FG_FAINT)
                    y += 34

        # 分隔线
        draw.line([(x, y + 13), (x + content_w, y + 13)], fill=RULE, width=1)
        return y + 26

    # ------------------------------------------------------------------ #
    # 整页
    # ------------------------------------------------------------------ #

    def render_page(self, tweets: list, page: int, pages: int,
                    status: dict) -> Image.Image:
        img = Image.new("RGB", (W, H), BG)
        draw = ImageDraw.Draw(img)
        content_w = W - 2 * MARGIN

        # 头部
        f_title = self.font(36, bold=True)
        f_small = self.font(24)
        draw.text((MARGIN, HEADER_Y), self.title, font=f_title, fill=FG)
        right = f"第 {page + 1}/{pages} 页 · 共 {status.get('count', 0)} 条"
        if status.get("last_poll"):
            right += " · 更新 " + status["last_poll"][-8:]
        draw.text((W - MARGIN - self._text_width(draw, right, f_small),
                   HEADER_Y + 12), right, font=f_small, fill=FG_FAINT)
        draw.line([(MARGIN, HEADER_LINE_Y), (W - MARGIN, HEADER_LINE_Y)],
                  fill=RULE, width=2)

        # 推文
        y = CONTENT_TOP
        drawn = 0
        for t in tweets:
            lines, photo_layout, h = self.measure_tweet(draw, t, content_w)
            if y + h > CONTENT_BOTTOM and drawn > 0:
                break
            y = self.draw_tweet(img, draw, t, MARGIN, y, content_w, lines,
                                photo_layout)
            drawn += 1
            if y > CONTENT_BOTTOM:
                break

        if drawn == 0 and not tweets:
            self._draw_empty(draw, status)

        # 底部按钮区留白（SAS 按钮由设备端绘制）
        draw.line([(MARGIN, FOOTER_Y), (W - MARGIN, FOOTER_Y)],
                  fill=RULE, width=2)
        return img

    def _draw_empty(self, draw, status: dict):
        f = self.font(32)
        msg = []
        err = status.get("last_error", "")
        if not status.get("count"):
            msg.append("还没有数据")
            if "尚未登录" in err:
                msg.append("（未登录：在电脑上运行  python3 relay.py login ）")
            elif err:
                msg.append(f"（{err[:60]}）")
        else:
            msg.append("（本页没有内容）")
        for i, m in enumerate(msg):
            w = self._text_width(draw, m, f)
            draw.text(((W - w) / 2, 600 + i * 56), m, font=f, fill=FG_DIM)
