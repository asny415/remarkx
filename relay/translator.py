"""机翻到中文，写入 tweets.translated，渲染端优先显示。

后端策略（translate_backend 配置，默认 mymemory，失败自动切换另一个）：
- mymemory : 免费匿名接口（无 key），500 字节/请求 → 自动分块；
             配置 translate_email 可把每日额度从 5k 提到 50k 字符
- google   : 免费 gtx 接口，出口 IP 被限流时不可用

只翻译“新入库”的推文；已是中文则跳过；单条失败仅告警留空，
绝不影响抓取主流程。
"""

import asyncio
import logging
import re

import httpx

log = logging.getLogger("remarkx.tr")

_CJK_RE = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")
_URL_RE = re.compile(r"https?://\S+")
_CHUNK = 400                     # mymemory 500B 上限，留安全余量


def _mostly_cjk(text: str) -> bool:
    """文本中的中日韩字符占比是否足以认为读者能看懂。"""
    letters = [c for c in text if c.isalpha()]
    if len(letters) < 8:
        return False
    cjk = sum(1 for c in letters if _CJK_RE.match(c))
    return cjk / max(len(letters), 1) >= 0.3


def _chunks(text: str, limit: int = _CHUNK) -> list:
    """按句/词边界把长文切成 ≤limit 的块。"""
    if len(text) <= limit:
        return [text]
    out, cur = [], ""
    for piece in re.split(r"(?<=[。！？!?.\n])\s*", text):
        while len(piece) > limit:
            if cur:
                out.append(cur)
                cur = ""
            out.append(piece[:limit])
            piece = piece[limit:]
        if len(cur) + len(piece) > limit:
            out.append(cur)
            cur = piece
        else:
            cur += piece
    if cur:
        out.append(cur)
    return out


class Translator:
    def __init__(self, config: dict):
        self.lang = str(config.get("translate", "zh-CN")).strip()
        self.backend = str(config.get("translate_backend", "mymemory")).strip()
        self.email = str(config.get("translate_email", "")).strip()
        self.proxy = config.get("proxy") or None
        self._sem = asyncio.Semaphore(2)

    @property
    def enabled(self) -> bool:
        return bool(self.lang)

    # ---------------- mymemory ---------------- #

    async def _mm_chunk(self, cli, text: str) -> str:
        r = await cli.get(
            "https://api.mymemory.translated.net/get",
            params={"q": text, "langpair": f"Autodetect|{self.lang}",
                    **({"de": self.email} if self.email else {})})
        r.raise_for_status()
        j = r.json()
        if str(j.get("responseStatus")) not in ("200", "None", "0"):
            raise RuntimeError(f"mymemory status={j.get('responseStatus')}")
        out = j.get("responseData", {}).get("translatedText", "")
        if "MYMEMORY WARNING" in out.upper():
            raise RuntimeError("mymemory quota exceeded")
        return out

    async def _mymemory(self, cli, text: str) -> str:
        parts = []
        for ch in _chunks(text):
            parts.append(await self._mm_chunk(cli, ch))
            await asyncio.sleep(0.3)          # 礼貌限速
        return "".join(parts)

    # ---------------- google gtx ---------------- #

    async def _google(self, cli, text: str) -> str:
        out = []
        for ch in _chunks(text):
            r = await cli.get(
                "https://translate.googleapis.com/translate_a/single",
                params={"client": "gtx", "sl": "auto", "tl": self.lang,
                        "dt": "t", "q": ch})
            r.raise_for_status()
            data = r.json()
            for part in (data[0] or []):
                if part and part[0]:
                    out.append(part[0])
        return "".join(out).strip()

    # ---------------- 对外 ---------------- #

    _BACKENDS = {"mymemory": "_mymemory", "google": "_google"}

    async def translate(self, text: str) -> str:
        """返回译文；空串表示不需要/失败（渲染端回退原文）。"""
        text = (text or "").strip()
        if not self.enabled or not text or _mostly_cjk(_URL_RE.sub("", text)):
            return ""

        order = [self.backend] + [b for b in self._BACKENDS
                                  if b != self.backend]
        async with self._sem:
            for name in order:
                fn = getattr(self, self._BACKENDS[name])
                for attempt in (1, 2):
                    try:
                        async with httpx.AsyncClient(
                                proxy=self.proxy, timeout=20,
                                headers={"User-Agent": "Mozilla/5.0"}) as cli:
                            out = await fn(cli, text)
                        if out.strip():
                            return out.strip()
                    except Exception as e:
                        log.warning("翻译[%s/%s]失败: %s", name, attempt, e)
                        await asyncio.sleep(1.0 * attempt)
        return ""

    async def translate_items(self, items: list, save) -> None:
        """并发翻译一批推文 dict（含 id/text），成功后调用 save(id, 译文)。"""
        if not self.enabled or not items:
            return

        async def one(item):
            zh = await self.translate(item.get("text", ""))
            if zh:
                save(item["id"], zh)

        await asyncio.gather(*(one(i) for i in items),
                             return_exceptions=True)
