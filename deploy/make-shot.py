#!/usr/bin/env python3
"""make-shot.py — 生成"设备外观"展示截图（用于 README / Release 封面）

把 reMarkable 2 的屏幕内容合成到设备外壳边框上：

  1) 取屏幕内容，两种方式任选：
     a. 实时从设备抓取（推荐）：SSH 触发系统截图守护进程
        （touch /tmp/screenshot），把生成的 PNG 拷回本地；
     b. 直接用本地图片（例如 book/ 里已保存的收藏帖图）：
        python3 deploy/make-shot.py --input /path/screen.png

  2) 用 PIL 画一个 reMarkable 2 风格的机身外壳（圆角、边框、阴影），
     把屏幕图按比例居中贴入显示区。

用法：
  # 从设备抓屏（<IP> 为 reMarkable 局域网 IP）
  python3 deploy/make-shot.py --device 192.168.1.50 --output release-shot.png

  # 用本地图片合成
  python3 deploy/make-shot.py --input screen.png --output release-shot.png

  # 屏幕图是横的（原生 1872x1404）且内容竖排时默认自动转 90°，可用
  # --no-rotate 关闭
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
import urllib.request

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("缺少 Pillow：pip install Pillow")


def grab_from_device(ip):
    """SSH 触发系统截图守护进程并拷回 PNG。"""
    ssh = ["ssh", "-o", "StrictHostKeyChecking=accept-new",
           "-o", "ConnectTimeout=10", f"root@{ip}"]
    subprocess.run(ssh + ["touch", "/tmp/screenshot"], check=True)

    # 守护进程写 PNG 需要一点时间，轮询检查输出文件
    import time
    paths = ["/usr/share/remarkable/screenshot.png",
             "/home/root/.local/share/remarkable/screenshots/"]
    out = None
    for _ in range(20):
        time.sleep(0.5)
        for p in paths:
            r = subprocess.run(ssh + ["ls", p],
                               capture_output=True, text=True)
            if r.returncode == 0:
                out = p
                break
        if out:
            break
        # 有些版本存为带时间戳的文件名，用通配符兜底
        r = subprocess.run(ssh + ["ls", "-t", paths[1] + "*.png"],
                           capture_output=True, text=True, shell=False)
        if r.returncode == 0 and r.stdout.strip():
            out = r.stdout.strip().split("\n")[0]
            break
    if not out:
        sys.exit("未在设备上找到截图输出（检查 /usr/share/remarkable 或 "
                 "~/.local/share/remarkable/screenshots）")
    tmp = tempfile.NamedTemporaryFile(suffix=".png", delete=False).name
    subprocess.run(["scp", "-o", "StrictHostKeyChecking=accept-new",
                    "-o", "ConnectTimeout=10", f"root@{ip}:" + out, tmp],
                   check=True)
    return tmp


def compose(screen_path, out_path, auto_rotate):
    screen = Image.open(screen_path).convert("RGB")
    # 设备原生 fb 是横屏 1872x1404，内容（竖排页）会跟着躺倒；
    # 仅当输入为横图时自动转 90° 回到竖排（--no-rotate 关闭）
    if auto_rotate and screen.size[0] > screen.size[1]:
        screen = screen.rotate(-90, expand=True)
    sw, sh = screen.size

    # 显示区按 3:4（1404x1872）竖排；设备外观比显示区略大（边框 + 机身）
    margin = int(sw * 0.08)          # 机身四周留白
    bezel = int(sw * 0.012)          # 屏幕到机身边缘的黑边
    body_w = sw + 2 * (bezel + margin)
    body_h = sh + 2 * (bezel + margin)
    radius = int(body_w * 0.05)

    img = Image.new("RGB", (body_w, body_h), (255, 255, 255))
    d = ImageDraw.Draw(img)

    # 机身（reMarkable 2 深灰） + 底部圆角阴影
    d.rounded_rectangle([3, 3, body_w - 3, body_h - 3], radius=radius,
                        fill=(200, 200, 200))
    d.rounded_rectangle([0, 0, body_w, body_h], radius=radius,
                        fill=(46, 46, 46))

    # 屏幕区（黑边内嵌屏幕图，等比例居中）
    box = (bezel + margin, bezel + margin,
           body_w - bezel - margin, body_h - bezel - margin)
    d.rounded_rectangle(box, radius=int(radius * 0.6), fill=(0, 0, 0))
    target = (box[2] - box[0], box[3] - box[1])
    scale = min(target[0] / sw, target[1] / sh)
    nw, nh = int(sw * scale), int(sh * scale)
    nx = (box[0] + box[2] - nw) // 2
    ny = (box[1] + box[3] - nh) // 2
    img.paste(screen.resize((nw, nh), Image.LANCZOS), (nx, ny))

    img.save(out_path, "PNG")
    print(f"已生成设备展示图: {out_path}  ({img.size[0]}x{img.size[1]})")


def main():
    ap = argparse.ArgumentParser(description="生成设备外观展示截图")
    ap.add_argument("--device", help="reMarkable IP，实时抓屏（推荐）")
    ap.add_argument("--input", help="本地屏幕图 PNG（与 --device 二选一）")
    ap.add_argument("--output", default="release-shot.png")
    ap.add_argument("--no-rotate", action="store_true",
                    help="屏幕图为竖排时不旋转")
    args = ap.parse_args()

    if not args.device and not args.input:
        sys.exit("请指定 --device <IP> 或 --input <png>")
    src = args.input
    if args.device:
        print(f"从设备 {args.device} 抓屏…")
        src = grab_from_device(args.device)
    compose(src, args.output, auto_rotate=(not args.no_rotate))


if __name__ == "__main__":
    main()
