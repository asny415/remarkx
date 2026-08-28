"""X (Twitter) 抓取器：直接发带 Cookie 的 GraphQL 请求拉 For You 时间线。

不走 twikit——X 网页端的自动翻译（Grok 译文）直接内嵌在时间线响应里
（tweet_results.result.grok_translated_post_with_availability），本抓取器
优先保存译文，没有译文时才用原文。

用法（在 relay.py 中）：
  await fetcher.poll()        # 拉取新推文 + 下载图片（译文优先）
  await fetcher.poll_extend() # 用 cursor 续抓更早内容
会话来自 data/session.json（{cookie名: 值}），可由 cookies_browser
从本机浏览器自动导入；也可 python3 relay.py login 触发导入。
"""

import asyncio
import json
import logging
import os

import httpx

log = logging.getLogger("remarkx.fetch")

# x.com 网页端公开 Bearer Token（所有匿名/登录请求共用；2026-08 已轮换，
# 失效时从浏览器 DevTools 里 authorization 头重新复制）
_BEARER = ("Bearer AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs"
           "%3D1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA")
_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
       "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")

# For You 时间线（2026-08 从网页端实测捕获；queryId 变了就重新抓包）
# count/includePromotedContent 与网页端真实请求一致（无头浏览器抓包对比：
# 网页端 count=20, includePromotedContent=true，程序此前写成了 30/false）
_OP_PATH = "wp06oo3fRGU4P1sK8rECqQ/HomeTimeline"
# Following（正在关注）时间线：queryId 取自网页端 JS bundle（2026-08）
_OP_FOLLOWING = "BLQWpfVqtgBqAqwRRJcJjA/HomeLatestTimeline"
_VARIABLES = {"count": 20, "includePromotedContent": True,
              "requestContext": "launch", "withCommunity": True}
_FEATURES = {
    "rweb_video_screen_enabled": False,
    "rweb_cashtags_enabled": True,
    "profile_label_improvements_pcf_label_in_post_enabled": True,
    "responsive_web_profile_redirect_enabled": True,
    "rweb_tipjar_consumption_enabled": False,
    "verified_phone_label_enabled": False,
    "creator_subscriptions_tweet_preview_api_enabled": True,
    "responsive_web_graphql_timeline_navigation_enabled": True,
    "premium_content_api_read_enabled": False,
    "communities_web_enable_tweet_community_results_fetch": True,
    "c9s_tweet_anatomy_moderator_badge_enabled": True,
    "responsive_web_grok_analyze_button_fetch_trends_enabled": False,
    "responsive_web_grok_analyze_post_followups_enabled": True,
    "rweb_cashtags_composer_attachment_enabled": True,
    "responsive_web_jetfuel_frame": True,
    "responsive_web_grok_share_attachment_enabled": True,
    "responsive_web_grok_annotations_enabled": True,
    "articles_preview_enabled": True,
    "responsive_web_edit_tweet_api_enabled": True,
    "rweb_conversational_replies_downvote_enabled": False,
    "graphql_is_translatable_rweb_tweet_is_translatable_enabled": True,
    "view_counts_everywhere_api_enabled": True,
    "longform_notetweets_consumption_enabled": True,
    "responsive_web_twitter_article_tweet_consumption_enabled": True,
    "content_disclosure_indicator_enabled": True,
    "content_disclosure_ai_generated_indicator_enabled": True,
    "responsive_web_grok_show_grok_translated_post": True,
    "responsive_web_grok_analysis_button_from_backend": True,
    "post_ctas_fetch_enabled": False,
    "freedom_of_speech_not_reach_fetch_enabled": True,
    "standardized_nudges_misinfo": True,
    "tweet_with_visibility_results_prefer_gql_limited_actions_policy_enabled": True,
    "longform_notetweets_rich_text_read_enabled": True,
    "longform_notetweets_inline_media_enabled": False,
    "responsive_web_grok_image_annotation_enabled": True,
    "responsive_web_grok_imagine_annotation_enabled": True,
    "responsive_web_grok_community_note_auto_translation_is_enabled": True,
    "responsive_web_enhance_cards_enabled": False,
}


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
        self._api = None          # 带 Cookie 的 API 客户端
        self._dl = None           # 无 Cookie 的下载客户端（pbs.twimg.com 等）
        self._cursor = None       # For You 向后翻页游标（内存态，重启即失）
        self._cursor_following = None  # Following 向后翻页游标

    # ------------------------------------------------------------------ #
    # 会话
    # ------------------------------------------------------------------ #

    def refresh_session(self) -> bool:
        """从本地浏览器重新导入 X Cookie 到会话文件，并重建客户端。

        成功返回 True。同步函数（读浏览器 Cookie 库会阻塞），
        调用方用 asyncio.to_thread 包裹。
        """
        from cookies_browser import try_import

        if not try_import(self.browsers, self.session_file):
            return False
        self._api = None
        return True

    async def login(self, username: str = "", email: str = None,
                    password: str = None, totp_secret: str = None):
        """兼容旧的 login 入口：直连 Cookie 模式不再支持密码登录，
        改为从浏览器导入 Cookie。"""
        if self.refresh_session():
            log.info("已从浏览器导入 X Cookie: %s", self.session_file)
            return
        raise XError("无法获取登录态：请在浏览器登录 x.com 后重试"
                     "（或确认 config.json 的 browser 指向已登录的浏览器）")

    # ------------------------------------------------------------------ #
    # HTTP 客户端
    # ------------------------------------------------------------------ #

    def _load_cookies(self) -> dict:
        if not os.path.exists(self.session_file):
            raise SessionError(
                "尚未登录（无会话文件）。"
                "可在浏览器登录 x.com 后运行 python3 relay.py login，"
                "或刷新首页自动导入")
        with open(self.session_file, encoding="utf-8") as f:
            data = json.load(f)
        # 兼容 twikit 会话文件（{"cookies": {...}} 或平铺 {名: 值}）
        if isinstance(data, dict) and isinstance(data.get("cookies"), dict):
            data = data["cookies"]
        if "auth_token" not in data or "ct0" not in data:
            raise SessionError("会话文件缺少 auth_token/ct0，登录态无效，"
                               "请从浏览器重新导入 Cookie")
        return data

    def _cookie_header(self, c: dict) -> str:
        parts = [f"{k}={c[k]}" for k in
                 ("auth_token", "ct0", "twid", "guest_id") if c.get(k)]
        return "; ".join(parts)

    async def ensure_client(self) -> httpx.AsyncClient:
        if self._api is not None:
            return self._api
        cookies = self._load_cookies()
        self._api = httpx.AsyncClient(
            proxy=self.proxy, timeout=30,
            headers={
                "User-Agent": _UA,
                "Authorization": _BEARER,
                "X-Csrf-Token": cookies["ct0"],
                "X-Twitter-Auth-Type": "OAuth2Session",
                "X-Twitter-Active-User": "yes",
                "Cookie": self._cookie_header(cookies),
                "Accept": "*/*",
                "Referer": "https://x.com/",
                "Origin": "https://x.com",
            },
        )
        return self._api

    def _dl_client(self) -> httpx.AsyncClient:
        """媒体下载用独立客户端，不携带 X 会话 Cookie。"""
        if self._dl is None:
            self._dl = httpx.AsyncClient(proxy=self.proxy, timeout=30,
                                         headers={"User-Agent": _UA})
        return self._dl

    # ------------------------------------------------------------------ #
    # 抓取
    # ------------------------------------------------------------------ #

    async def _fetch_page(self, cursor: str = None,
                          op: str = _OP_PATH,
                          variables_extra: dict = None) -> dict:
        cli = await self.ensure_client()
        variables = dict(_VARIABLES)
        variables["count"] = max(1, min(self.poll_count, 50))
        if variables_extra:
            variables.update(variables_extra)
        if cursor:
            variables["cursor"] = cursor
        params = {
            "variables": json.dumps(variables, separators=(",", ":")),
            "features": json.dumps(_FEATURES, separators=(",", ":")),
        }
        try:
            r = await cli.get(f"https://x.com/i/api/graphql/{op}",
                              params=params)
        except httpx.HTTPError as e:
            raise XError(f"网络错误: {e}") from e
        if r.status_code in (401, 403):
            raise SessionError(f"登录态失效 (HTTP {r.status_code})，"
                               "将从浏览器重新导入 Cookie")
        if r.status_code == 429:
            raise XError("被限流(429)，请把 poll_seconds 调大")
        if r.status_code != 200:
            raise XError(f"拉取时间线失败: HTTP {r.status_code} "
                         f"{r.text[:200]}")
        try:
            return r.json()
        except ValueError as e:
            raise XError(f"响应不是 JSON: {r.text[:200]}") from e

    async def _fetch_following(self, cursor: str = None) -> dict:
        """拉取 Following（正在关注）时间线，顺时序。"""
        return await self._fetch_page(
            cursor=cursor, op=_OP_FOLLOWING,
            variables_extra={"includePromotedContent": False})

    @staticmethod
    def _merge(fy: list, fl: list) -> list:
        """For You + Following 按推文 id 去重后 1:1 交错合并。

        两组内容在书里交错排列，翻页一起翻；同一条推文只出现一次。
        """
        seen = set()
        out = []
        i = j = 0
        while i < len(fy) or j < len(fl):
            if i < len(fy):
                t = fy[i]
                if t["id"] not in seen:
                    seen.add(t["id"])
                    out.append(t)
                i += 1
            if j < len(fl):
                t = fl[j]
                if t["id"] not in seen:
                    seen.add(t["id"])
                    out.append(t)
                j += 1
        return out

    async def poll(self) -> tuple:
        """拉取最新一页 For You + Following 时间线，合并入库。

        返回 (合并后本轮全部条目, 新入库条数)。刷新即阅读内容的重建。
        """
        fy_data = await self._fetch_page()
        fy_items, self._cursor = self._parse_timeline(fy_data)
        fl_data = await self._fetch_following()
        fl_items, self._cursor_following = self._parse_timeline(fl_data)
        merged = self._merge(fy_items, fl_items)
        return await self._ingest(merged)

    async def poll_extend(self) -> tuple:
        """续抓 For You + Following 更早内容（各自 cursor 向后翻），合并追加。

        返回同 poll()。
        """
        if self._cursor or self._cursor_following:
            fy_batch, fl_batch = [], []
            if self._cursor:
                data = await self._fetch_page(cursor=self._cursor)
                fy_batch, self._cursor = self._parse_timeline(data)
            if self._cursor_following:
                data = await self._fetch_following(
                    cursor=self._cursor_following)
                fl_batch, self._cursor_following = self._parse_timeline(data)
            merged = self._merge(fy_batch, fl_batch)
            if merged:
                return await self._ingest(merged)
        log.info("尚无翻页游标，本次拉取最新一页")
        return await self.poll()

    async def _ingest(self, items: list) -> tuple:
        # 媒体改为按页懒加载（见 ensure_media）：抓取阶段只入库不下载，
        # 渲染某页前才并发补下该页推文的图片/头像，首屏不再被全量下载拖慢。
        new = 0
        for item in items:
            if self.store.upsert_tweet(item):
                new += 1
        return items, new

    async def ensure_media(self, tweets: list) -> None:
        """按需下载给定推文的缺失媒体/头像（已缓存自动跳过）。"""
        sem = asyncio.Semaphore(4)

        async def one(t):
            async with sem:
                await self._download_media(t)
                await self._download_avatar(t)

        await asyncio.gather(*(one(t) for t in tweets if t))

    # ------------------------------------------------------------------ #
    # 响应解析
    # ------------------------------------------------------------------ #

    @staticmethod
    def _unwrap_result(result: dict):
        """TweetWithVisibilityResults -> 内层 Tweet。"""
        if not isinstance(result, dict):
            return None
        if result.get("__typename") == "TweetWithVisibilityResults":
            result = result.get("tweet") or {}
        return result if result.get("__typename") == "Tweet" else None

    def _parse_timeline(self, data: dict) -> tuple:
        """GraphQL HomeTimeline JSON -> (items, bottom_cursor)。"""
        items, cursor = [], None
        try:
            instructions = (data["data"]["home"]
                            ["home_timeline_urt"]["instructions"])
        except (KeyError, TypeError):
            raise XError("时间线响应结构变化，无法解析")
        for ins in instructions:
            if ins.get("type") != "TimelineAddEntries":
                continue
            for entry in ins.get("entries", []):
                eid = entry.get("entryId", "")
                content = entry.get("content", {})
                if content.get("entryType") == "TimelineTimelineCursor":
                    if "cursor-bottom" in eid:
                        cursor = content.get("value") or cursor
                    continue
                if eid.startswith("promoted"):
                    continue  # 广告
                # TimelineTimelineItem: content.itemContent
                # TimelineTimelineModule(线程): content.items[].item.itemContent
                contents = [content.get("itemContent")]
                if not contents[0] and isinstance(content.get("items"), list):
                    contents = [it.get("item", {}).get("itemContent")
                                for it in content["items"]]
                for ic in contents:
                    if not ic or ic.get("__typename") != "TimelineTweet":
                        continue
                    item = self._normalize(ic.get("tweet_results", {})
                                           .get("result"))
                    if item:
                        items.append(item)
        return items, cursor

    def _normalize(self, result) -> dict:
        """GraphQL tweet result -> 内部 dict。

        - 纯转推：外层=转发者（header 展示转发者），原帖进 quoted 块；
          id 归一到原帖 id，转推件与原推同屏时自然去重；
        - 引用推文：外层文本=转发者评论，被引用原帖进 quoted 块；
        - 普通推文：无 quoted 块。
        各级都带 stats（转发/点赞/评论/阅读数）。
        """
        try:
            outer = self._unwrap_result(result)
            if outer is None:
                return None
            legacy = outer.get("legacy", {})
            rt = legacy.get("retweeted_status_result")
            quoted_res = (outer.get("quoted_status_result")
                          or legacy.get("quoted_status_result"))
            if rt:
                orig = self._unwrap_result(rt.get("result"))
                if orig is None:
                    return None
                quoted_src = orig
            elif quoted_res:
                quoted_src = self._unwrap_result(quoted_res.get("result"))
            else:
                quoted_src = None

            name, handle, avatar = self._author_info(outer)

            def _raw_text(r):
                lg = r.get("legacy", {}) or {}
                return (self._note_text(r)
                        or lg.get("full_text", "")).strip()

            def _text_of(r):
                """译文优先：Grok 自动翻译内嵌在各级响应里。"""
                g = (r.get("grok_translated_post_with_availability") or {})
                d = g.get("data") or {}
                return (d.get("translation") or "").strip() or _raw_text(r)

            if rt:
                comment = ""                       # 纯转推无评论
                main_text = _text_of(orig)
                main_raw = _raw_text(orig)
                media = self._media_list(orig)
                stats = self._stats_of(orig)
                q_media = media                    # 同一列表对象：下载路径共享
                q_stats = stats
                q_created = ((orig.get("legacy") or {}).get("created_at") or "")
            elif quoted_src:
                comment = _text_of(outer)          # 引用者的评论
                main_text = comment
                main_raw = _raw_text(outer)
                media = self._media_list(outer)
                stats = self._stats_of(outer)
                q_media = self._media_list(quoted_src)
                q_stats = self._stats_of(quoted_src)
                q_created = ((quoted_src.get("legacy") or {})
                             .get("created_at") or "")
            else:
                comment = ""
                main_text = _text_of(outer)
                main_raw = _raw_text(outer)
                media = self._media_list(outer)
                stats = self._stats_of(outer)
                q_media = []
                q_stats = {}
                q_created = ""

            if rt:
                # 原帖 id：转推件与原推同屏时按原帖 id 去重
                oid = (orig.get("rest_id")
                       or (orig.get("legacy") or {}).get("id_str", "")
                       or outer.get("rest_id") or legacy.get("id_str", ""))
            else:
                oid = (outer.get("rest_id") or legacy.get("id_str", ""))
            if not (main_text or comment):
                return None

            quoted = None
            if quoted_src is not None:
                qname, qhandle, _ = self._author_info(quoted_src)
                quoted = {
                    "author_name": qname,
                    "author_handle": qhandle,
                    "text": _text_of(quoted_src),
                    "created_at": q_created,
                    "media": q_media,
                    "stats": q_stats,
                }

            return {
                "id": str(oid),
                "created_at": legacy.get("created_at", ""),
                "author_name": name,
                "author_handle": handle,
                # 译文优先，无译文用原文
                "text": main_text,
                "original_text": main_raw,
                "comment": comment,
                "is_retweet": rt is not None,
                "rt_handle": handle if rt else "",
                "quoted": quoted,
                "stats": stats,
                "media": media,
                "url": f"https://x.com/{handle}/status/{oid}",
                "avatar": avatar.replace("_normal.", "_bigger."),
                "lang": ((orig if rt else outer).get("legacy") or {})
                        .get("lang", ""),
                "source_lang": ((outer.get(
                    "grok_translated_post_with_availability") or {})
                    .get("data") or {}).get("source_language", ""),
                "dest_lang": ((outer.get(
                    "grok_translated_post_with_availability") or {})
                    .get("data") or {}).get("destination_language", ""),
            }
        except Exception as e:
            log.warning("跳过无法解析的推文: %s", e)
            return None

    @staticmethod
    def _author_info(r: dict) -> tuple:
        """tweet result -> (name, handle, avatar)。"""
        core = r.get("core", {}).get("user_results", {}).get("result")
        if not isinstance(core, dict):
            core = {}
        ucore = core.get("core") or {}       # 2026 版：name/screen_name 在 core
        ulegacy = core.get("legacy") or {}   # 旧版兼容
        name = ucore.get("name") or ulegacy.get("name", "")
        handle = ((ucore.get("screen_name")
                   or ulegacy.get("screen_name", "")) or "").lstrip("@")
        avatar = ((core.get("avatar") or {}).get("image_url")
                  or ulegacy.get("profile_image_url_https", "") or "")
        return name, handle, avatar

    @staticmethod
    def _stats_of(r: dict) -> dict:
        """tweet result -> 互动数（转发/点赞/评论/阅读）。"""
        lg = r.get("legacy", {}) or {}
        views = r.get("views") or r.get("ext_views") or {}

        def _i(v):
            try:
                return int(v)
            except (TypeError, ValueError):
                return 0

        return {
            "reposts": _i(lg.get("retweet_count")),
            "likes": _i(lg.get("favorite_count")),
            "replies": _i(lg.get("reply_count")),
            "views": _i(views.get("count")),
        }

    @staticmethod
    def _note_text(tweet: dict) -> str:
        """长推文（note_tweet）全文。"""
        note = (tweet.get("note_tweet") or {}).get("note_tweet_results", {})
        return ((note.get("result") or {}).get("text") or "").strip()

    @staticmethod
    def _media_list(tweet: dict) -> list:
        media = ((tweet.get("legacy", {}).get("extended_entities") or {})
                 .get("media") or [])
        out = []
        for m in media:
            mtype = m.get("type", "") or "photo"
            if mtype not in ("photo", "video", "animated_gif"):
                continue
            size = m.get("original_info", {}) or {}
            out.append({
                "url": m.get("media_url_https") or m.get("media_url", ""),
                "w": int(size.get("width") or 0),
                "h": int(size.get("height") or 0),
                "path": "",
                "type": "video" if mtype != "photo" else "photo",
            })
        return out

    # ------------------------------------------------------------------ #
    # 媒体下载
    # ------------------------------------------------------------------ #

    async def _download_media(self, item: dict) -> None:
        """下载推文图片到 media_dir，成功后把本地相对路径写回 item。

        引用块（quoted）的原帖媒体一并下载；纯转推时两者是同一列表对象，
        只下载一份。引用块文件名带 _q 前缀，避免与转发者自己的图片冲突。
        """
        jobs = [(m, "") for m in (item.get("media") or [])]
        qm = (item.get("quoted") or {}).get("media") or []
        if qm and qm is not item.get("media"):
            jobs += [(m, "q") for m in qm]
        if not jobs:
            return
        os.makedirs(self.media_dir, exist_ok=True)
        cli = self._dl_client()
        for i, (m, pre) in enumerate(jobs):
            # 文件名按 tweet id 稳定，已存在的直接复用，避免每次 force 拉取都重下
            base = os.path.join(self.media_dir, f"{item['id']}_{pre}{i}")
            cached = None
            for ext in (".jpg", ".png"):
                p = base + ext
                if os.path.exists(p) and os.path.getsize(p) > 0:
                    cached = p
                    break
            if cached:
                m["path"] = os.path.relpath(cached, self.media_dir)
                continue
            path = base + ".jpg"
            try:
                resp = await cli.get(m["url"])
                if resp.status_code != 200:
                    log.warning("媒体下载 %s -> HTTP %s", m["url"][:80],
                                resp.status_code)
                    continue
                ctype = (resp.headers.get("Content-Type") or "").lower()
                if "png" in ctype:
                    path = base + ".png"
                with open(path, "wb") as f:
                    f.write(resp.content)
                m["path"] = os.path.relpath(path, self.media_dir)
            except Exception as e:
                log.warning("媒体下载失败 %s: %s", m["url"][:80], e)
        self._update_media(item)

    async def _download_avatar(self, item: dict) -> None:
        """下载作者头像（按用户名缓存），成功后把相对路径写回 item。"""
        url = item.get("avatar") or ""
        if not url:
            return
        handle = item.get("author_handle") or "unknown"
        av_dir = os.path.join(self.media_dir, "avatars")
        os.makedirs(av_dir, exist_ok=True)
        ext = ".jpg"
        for e in (".png", ".webp"):
            if url.lower().endswith(e):
                ext = e
                break
        path = os.path.join(av_dir, f"{handle}{ext}")
        if os.path.exists(path) and os.path.getsize(path) > 0:
            item["avatar"] = os.path.relpath(path, self.media_dir)
            return
        try:
            resp = await self._dl_client().get(url)
            if resp.status_code != 200:
                log.warning("头像下载 %s -> HTTP %s", url[:80],
                            resp.status_code)
                return
            with open(path, "wb") as f:
                f.write(resp.content)
            item["avatar"] = os.path.relpath(path, self.media_dir)
        except Exception as e:
            log.warning("头像下载失败 %s: %s", url[:80], e)

    def _update_media(self, item: dict) -> None:
        self.store.update_media(
            item["id"], json.dumps(item["media"], ensure_ascii=False))
