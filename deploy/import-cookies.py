#!/usr/bin/env python3
"""从本机浏览器导入 X 会话 Cookie（与 yt-dlp --cookies-from-browser 同一机制）。

前提：浏览器里已登录 x.com。提取到的 Cookie 写成 {cookie名: 值} 的 JSON，
设备端 xr 直接读取使用，无需再输密码。

用法：
  python3 import-cookies.py brave /tmp/cookies.json
  python3 import-cookies.py brave,chromium /tmp/cookies.json   # 逗号分隔多个，任一成功即可
"""

import json
import logging
import sys

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
log = logging.getLogger("import-cookies")

X_DOMAINS = ("x.com", "twitter.com")

# v11 Cookie（Chrome 127+）需要新 yt-dlp（2024-11 起支持 AES-GCM 解密）
_MIN_YTDLP = (2024, 11)


def _ytdlp_version() -> tuple:
    try:
        from yt_dlp import version as v
    except ImportError:
        log.error("缺少 yt-dlp：请在项目 venv 安装 'pip install -U yt-dlp'")
        raise SystemExit(1)
    try:
        parts = v.__version__.split(".")
        return tuple(int(p) for p in parts[:2])
    except Exception:
        return (0, 0)


def _check_ytdlp() -> None:
    ver = _ytdlp_version()
    if ver < _MIN_YTDLP:
        log.error("yt-dlp 版本过旧（%s），无法解密新版 v11 Cookie。"
                  "请升级：'pip install -U yt-dlp'（或使用项目 .venv）",
                  ".".join(str(x) for x in ver))
        raise SystemExit(1)


def _is_x_domain(domain: str) -> bool:
    d = (domain or "").lstrip(".").lower()
    return any(d == s or d.endswith("." + s) for s in X_DOMAINS)


def export_from_browser(browser: str, dest: str) -> bool:
    _check_ytdlp()
    try:
        from yt_dlp.cookies import extract_cookies_from_browser
    except ImportError:
        log.error("缺少 yt-dlp：请在项目 venv 安装 'pip install -U yt-dlp'")
        return False
    try:
        jar = extract_cookies_from_browser(browser)
    except Exception as e:  # noqa: BLE001
        log.warning("无法读取浏览器 [%s] 的 Cookie：%s", browser, e)
        return False

    cookies = {}
    for c in jar:
        if _is_x_domain(c.domain):
            cookies[c.name] = c.value

    if "auth_token" not in cookies:
        log.warning("浏览器 [%s] 里没有找到 X 登录态（auth_token）——"
                    "请先在该浏览器登录 x.com", browser)
        return False

    tmp = dest + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cookies, f, ensure_ascii=False)
    import os
    os.replace(tmp, dest)
    log.info("已从浏览器 [%s] 导入 %d 个 X Cookie -> %s",
             browser, len(cookies), dest)
    return True


def try_import(browsers: list, dest: str) -> bool:
    for b in browsers:
        if export_from_browser(b, dest):
            return True
    return False


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    browsers = [b.strip() for b in sys.argv[1].split(",") if b.strip()]
    dest = sys.argv[2]
    if not try_import(browsers, dest):
        sys.exit(1)


if __name__ == "__main__":
    main()
