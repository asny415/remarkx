#!/usr/bin/env python3
"""remarkx relay — 家中转站：抓 X 首页时间线 -> 缓存 -> 渲染 -> 提供给 reMarkable。

内存模型：不做本地持久化，进程内存里只保留"当前阅读内容"（本次刷新抓到
的批 + 顺着 cursor 向后续抓的更早批），API 与渲染全部直接读内存。设备回
首页且距上次抓取超过 poll_seconds 时刷新一次（single-flight，并发只等不
重复抓），内容整体重建；翻到书尾时按 cursor 续抓，内容往后追加。第 1 页
及以后永远直接读缓存，翻页不产生任何 X 请求。渲染结果按数据版本缓存并
预渲染下一页，翻页秒开。

用法:
  python3 relay.py login              # 首次：登录小号（交互式）
  python3 relay.py run                # 启动服务（按需抓取 + HTTP）
  python3 relay.py run --mock         # 用模拟数据跑（调试/演示用）
  python3 relay.py render out.png     # 把第 0 页渲染成 PNG（检查排版）
"""

import argparse
import asyncio
import io
import json
import logging
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from memstore import MemStore  # noqa: E402
from fetcher import Fetcher, XError, SessionError  # noqa: E402
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
        "poll_seconds": 300,    # 缓存有效期（秒）：仅设备请求首页且过期才抓一次
        "poll_count": 30,       # 每次拉多少条
        "page_size": 12,        # 每页候选条数（实际按版面裁剪）
        "title": "X · Following",
        "translate": "",        # 机翻目标语言；留空禁用（免费后端不稳定，暂默认关）
        "data_dir": os.path.join(BASE, "data"),
        "font": "",             # 留空=自动找中文字体
        "browser": "brave",     # 会话失效时自动导入 Cookie 的浏览器（可逗号分隔多个）
        "mock": False,
    }
    if os.path.exists(path):
        with open(path) as f:
            user = json.load(f)
        cfg.update(user)
    cfg["data_dir"] = os.path.abspath(cfg["data_dir"])
    cfg["media_dir"] = os.path.join(cfg["data_dir"], "media")
    cfg["session_file"] = os.path.join(cfg["data_dir"], "session.json")
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


def seed_mock(store: MemStore, media_dir: str, n: int = 14) -> None:
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
    # 按新→旧顺序插入：展示顺序 = 插入顺序，第 0 页是最新一条
    for i, (name, handle, text, media, is_rt, mins) in enumerate(
            texts[:n]):
        dt = now - timedelta(minutes=mins * 7 + i)
        store.upsert_tweet({
            "id": str(1900000000000000000 + i),
            "created_at": dt.strftime("%a %b %d %H:%M:%S %z %Y"),
            "author_name": name,
            "author_handle": handle,
            "text": text,
            "is_retweet": is_rt,
            "rt_handle": "someone" if is_rt else "",
            "media": [{"url": "", "w": 0, "h": 0, "path": p} for p in media],
            "url": "",
        })
    log.info("mock 数据已写入 %d 条", n)


# ---------------------------------------------------------------------- #
# HTTP 服务
# ---------------------------------------------------------------------- #


def build_app(cfg: dict, store: MemStore, renderer: Renderer, fetcher=None):
    from aiohttp import web

    # 每个 app 一份运行时状态：抓取锁 / 抓取中标志 / 渲染缓存 / 运行元信息
    # （全部内存态，进程重启即清空）
    state = {
        "lock": asyncio.Lock(),
        "fetching": False,
        "cache": {},        # page -> (version, png bytes)
        "inflight": set(),  # 正在预渲染的页码
        "layout": None,
        "layout_v": None,
        # 渲染纪元：每次启动重新生成，进入版本号 -> 重启后设备/本端
        # 的旧页缓存全部失配，强制按新版式重下
        "epoch": str(int(time.time())),
        "gen": 0,           # 内容代数：每次重建/追加成功后 +1，进版本号
        "started": time.strftime("%Y-%m-%d %H:%M:%S"),
        "last_poll": "",
        "last_poll_ok": "",
        "last_error": "",
        "last_extend": "",
    }

    def data_version() -> str:
        return (f"{state['epoch']}:{state['gen']}:"
                f"{store.count()}:{store.latest().get('id', '')}")

    def is_stale() -> bool:
        """无内容，或距上次抓取超过 poll_seconds。"""
        if store.count() == 0:
            return True
        m = state["last_poll"]
        if not m:
            return True
        try:
            t = time.mktime(time.strptime(m, "%Y-%m-%d %H:%M:%S"))
        except ValueError:
            return True
        return time.time() - t > int(cfg["poll_seconds"])

    async def poll_once() -> tuple:
        """抓一次 X；会话失效时自动从浏览器导入 Cookie 重试一次。"""
        try:
            return await fetcher.poll()
        except SessionError as e:
            log.warning("会话失效，尝试从浏览器 [%s] 自动恢复 ...",
                        cfg.get("browser"))
            state["last_error"] = "会话失效，正在自动恢复…"
            ok = await asyncio.to_thread(fetcher.refresh_session)
            if not ok:
                raise XError("会话失效，且浏览器 Cookie 导入失败"
                             "（浏览器未安装/未登录 X？）。"
                             "请在浏览器登录 x.com 后刷新首页重试") from e
            return await fetcher.poll()

    def get_layout():
        version = data_version()
        if state.get("layout_v") != version:
            tweets = store.fetch_page(0, 10 ** 6)
            state["layout"] = renderer.paginate(tweets)
            state["layout_v"] = version
        return state["layout"]

    async def ensure_fresh(force: bool = False) -> None:
        """服务首页前确保数据新鲜：过期才抓（force=1 则强制抓）；已有抓取则等它。"""
        if fetcher is None:
            return
        if not force and not is_stale():
            return
        if state["lock"].locked():
            async with state["lock"]:
                pass  # 等进行中的抓取结束，下面会重新判断是否还过期
        async with state["lock"]:
            if not force and not is_stale():
                return
            state["fetching"] = True
            log.info("数据已过期，按需从 X 抓取 ...")
            try:
                batch, n = await poll_once()
                log.info("按需抓取完成，本次时间线 %d 条，新入库 %d 条",
                         len(batch), n)
                if batch:
                    # 刷新 = 重建阅读内容：整体替换为本次抓到的批
                    store.reset(batch)
                    state["gen"] += 1
                state["last_poll"] = time.strftime("%Y-%m-%d %H:%M:%S")
                state["last_poll_ok"] = "1"
                state["last_error"] = ""
            except XError as e:
                log.error("按需抓取失败: %s", e)
                state["last_error"] = str(e)
            except Exception as e:  # noqa: BLE001
                log.exception("按需抓取异常")
                state["last_error"] = repr(e)[:300]
            finally:
                state["fetching"] = False

    async def extend_older() -> None:
        """读者接近书尾时向 X 续抓更早内容（低频，让书永远翻不完）。"""
        if fetcher is None or state["fetching"]:
            return
        m = state["last_extend"]
        if m:
            try:
                t0 = time.mktime(time.strptime(m, "%Y-%m-%d %H:%M:%S"))
                if time.time() - t0 < 30:
                    return
            except ValueError:
                pass
        async with state["lock"]:
            if state["fetching"]:
                return
            state["fetching"] = True
            try:
                batch, n = await fetcher.poll_extend()
                log.info("续抓更早内容 %d 条", n)
                if batch and store.add_batch(batch):
                    state["gen"] += 1
                state["last_extend"] = time.strftime("%Y-%m-%d %H:%M:%S")
                state["last_poll"] = time.strftime("%Y-%m-%d %H:%M:%S")
            except XError as e:
                log.warning("续抓失败: %s", e)
            except Exception as e:  # noqa: BLE001
                log.exception("续抓异常")
            finally:
                state["fetching"] = False

    async def render_png(page: int, per: int) -> bytes:
        pages = get_layout()
        img = await asyncio.to_thread(renderer.render_page, pages, page)
        buf = io.BytesIO()
        img.save(buf, format="PNG", optimize=True)
        return buf.getvalue()

    async def prerender(page: int, per: int) -> None:
        """后台预渲染一页（渲染期间数据变了则丢弃结果）。"""
        version = data_version()
        state["inflight"].add(page)
        try:
            data = await render_png(page, per)
            if data_version() == version:
                if len(state["cache"]) > 32:
                    state["cache"].clear()
                state["cache"][page] = (version, data)
        except Exception:  # noqa: BLE001
            log.exception("预渲染第 %d 页失败（不影响主流程）", page)
        finally:
            state["inflight"].discard(page)

    async def h_page(request: web.Request) -> web.Response:
        try:
            p = int(request.query.get("p", "0"))
        except ValueError:
            p = 0
        p = max(0, p)
        force = request.query.get("force", "") == "1"
        if p == 0 or store.count() == 0:
            await ensure_fresh(force=force)
        pages = get_layout()
        total_pages = len(pages)
        if p > total_pages - 1:
            p = max(0, total_pages - 1)
        # 读到接近书尾时，异步续抓更早内容
        if fetcher is not None and total_pages > 0 and p >= total_pages - 2:
            asyncio.create_task(extend_older())
        version = data_version()
        hit = state["cache"].get(p)
        if hit and hit[0] == version:
            data = hit[1]
        else:
            data = await render_png(p, int(cfg["page_size"]))
            if len(state["cache"]) > 32:
                state["cache"].clear()
            state["cache"][p] = (version, data)
        # 预渲染下一页：翻页秒开
        nxt = p + 1
        nhit = state["cache"].get(nxt)
        if (nxt < total_pages
                and not (nhit and nhit[0] == version)
                and nxt not in state["inflight"]):
            asyncio.create_task(prerender(nxt, int(cfg["page_size"])))
        return web.Response(body=data, content_type="image/png",
                            headers={"Cache-Control": "no-store"})

    async def h_status(request: web.Request) -> web.Response:
        s = {
            "count": store.count(),
            "latest": store.latest(),
            "last_poll": state["last_poll"],
            "last_poll_ok": state["last_poll_ok"],
            "last_error": state["last_error"],
            "started": state["started"],
            "now": time.strftime("%Y-%m-%d %H:%M:%S"),
        }
        s["fetching"] = state["fetching"]
        s["stale"] = fetcher is not None and is_stale()
        s["pages"] = len(get_layout())
        s["version"] = data_version()
        return web.json_response(s)

    async def h_layout(request: web.Request) -> web.Response:
        try:
            p = int(request.query.get("p", "0"))
        except ValueError:
            p = 0
        pages = get_layout()
        total = len(pages)
        p = min(max(p, 0), max(0, total - 1))
        return web.json_response({
            "pages": total,
            "p": p,
            "cards": renderer.card_rects(pages)[p] if total else [],
        })

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
        window_pages = len(get_layout())
        return web.Response(
            content_type="text/html",
            text=f"""<!doctype html><meta charset=utf-8>
<h2>remarkx relay</h2>
<pre>
count         {store.count()}
window pages {window_pages}
latest id     {store.latest().get('id', '-')}
last poll     {state['last_poll'] or '-'}
last error    {state['last_error'] or '-'}
time          {time.strftime('%Y-%m-%d %H:%M:%S')}
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
    app.router.add_get("/api/layout", h_layout)
    app.router.add_get("/media/{path:.+}", h_media)
    app.router.add_get("/healthz", h_health)
    return app


# ---------------------------------------------------------------------- #
# 入口
# ---------------------------------------------------------------------- #

def cmd_login(args) -> None:
    logging.basicConfig(level=logging.INFO)
    cfg = load_config(args.config)
    fetcher = Fetcher(cfg, MemStore())
    asyncio.run(fetcher.login(
        username=args.user, email=args.email, password=args.password))


def cmd_run(args) -> None:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    cfg = load_config(args.config)
    if args.mock:
        cfg["mock"] = True
    store = MemStore()
    renderer = Renderer(cfg["media_dir"], cfg["title"], cfg.get("font", ""), prefer_zh=bool(cfg.get("translate", "")))

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
            # 首次启动且无会话时，先尝试从浏览器自动导入 Cookie（免输密码）
            if not os.path.exists(cfg["session_file"]):
                log.info("无会话文件，尝试从浏览器 [%s] 自动导入 Cookie ...",
                         cfg.get("browser"))
                if await asyncio.to_thread(fetcher.refresh_session):
                    log.info("浏览器 Cookie 自动导入成功")
                else:
                    log.warning("浏览器 Cookie 自动导入失败。可运行 "
                                "'python3 relay.py login' 手动登录，"
                                "或在浏览器登录 x.com 后在阅读器刷新首页重试")
            app = build_app(cfg, store, renderer, fetcher)
            runner = web.AppRunner(app)
            await runner.setup()
            site = web.TCPSite(runner, cfg["bind"], cfg["port"])
            await site.start()
            log.info("服务启动: http://%s:%s（按需抓取：设备请求首页且数据"
                     "超过 %ss 未更新时才抓一次 X）",
                     cfg["bind"], cfg["port"], cfg["poll_seconds"])
            await asyncio.Event().wait()

        asyncio.run(run_real())


def cmd_render(args) -> None:
    cfg = load_config(args.config)
    store = MemStore()
    renderer = Renderer(cfg["media_dir"], cfg["title"], cfg.get("font", ""), prefer_zh=bool(cfg.get("translate", "")))
    if store.count() == 0:
        seed_mock(store, cfg["media_dir"])
    tweets = store.fetch_page(0, 10 ** 6)
    pages = renderer.paginate(tweets)
    img = renderer.render_page(pages, 0)
    img.save(args.out)
    log.info("已渲染 %s (%dx%d)，共 %d 页", args.out, W, H, len(pages))


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

    args = ap.parse_args()
    if not getattr(args, "cmd", None):
        ap.print_help()
        sys.exit(1)
    args.fn(args)


if __name__ == "__main__":
    main()
