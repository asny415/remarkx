"""从本机浏览器导入 X 会话 Cookie（与 yt-dlp --cookies-from-browser 同一机制）。

前提：浏览器里已登录 x.com。提取到的 Cookie 写成 twikit 的 JSON 格式
（{cookie名: 值}），relay 后续直接复用，无需再输密码。
"""

import json
import logging
import os

log = logging.getLogger("remarkx.cookies")

X_DOMAINS = ("x.com", "twitter.com")


def _is_x_domain(domain: str) -> bool:
    d = (domain or "").lstrip(".").lower()
    return any(d == s or d.endswith("." + s) for s in X_DOMAINS)


def export_from_browser(browser: str, dest: str) -> bool:
    """读浏览器 Cookie 库，筛出 X 会话 Cookie，写入 dest（twikit JSON 格式）。

    成功返回 True；失败（浏览器不存在/未登录 X/解密失败等）返回 False。
    """
    try:
        from yt_dlp.cookies import extract_cookies_from_browser
    except ImportError:
        log.error("缺少 yt-dlp，无法导入浏览器 Cookie：pip install yt-dlp")
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
    os.replace(tmp, dest)
    log.info("已从浏览器 [%s] 导入 %d 个 X Cookie -> %s",
             browser, len(cookies), dest)
    return True


def try_import(browsers: list, dest: str) -> bool:
    """按顺序尝试多个浏览器，任一成功即返回 True。"""
    for b in browsers:
        if export_from_browser(b, dest):
            return True
    return False
