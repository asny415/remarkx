"""X (Twitter) 抓取器：基于 twikit，登录小号，拉取 Home -> For You 时间线。

用法（在 relay.py 中）：
  await fetcher.login(...)   # 首次：交互式登录，保存会话
  await fetcher.poll()       # 按需调用：拉取新推文 + 下载图片 + 机翻中文
"""

import asyncio
import logging
import os

log = logging.getLogger("remarkx.fetch")


class XError(Exception):
    """登录/会话/网络类错误，message 会展示给用户。"""


class SessionError(XError):
    """会话失效——可通过从浏览器导入 Cookie 自动恢复。"""


class Fetcher:
    def __init__(self, config: dict, store):
        self.cfg = config
        self.store = store
        self.proxy = config.get("proxy") or None
        self.session_file = config["session_file"]
        self.media_dir = config["media_dir"]
        self.poll_count = int(config.get("poll_count", 30))
        # 浏览器名（可多个，逗号分隔），用于会话失效时自动导入 Cookie
        self.browsers = [b.strip() for b in
                         str(config.get("browser", "brave")).split(",")
                         if b.strip()]
        self._client = None
        self._timeline = None

    # ------------------------------------------------------------------ #
    # 会话恢复
    # ------------------------------------------------------------------ #

    def refresh_session(self) -> bool:
        """从本地浏览器重新导入 X Cookie 到会话文件，并重建客户端。

        成功返回 True。同步函数（读浏览器 Cookie 库会阻塞），
        调用方用 asyncio.to_thread 包裹。
        """
        from cookies_browser import try_import

        if not try_import(self.browsers, self.session_file):
            return False
        self._client = None
        client = self._new_client()
        client.load_cookies(self.session_file)
        self._client = client
        return True

    # ------------------------------------------------------------------ #
    # 客户端
    # ------------------------------------------------------------------ #

    def _new_client(self):
        import twikit

        return twikit.Client(proxy=self.proxy)

    async def ensure_client(self):
        if self._client is not None:
            return self._client
        if not os.path.exists(self.session_file):
            # 尚未登录也归为 SessionError：可尝试从浏览器导入 Cookie 自愈
            raise SessionError("尚未登录（无会话文件）。"
                               "可运行 python3 relay.py login，"
                               "或在浏览器登录 x.com 后刷新首页自动导入")
        client = self._new_client()
        client.load_cookies(self.session_file)
        self._client = client
        return client

    async def login(self, username: str, email: str = None,
                    password: str = None, totp_secret: str = None):
        """交互式登录（2FA 验证码在终端输入）。成功后保存会话文件。"""
        import getpass

        client = self._new_client()
        username = username or input("账号(用户名/邮箱/手机号): ").strip()
        email = email or input("备用邮箱(可选, 回车跳过): ").strip() or None
        password = password or getpass.getpass("密码: ")
        totp_secret = totp_secret or input(
            "2FA secret(可选, 回车跳过): ").strip() or None

        log.info("登录中，如出现 2FA 请在提示后输入验证码 ...")
        await client.login(
            auth_info_1=username,
            auth_info_2=email,
            password=password,
            totp_secret=totp_secret,
            cookies_file=self.session_file,
        )
        self._client = client
        log.info("登录成功，会话已保存: %s", self.session_file)
        return client

    # ------------------------------------------------------------------ #
    # 抓取
    # ------------------------------------------------------------------ #

    async def poll(self) -> int:
        """拉取最新一页 For You 时间线，入库新推文+图片+译文，返回新推文数。"""
        client = await self.ensure_client()
        try:
            result = await client.get_timeline(self.poll_count)
        except Exception as e:
            raise self._classify(e) from e
        self._timeline = result
        return await self._ingest(client, result)

    async def poll_extend(self) -> int:
        """续抓更早的 For You 时间线（cursor 向后翻），用于"书读不完"。"""
        client = await self.ensure_client()
        try:
            if self._timeline is None:
                result = await client.get_timeline(self.poll_count)
            else:
                result = await self._timeline.next()
        except Exception as e:
            raise self._classify(e) from e
        self._timeline = result
        return await self._ingest(client, result)

    async def _ingest(self, client, result) -> int:
        from translator import Translator

        new_items = []
        for tweet in result:
            item = self._normalize(tweet)
            if item is None:
                continue
            await self._download_media(client, item)
            if self.store.upsert_tweet(item):
                new_items.append(item)
        # 机翻中文（仅新推文；失败不影响主流程）
        tr = Translator(self.cfg)
        if tr.enabled and new_items:
            try:
                await tr.translate_items(
                    new_items, lambda i, zh: self.store.update_translation(i, zh))
            except Exception as e:
                log.warning("翻译批次失败: %s", e)
        return len(new_items)

    def _classify(self, e: Exception) -> XError:
        """把底层异常翻译成对用户友好的错误（会话失效 -> SessionError）。"""
        if isinstance(e, XError):
            return e
        try:
            from twikit import errors as te
        except ImportError:
            te = None
        if te is not None and isinstance(e, te.TwitterException):
            if isinstance(e, (te.Unauthorized, te.Forbidden)):
                return SessionError(f"登录态失效: {e}")
            if isinstance(e, te.AccountLocked):
                return XError("账号被临时锁定（可能触发风控），"
                              "稍后再试或检查该账号是否异常")
            if isinstance(e, te.AccountSuspended):
                return XError("账号已被停用")
            if isinstance(e, te.TooManyRequests):
                return XError("被限流(429)，请把 poll_seconds 调大")
        return XError(f"拉取时间线失败: {str(e)[:200]}")

    # ------------------------------------------------------------------ #
    # 数据规整
    # ------------------------------------------------------------------ #

    def _normalize(self, t) -> dict:
        """twikit.Tweet -> 内部 dict。retweeted 展开为原推文。"""
        try:
            orig = t.retweeted_tweet if t.retweeted_tweet is not None else t
            author = orig.user or t.user
            name = author.name if author else ""
            handle = (author.screen_name if author else "").lstrip("@")

            media = []
            for m in (orig.media or []):
                mtype = getattr(m, "type", "") or "photo"
                if mtype not in ("photo", "video", "animated_gif"):
                    continue
                media.append({
                    "url": m.media_url or m.url,
                    "w": int(m.width or 0),
                    "h": int(m.height or 0),
                    "path": "",
                    "type": "video" if mtype != "photo" else "photo",
                })

            return {
                "id": str(t.id),
                "created_at": t.created_at,
                "author_name": name,
                "author_handle": handle,
                # 长推(note_tweet)用全文，避免 X 时间线接口返回的截断文本
                "text": (getattr(orig, "full_text", None)
                         or orig.text or "").strip(),
                "is_retweet": t.retweeted_tweet is not None,
                "rt_handle": (
                    (t.user.screen_name if t.user else "").lstrip("@")
                    if t.retweeted_tweet is not None else ""
                ),
                "media": media,
                "url": f"https://x.com/{handle}/status/{t.id}",
            }
        except Exception as e:
            log.warning("跳过无法解析的推文: %s", e)
            return None

    async def _download_media(self, client, item: dict) -> None:
        """下载推文图片到 media_dir，成功后把本地相对路径写回 item。"""
        if not item["media"]:
            return
        os.makedirs(self.media_dir, exist_ok=True)
        for i, m in enumerate(item["media"]):
            path = os.path.join(self.media_dir, f"{item['id']}_{i}.jpg")
            try:
                resp = await client.http.get(m["url"], timeout=30)
                if resp.status_code != 200:
                    log.warning("媒体下载 %s -> HTTP %s", m["url"][:80],
                                resp.status_code)
                    continue
                data = resp.content
                ctype = (resp.headers.get("Content-Type") or "").lower()
                if "png" in ctype:
                    path = os.path.join(self.media_dir, f"{item['id']}_{i}.png")
                with open(path, "wb") as f:
                    f.write(data)
                m["path"] = os.path.relpath(path, self.media_dir)
            except Exception as e:
                log.warning("媒体下载失败 %s: %s", m["url"][:80], e)
        # 下载完成后更新数据库里的 media 列表（含本地路径）
        import json
        self._update_media(item)

    def _update_media(self, item: dict) -> None:
        import json
        self.store.update_media(
            item["id"], json.dumps(item["media"], ensure_ascii=False))
