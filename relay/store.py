"""SQLite 存储层：推文 + 元信息。

所有方法都是同步的（sqlite 操作很快），在 asyncio 里直接调用即可。
"""

import json
import os
import sqlite3
import threading
import time

_SCHEMA = """
CREATE TABLE IF NOT EXISTS tweets (
    id              TEXT PRIMARY KEY,
    created_at      TEXT NOT NULL,
    author_name     TEXT NOT NULL DEFAULT '',
    author_handle   TEXT NOT NULL DEFAULT '',
    text            TEXT NOT NULL DEFAULT '',
    is_retweet      INTEGER NOT NULL DEFAULT 0,
    rt_handle       TEXT NOT NULL DEFAULT '',
    media           TEXT NOT NULL DEFAULT '[]',
    url             TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_tweets_id ON tweets (CAST(id AS INTEGER) DESC);
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""

# 旧库平滑迁移：tweets 增加扩展列（已存在则忽略）
_MIGRATIONS = [
    "ALTER TABLE tweets ADD COLUMN translated TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE tweets ADD COLUMN avatar TEXT NOT NULL DEFAULT ''",
    # 抓取序号：每次新入库递增；渲染按序号倒序（越新抓的越靠前）
    "ALTER TABLE tweets ADD COLUMN seq INTEGER NOT NULL DEFAULT 0",
]


class Store:
    def __init__(self, path: str):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.path = path
        self._lock = threading.Lock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.executescript(_SCHEMA)
        for ddl in _MIGRATIONS:
            try:
                self._db.execute(ddl)
            except sqlite3.OperationalError:
                pass                    # 列已存在
        # 回填抓取序号：旧数据按推文 id 顺序赋值（新入库的序号会超过它们）
        self._db.execute(
            "UPDATE tweets SET seq = "
            " (SELECT COUNT(*) FROM tweets t2 "
            "  WHERE CAST(t2.id AS INTEGER) <= CAST(tweets.id AS INTEGER)) "
            "WHERE seq = 0")
        self._db.commit()
        row = self._db.execute("SELECT MAX(seq) FROM tweets").fetchone()
        saved = self.get_meta("seq_counter")
        self._seq = max(int(row[0] or 0),
                        int(saved) if saved.isdigit() else 0)

    # ---------- meta ----------

    def set_meta(self, key: str, value: str) -> None:
        with self._lock:
            self._db.execute(
                "INSERT INTO meta(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )
            self._db.commit()

    def get_meta(self, key: str) -> str:
        with self._lock:
            row = self._db.execute(
                "SELECT value FROM meta WHERE key=?", (key,)
            ).fetchone()
        return row[0] if row else ""

    def meta_dict(self) -> dict:
        with self._lock:
            rows = self._db.execute("SELECT key, value FROM meta").fetchall()
        return dict(rows)

    # ---------- tweets ----------

    def upsert_tweet(self, t: dict) -> bool:
        """插入一条推文。已存在则忽略，返回是否为新推文。

        新推文按抓取顺序分配递增 seq，渲染按 seq 倒序（越新抓的越靠前）。
        """
        with self._lock:
            seq = self._seq + 1
            cur = self._db.execute(
                "INSERT OR IGNORE INTO tweets "
                "(id, created_at, author_name, author_handle, text, "
                " is_retweet, rt_handle, media, url, avatar, seq) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                (
                    str(t["id"]),
                    t.get("created_at", ""),
                    t.get("author_name", ""),
                    t.get("author_handle", ""),
                    t.get("text", ""),
                    1 if t.get("is_retweet") else 0,
                    t.get("rt_handle", ""),
                    json.dumps(t.get("media", []), ensure_ascii=False),
                    t.get("url", ""),
                    t.get("avatar", ""),
                    seq,
                ),
            )
            if cur.rowcount > 0:
                self._seq = seq
                self._db.execute(
                    "INSERT INTO meta(key, value) VALUES('seq_counter', ?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                    (str(seq),),
                )
                self._db.commit()
                return True
            self._db.commit()
            return False

    def update_translation(self, tweet_id: str, translated: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE tweets SET translated=? WHERE id=?",
                (translated, str(tweet_id)),
            )
            self._db.commit()

    def update_media(self, tweet_id: str, media_json: str) -> None:
        with self._lock:
            self._db.execute(
                "UPDATE tweets SET media=? WHERE id=?",
                (media_json, str(tweet_id)),
            )
            self._db.commit()

    def tweet_ids(self) -> set:
        with self._lock:
            rows = self._db.execute("SELECT id FROM tweets").fetchall()
        return {r[0] for r in rows}

    def count(self) -> int:
        with self._lock:
            row = self._db.execute("SELECT COUNT(*) FROM tweets").fetchone()
        return row[0]

    def pages(self, page_size: int) -> int:
        """按条数粗略估算页数（渲染时实际按版面裁剪）。"""
        n = self.count()
        return max(1, -(-n // page_size))

    def fetch_page(self, page: int, per_page: int) -> list:
        """第 page 页（0 = 最新），每页 per_page 条，按时间倒序。"""
        page = max(0, page)
        per_page = max(1, per_page)
        lo = page * per_page  # page 0 = 最新的 per_page 条
        with self._lock:
            rows = self._db.execute(
                "SELECT id, created_at, author_name, author_handle, text, "
                "       is_retweet, rt_handle, media, url, translated, avatar "
                "FROM tweets ORDER BY seq DESC, CAST(id AS INTEGER) DESC "
                "LIMIT ? OFFSET ?",
                (per_page, lo),
            ).fetchall()
        out = []
        for r in rows:
            d = {
                "id": r[0],
                "created_at": r[1],
                "author_name": r[2],
                "author_handle": r[3],
                "text": r[4],
                "is_retweet": bool(r[5]),
                "rt_handle": r[6],
                "media": json.loads(r[7] or "[]"),
                "url": r[8],
                "translated": r[9] or "",
                "avatar": r[10] or "",
            }
            out.append(d)
        return out

    def latest(self) -> dict:
        """最新入库（最近一次抓到的）推文，而非发布时间最新的。"""
        with self._lock:
            row = self._db.execute(
                "SELECT id, created_at FROM tweets "
                "ORDER BY seq DESC, CAST(id AS INTEGER) DESC LIMIT 1"
            ).fetchone()
        if not row:
            return {}
        return {"id": row[0], "created_at": row[1]}

    def status(self) -> dict:
        m = self.meta_dict()
        return {
            "count": self.count(),
            "latest": self.latest(),
            "last_poll": m.get("last_poll", ""),
            "last_poll_ok": m.get("last_poll_ok", ""),
            "last_error": m.get("last_error", ""),
            "started": m.get("started", ""),
            "now": time.strftime("%Y-%m-%d %H:%M:%S"),
        }
