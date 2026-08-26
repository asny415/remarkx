#!/usr/bin/env python3
"""remarkx relay — 家中转站：抓 X 首页时间线 -> 缓存 -> 渲染 -> 提供给 reMarkable。

用法:
  python3 relay.py login              # 首次：登录小号（交互式）
  python3 relay.py run                # 启动服务（抓取轮询 + HTTP）
  python3 relay.py run --mock         # 用模拟数据跑（调试/演示用）
  python3 relay.py render out.png     # 把第 0 页渲染成 PNG（检查排版）
  python3 relay.py mockseed           # 往库里塞一批模拟数据
"""

import argparse
import asyncio
import json
import logging
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from store import Store  # noqa: E402
from fetcher import Fetcher, XError  # noqa: E402
from render import Renderer, W, H  # noqa: E402

log = logging.getLogger("remarkx")

BASE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------- #
# 配置
# ---------------------------------------------------------------------- #

def load_config(path: str) -> dict:
    cfg = {
        "bind": "0.0.0.0",
        "port": 8788,
        "proxy": "",            # 访问 X 用的代理，如 http://127.0.0.1:7890
        "poll_seconds": 300,    # 轮询间隔
        "poll_count": 30,       # 每次拉多少条
        "page_size": 12,        # 每页候选条数（实际按版面裁剪）
        "title": "X · Following",
        "data_dir": os.path.join(BASE, "data"),
        "font": "",             # 留空=自动找中文字体
        "mock": False,
    }
    if os.path.exists(path):
        with open(path) as f:
            user = json.load(f)
        cfg.update(user)
    cfg["data_dir"] = os.path.abspath(cfg["data_dir"])
    cfg["media_dir"] = os.path.join(cfg["data_dir"], "media")
    cfg["session_file"] = os.path.join(cfg["data_dir"], "session.json")
    cfg["db_file"] = os.path.join(cfg["data_dir"], "tweets.db")
    os.makedirs(cfg["media_dir"], exist_ok=True)
    return cfg


# ---------------------------------------------------------------------- #
# mock 数据
# ---------------------------------------------------------------------- #

def _mock_img(media_dir: str, name: str, w: int, h: int, c1, c2) -> str:
    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = tuple(int(c1[i] + (c2[i] - c1[i]) * (x + y) / (w + h))
                             for i in range(3))
    p = os.path.join(media_dir, name)
    img.save(p)
    return os.path.basename(p)


def seed_mock(store: Store, media_dir: str, n: int = 14) -> None:
    """塞入一批模拟推文（含中文、RT、图片），供无账号时调试。"""
    from datetime import datetime, timedelta, timezone

    os.makedirs(media_dir, exist_ok=True)
    now = datetime.now(timezone.utc)
    imgs = [
        _mock_img(media_dir, "mock_0.jpg", 1600, 1000, (60, 90, 160), (200, 220, 240)),
        _mock_img(media_dir, "mock_1.jpg", 900, 900, (160, 120, 60), (240, 220, 200)),
        _mock_img(media_dir, "mock_2.jpg", 1200, 800, (90, 150, 90), (220, 240, 220)),
        _mock_img(media_dir, "mock_3.jpg", 1600, 1000, (150, 70, 90), (240, 210, 220)),
    ]
    texts = [
        ("Kubernetes", "kubernetes",
         "KubeCon 2026 开幕：今年的主题是「把 AI 工作负载跑在边缘」。"
         "集群自愈、GPU 调度、eBPF 可观测性是最热门的三个议题。"
         "现场演示了 5000 节点集群在 30 秒内完成滚动升级，"
         "控制面用了新的流式 etcd 架构，P99 延迟降到 8ms 以内。",
         [imgs[0]], False, 0),
        ("林间鹿", "linjianlu",
         "今天试了用 reMarkable 看 X 的新流程：电脑中转渲染成图片，"
         "墨水屏上刷文本+图片，完全没有视频和动图，眼睛舒服多了。"
         "翻页有一点延迟但完全能接受，比盯着手机强太多。",
         [imgs[1]], False, 1),
        ("OpenAI", "openai",
         "Introducing a new approach to on-device reasoning for "
         "resource-constrained hardware. Early benchmarks show 4x "
         "improvement in tokens/sec on edge devices while preserving "
         "quality on long-context tasks.",
         [imgs[2]], False, 2),
        ("系统管理员老周", "ops_laozhou",
         "RT 提醒：周末把家里 NAS 从群晖换成了 TrueNAS Scale，"
         "踩坑记录：ZFS 的 ARC 缓存和 Docker 的 cgroup 限制要重新调，"
         "不然内存占用会飙到 70%。另外 UPS 的 NUT 服务记得改 socket 路径。",
         [], True, 3),
        ("Rust 中文社区", "rust_zh",
         "RFC 3584 讨论总结：unsafe_cell 的语义在 async 场景下的行为"
         "最终没有达成一致，将推迟到 1.90 再议。社区建议先用 "
         "std::sync::atomic 的扩展 API 过渡。",
         [imgs[3]], False, 4),
        ("咖啡与代码", "code_coffee",
         "下午三点，手冲耶加雪菲，豆子是水洗处理的，"
         "花香很突出。代码写不动的时候就适合停下来喝一杯。",
         [], False, 5),
        ("NASA", "NASA",
         "Webb has completed its latest observation of the Carina Nebula, "
         "revealing previously hidden protostellar jets in infrared. "
         "Full image release this Friday.",
         [imgs[0], imgs[1]], False, 6),
        ("徒步老王", "hiking_wang",
         "周末爬了武功山，从沈子村到发云界全程 6 公里，"
         "草甸在雨后特别绿。提醒：山顶风大，体感比山下低 8 度左右，"
         "带件冲锋衣。",
         [imgs[2], imgs[3]], False, 7),
        ("科技日报", "techdaily",
         "某开源项目发布 2.0 版本：全新的插件系统、"
         "性能提升 3 倍、支持中文本地化。",
         [], True, 8),
        ("读书记录", "reading_log",
         "《置身事内》读完。地方政府在经济增长中的角色比想象中复杂，"
         "土地财政、城投债、招商引资，一条线串起来很清楚。"
         "推荐给所有关心宏观经济的朋友。",
         [], False, 9),
        ("Linux 内核周报", "kernel_weekly",
         "本周内核 6.18-rc 合并窗口摘要：VFS 层引入新的 IO 调度器，"
         "网络子树合并了 MPTCP 的改进，ARM64 平台新增对 "
         "Neoverse V3 的支持。",
         [], False, 10),
        ("摄影小赵", "photo_zhao",
         "用理光 GR3 扫街的一天。高感光度在夜晚的霓虹下依然很扎实，"
         "35mm 等效焦段在狭窄的巷子里非常舒服。",
         [imgs[1], imgs[2], imgs[3], imgs[0]], False, 11),
        ("气象站", "weather_station",
         "明天有小雨，气温 18~24℃，东南风 3 级。"
         "周末转晴，适合出行。",
         [], False, 12),
        ("开源翻译组", "oss_translate",
         "我们完成了 42 篇文档的翻译，覆盖数据库、网络和分布式系统。"
         "欢迎加入，GitHub 上认领 issue 即可。",
         [], False, 13),
    ]
    for i, (name, handle, text, media, is_rt, mins) in enumerate(texts[:n]):
        dt = now - timedelta(minutes=mins * 7 + i)
        store.upsert_tweet({
            "id": str(1900000000000000000 + (len(texts[:n]) - 1 - i)),
            "created_at": dt.strftime("%a %b %d %H:%M:%S %z %Y"),
            "author_name": name,
            "author_handle": handle,
            "text": text,
            "is_retweet": is_rt,
            "rt_handle": "someone" if is_rt else "",
            "media": [{"url": "", "w": 0, "h": 0, "path": p} for p in media],
            "url": "",
        })
    store.set_meta("mock", "1")
    store.set_meta("last_poll", time.strftime("%Y-%m-%d %H:%M:%S"))
    store.set_meta("last_poll_ok", "1")
    log.info("mock 数据已写入 %d 条", n)


# ---------------------------------------------------------------------- #
# HTTP 服务
# ---------------------------------------------------------------------- #

def build_app(cfg: dict, store: Store, renderer: Renderer):
    from aiohttp import web

    async def h_page(request: web.Request) -> web.Response:
        try:
            p = int(request.query.get("p", "0"))
        except ValueError:
            p = 0
        p = max(0, p)
        per = int(cfg["page_size"])
        total_pages = max(1, -(-store.count() // per))
        if p > total_pages:
            p = total_pages - 1
        tweets = store.fetch_page(p, per)
        status = store.status()
        img = renderer.render_page(tweets, p, total_pages, status)
        import io
        buf = io.BytesIO()
        img.save(buf, format="PNG", optimize=True)
        data = buf.getvalue()
        return web.Response(body=data, content_type="image/png",
                            headers={"Cache-Control": "no-store"})

    async def h_status(request: web.Request) -> web.Response:
        return web.json_response(store.status())

    async def h_feed(request: web.Request) -> web.Response:
        try:
            p = int(request.query.get("p", "0"))
            n = int(request.query.get("n", "20"))
        except ValueError:
            p, n = 0, 20
        return web.json_response({"tweets": store.fetch_page(p, n)})

    async def h_media(request: web.Request) -> web.Response:
        from aiohttp.web import HTTPNotFound
        name = os.path.basename(request.match_info["path"])
        full = os.path.abspath(os.path.join(cfg["media_dir"], name))
        if not full.startswith(os.path.abspath(cfg["media_dir"])) \
                or not os.path.isfile(full):
            raise HTTPNotFound()
        return web.FileResponse(full)

    async def h_index(request: web.Request) -> web.Response:
        s = store.status()
        return web.Response(
            content_type="text/html",
            text=f"""<!doctype html><meta charset=utf-8>
<h2>remarkx relay</h2>
<pre>
count       {s['count']}
latest id   {s['latest'].get('id', '-')}
last poll   {s['last_poll'] or '-'}
last error  {s['last_error'] or '-'}
time        {s['now']}
</pre>
<p><a href="/page?p=0">第 0 页 PNG</a> · <a href="/api/status">status JSON</a></p>
""")

    async def h_health(request: web.Request) -> web.Response:
        return web.Response(text="ok")

    app = web.Application()
    app.router.add_get("/", h_index)
    app.router.add_get("/page", h_page)
    app.router.add_get("/api/status", h_status)
    app.router.add_get("/api/feed", h_feed)
    app.router.add_get("/media/{path:.+}", h_media)
    app.router.add_get("/healthz", h_health)
    return app


# ---------------------------------------------------------------------- #
# 轮询
# ---------------------------------------------------------------------- #

async def poll_loop(cfg: dict, store: Store, fetcher: Fetcher):
    while True:
        started = time.monotonic()
        try:
            n = await fetcher.poll()
            store.set_meta("last_poll", time.strftime("%Y-%m-%d %H:%M:%S"))
            store.set_meta("last_poll_ok", "1")
            store.set_meta("last_error", "")
            log.info("轮询完成，新增 %d 条（耗时 %.1fs）", n,
                     time.monotonic() - started)
            delay = cfg["poll_seconds"]
        except XError as e:
            store.set_meta("last_error", str(e))
            log.error("抓取错误（15 分钟后重试）: %s", e)
            delay = max(cfg["poll_seconds"], 900)
        except Exception as e:  # noqa: BLE001
            store.set_meta("last_error", repr(e)[:300])
            log.exception("轮询异常（15 分钟后重试）")
            delay = max(cfg["poll_seconds"], 900)
        await asyncio.sleep(delay)


# ---------------------------------------------------------------------- #
# 入口
# ---------------------------------------------------------------------- #

def cmd_login(args) -> None:
    logging.basicConfig(level=logging.INFO)
    cfg = load_config(args.config)
    fetcher = Fetcher(cfg, Store(cfg["db_file"]))
    asyncio.run(fetcher.login(
        username=args.user, email=args.email, password=args.password))


def cmd_run(args) -> None:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    cfg = load_config(args.config)
    if args.mock:
        cfg["mock"] = True
    store = Store(cfg["db_file"])
    store.set_meta("started", time.strftime("%Y-%m-%d %H:%M:%S"))
    renderer = Renderer(cfg["media_dir"], cfg["title"], cfg.get("font", ""))

    if cfg["mock"]:
        if store.count() == 0:
            seed_mock(store, cfg["media_dir"])

        async def run_mock():
            from aiohttp import web
            app = build_app(cfg, store, renderer)
            runner = web.AppRunner(app)
            await runner.setup()
            site = web.TCPSite(runner, cfg["bind"], cfg["port"])
            await site.start()
            log.info("mock 模式: http://%s:%s", cfg["bind"], cfg["port"])
            await asyncio.Event().wait()

        asyncio.run(run_mock())
    else:
        fetcher = Fetcher(cfg, store)

        async def run_real():
            from aiohttp import web
            app = build_app(cfg, store, renderer)
            runner = web.AppRunner(app)
            await runner.setup()
            site = web.TCPSite(runner, cfg["bind"], cfg["port"])
            await site.start()
            log.info("服务启动: http://%s:%s", cfg["bind"], cfg["port"])
            await poll_loop(cfg, store, fetcher)

        asyncio.run(run_real())


def cmd_render(args) -> None:
    cfg = load_config(args.config)
    store = Store(cfg["db_file"])
    renderer = Renderer(cfg["media_dir"], cfg["title"], cfg.get("font", ""))
    if store.count() == 0:
        seed_mock(store, cfg["media_dir"])
    tweets = store.fetch_page(0, int(cfg["page_size"]))
    status = store.status()
    img = renderer.render_page(tweets, 0, 1, status)
    img.save(args.out)
    log.info("已渲染 %s (%dx%d)", args.out, W, H)


def cmd_mockseed(args) -> None:
    logging.basicConfig(level=logging.INFO)
    cfg = load_config(args.config)
    store = Store(cfg["db_file"])
    seed_mock(store, cfg["media_dir"])


def main() -> None:
    ap = argparse.ArgumentParser(description="remarkx relay")
    ap.add_argument("--config", default=os.path.join(BASE, "config.json"))
    sub = ap.add_subparsers(dest="cmd")

    p_login = sub.add_parser("login", help="登录小号（交互式）")
    p_login.add_argument("--user")
    p_login.add_argument("--email")
    p_login.add_argument("--password")
    p_login.set_defaults(fn=cmd_login)

    p_run = sub.add_parser("run", help="启动服务")
    p_run.add_argument("--mock", action="store_true",
                       help="模拟数据模式（不连 X）")
    p_run.set_defaults(fn=cmd_run)

    p_render = sub.add_parser("render", help="渲染第 0 页到 PNG")
    p_render.add_argument("out", nargs="?", default="page0.png")
    p_render.set_defaults(fn=cmd_render)

    p_seed = sub.add_parser("mockseed", help="写入模拟数据")
    p_seed.set_defaults(fn=cmd_mockseed)

    args = ap.parse_args()
    if not getattr(args, "cmd", None):
        ap.print_help()
        sys.exit(1)
    args.fn(args)


if __name__ == "__main__":
    main()
