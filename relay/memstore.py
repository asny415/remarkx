"""内存 feed：当前阅读内容的唯一事实来源，进程重启即清空。

- 刷新（poll）：reset() 用本次抓到的批整体替换；
- 续抓（extend）：add_batch() 把更早的批往后追加；
- 展示顺序 = 插入顺序（X 时间线页序，续抓批次只往后接）。
  已展示过的内容位置永不改变，续抓不会引起版面重排。

接口形状与渲染器 / HTTP 接口的既有约定保持一致。
"""

import json


def _norm(t: dict) -> dict:
    """抓取条目 -> 渲染/接口用的规范字典（与原 fetch_page 输出同形）。"""
    return {
        "id": str(t["id"]),
        "created_at": t.get("created_at", ""),
        "author_name": t.get("author_name", ""),
        "author_handle": t.get("author_handle", ""),
        "text": t.get("text", ""),
        "is_retweet": bool(t.get("is_retweet")),
        "rt_handle": t.get("rt_handle", ""),
        "comment": t.get("comment", ""),
        "quoted": t.get("quoted"),
        "stats": t.get("stats") or {},
        "media": list(t.get("media", []) or []),
        "url": t.get("url", ""),
        "translated": t.get("translated", ""),
        "avatar": t.get("avatar", ""),
    }


class MemStore:
    def __init__(self):
        self._tweets: dict = {}   # id -> 规范条目

    # ---------- 写入 ----------

    def upsert_tweet(self, t: dict) -> bool:
        """按 id 去重插入，返回是否为新条目。"""
        tid = str(t["id"])
        if tid in self._tweets:
            return False
        self._tweets[tid] = _norm(t)
        return True

    def reset(self, items: list) -> int:
        """清空并用一批条目重建（刷新语义），返回实际入库条数。"""
        self._tweets = {}
        return self.add_batch(items)

    def add_batch(self, items: list) -> int:
        """追加一批条目（续抓语义），返回实际新增条数。"""
        n = 0
        for t in items or []:
            if self.upsert_tweet(t):
                n += 1
        return n

    def update_translation(self, tweet_id: str, translated: str) -> None:
        d = self._tweets.get(str(tweet_id))
        if d is not None:
            d["translated"] = translated

    def update_media(self, tweet_id: str, media_json: str) -> None:
        d = self._tweets.get(str(tweet_id))
        if d is not None:
            d["media"] = json.loads(media_json or "[]")

    # ---------- 读取 ----------

    def _ordered(self) -> list:
        # dict 保持插入序：刷新批次在前，续抓批次依次往后接
        return list(self._tweets.values())

    def fetch_page(self, page: int, per_page: int) -> list:
        """第 page 页（0 = 最新），每页 per_page 条，按插入顺序。"""
        page = max(0, page)
        per_page = max(1, per_page)
        lo = page * per_page
        return self._ordered()[lo:lo + per_page]

    def latest(self) -> dict:
        o = self._ordered()
        if not o:
            return {}
        return {"id": o[0]["id"], "created_at": o[0]["created_at"]}

    def count(self) -> int:
        return len(self._tweets)
