#!/bin/bash
# install-remarkable.sh — 一键部署 remarkx 独立阅读器到 reMarkable 设备
#
# 设备端自抓取/自渲染（无 relay）：需配置代理 + X Cookie 才能直连 x.com。
#
# 用法：
#   ./install-remarkable.sh <设备IP> [--proxy http://PC:7890] \
#       [--cookie-file /path/cookies.json | --browser brave,chromium]
#
#   设备IP      reMarkable 的局域网 IP（需已开启开发者模式/SSH）
#   --proxy     X 直连用的代理（须设备能访问，如家里 PC 的 Clash/V2Ray）
#   --cookie-file  直接提供 X Cookie JSON（{auth_token,ct0,...}）
#   --browser      从 PC 浏览器自动提取（逗号分隔多个，任一成功即可）
#   都不给时：交互式询问代理，并尝试从浏览器提取 Cookie
#
# 认证：优先 SSH 密钥；否则若装了 sshpass 且设了 SSHPASS 环境变量用密码；
#       都不满足则 ssh 交互式输入密码（reMarkable 默认 root）。
#
# 部署内容：
#   /home/root/xreader/xr            阅读器（单文件，QML 已内嵌）
#   /home/root/xreader/run-reader.sh hello-hotkey 触发的启动脚本
#   /home/root/xreader/config.json   代理/Cookie/字体配置
#   /home/root/xreader/cookies.json  X 登录态 Cookie
#   /home/root/xreader/fonts/remarkx-cjk.ttf   渲染字体（换字体同名覆盖）
#   /home/root/hello-launch/hello-hotkey       长按守护进程
#   /home/root/hello-launch/hello-hotkey.service 并注册为开机自启

set -euo pipefail

IP="${1:?用法: $0 <设备IP> [--proxy URL] [--cookie-file F | --browser B]}"
shift
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

PROXY=""
COOKIE_FILE=""
BROWSERS=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --proxy) PROXY="${2:-}"; shift 2 ;;
        --cookie-file) COOKIE_FILE="${2:-}"; shift 2 ;;
        --browser) BROWSERS="${2:-}"; shift 2 ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done

# ---------- 认证方式 ----------
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)
if command -v sshpass >/dev/null 2>&1 && [ -n "${SSHPASS:-}" ]; then
    SSH=(sshpass -e ssh "${SSH_OPTS[@]}")
    SCP=(sshpass -e scp "${SSH_OPTS[@]}")
else
    SSH=(ssh "${SSH_OPTS[@]}")
    SCP=(scp "${SSH_OPTS[@]}")
fi
device() { "${SSH[@]}" "root@${IP}" "$@"; }
push()   { "${SCP[@]}" "$@"; }

# 阶段一：比较本地与设备端哈希，记录需要拷贝的文件（不立即拷贝，
# 必须先确定 CHANGED 再停服务，否则正在运行的二进制被覆盖会 ETXTBSY）
TO_COPY=()
CHANGED=0
check_changed() {
    local src="$1" dst="$2"
    local local_md5 remote_md5
    local_md5="$(md5sum "$src" | awk '{print $1}')"
    remote_md5="$(device "md5sum \"$dst\" 2>/dev/null" || true)"
    remote_md5="$(printf '%s' "$remote_md5" | awk '{print $1}' | tr -d '[:space:]')"
    if [ -n "$remote_md5" ] && [ "$local_md5" = "$remote_md5" ]; then
        echo "  跳过（未变化）: $dst"
    else
        echo "  需更新: $dst"
        TO_COPY+=("$src|$dst")
        CHANGED=1
    fi
}

# 阶段二：把记录到的文件逐个 scp 到设备（此时服务已停）
copy_all() {
    local entry src dst
    for entry in "${TO_COPY[@]}"; do
        src="${entry%%|*}"
        dst="${entry#*|}"
        echo "  拷贝: $dst"
        push "$src" "root@${IP}:$dst"
    done
}

# ---------- 代理 ----------
if [ -z "$PROXY" ]; then
    read -rp "X 直连代理（设备需能访问，如 http://192.168.1.100:7890，直接回车跳过）: " PROXY
fi
if [ -n "$PROXY" ]; then
    printf '代理: %s\n' "$PROXY"
else
    echo "警告：未配置代理。若 x.com 不可直连，阅读器将无法抓取内容。"
fi

# ---------- Cookie ----------
mkdir -p "${ROOT}/deploy/build"
LOCAL_COOKIE="${ROOT}/deploy/build/cookies.json"
# 优先用项目 venv 的 python（yt-dlp 较新，能解 v11 Cookie）；否则退回系统 python3
if [ -x "${ROOT}/.venv/bin/python" ]; then
    PY="${ROOT}/.venv/bin/python"
else
    PY="python3"
fi
if [ -n "$COOKIE_FILE" ]; then
    cp "$COOKIE_FILE" "$LOCAL_COOKIE"
    echo "使用 Cookie 文件: $COOKIE_FILE"
elif [ -n "$BROWSERS" ]; then
    echo "== 从浏览器 [$BROWSERS] 导入 X Cookie =="
    if ! "$PY" "${ROOT}/deploy/import-cookies.py" "$BROWSERS" "$LOCAL_COOKIE"; then
        echo "Cookie 导入失败"; exit 1
    fi
else
    echo "== 尝试从浏览器导入 X Cookie（brave/chromium/firefox） =="
    if ! "$PY" "${ROOT}/deploy/import-cookies.py" \
            "brave,chromium,firefox" "$LOCAL_COOKIE"; then
        echo "浏览器导入失败。可用 --browser 指定，或用 --cookie-file 提供。"
        exit 1
    fi
fi
test -s "$LOCAL_COOKIE" || { echo "Cookie 为空"; exit 1; }

# ---------- 本地构建 ----------
# 注意：xr 需在 Remarkable SDK 环境（environment-setup-cortexa7hf...）中构建
echo "== 构建 xr（AddressSanitizer 版：隔离释放内存，稳定运行） =="
if [ ! -d "${ROOT}/xreader-app/build" ]; then
    (cd "${ROOT}/xreader-app" && cmake -B build -DREMARKX_ASAN=ON) \
        || { echo "构建配置失败——请先在 SDK 环境执行: cd xreader-app && cmake -B build -DREMARKX_ASAN=ON"; exit 1; }
else
    (cd "${ROOT}/xreader-app" && cmake -B build -DREMARKX_ASAN=ON) \
        || { echo "重新配置失败"; exit 1; }
fi
cmake --build "${ROOT}/xreader-app/build" >/dev/null
test -x "${ROOT}/xreader-app/build/xr" || { echo "构建 xr 失败（需在 SDK 环境中运行本脚本）"; exit 1; }

# ASan 运行库 libasan.so.8（随 SDK 提供），部署到设备供 xr 链接
LIBASAN="${TARGET_SYSROOT:-${RM_SDK:-/home/wwq/remarkable-sdk}/sysroots/cortexa7hf-neon-remarkable-linux-gnueabi}/usr/lib/libasan.so.8.0.0"
if [ ! -f "$LIBASAN" ]; then
    LIBASAN="${RM_SDK:-/home/wwq/remarkable-sdk}/sysroots/cortexa7hf-neon-remarkable-linux-gnueabi/usr/lib/libasan.so.8"
fi
if [ -f "$LIBASAN" ]; then
    cp "$LIBASAN" "${ROOT}/deploy/build/libasan.so.8"
    echo "libasan: $(basename "$LIBASAN")"
else
    echo "警告：找不到 libasan.so.8，设备端可能因缺少运行库无法启动"; exit 1
fi

# hello-hotkey 需交叉编译为 armv7l（用本机编译器会 203/EXEC 跑不起来）
echo "== 构建 hello-hotkey (armv7l) =="
RM_SDK="${RM_SDK:-/home/wwq/remarkable-sdk}"
CROSS_GCC="$RM_SDK/sysroots/aarch64-codexsdk-linux/usr/bin/arm-remarkable-linux-gnueabi/arm-remarkable-linux-gnueabi-gcc"
TARGET_SYSROOT="$RM_SDK/sysroots/cortexa7hf-neon-remarkable-linux-gnueabi"
if [ -x "$CROSS_GCC" ]; then
    "$CROSS_GCC" -O2 -Wall -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a7 \
        --sysroot="$TARGET_SYSROOT" \
        -o "${ROOT}/deploy/build/hello-hotkey" \
        "${ROOT}/device/launcher/hello-hotkey.c"
else
    echo "找不到交叉编译器 $CROSS_GCC（可用 RM_SDK 指定 SDK 路径）"
    exit 1
fi

# ---------- 连通性检查 ----------
echo "== 连接设备 ${IP} =="
device 'echo connected; uname -m' || { echo "无法连接，请检查 IP / SSH"; exit 1; }

# ---------- 部署 ----------
echo "== 准备设备目录 =="
device 'mkdir -p /home/root/xreader/book /home/root/xreader/fonts /home/root/hello-launch'

echo "== 比较本地与设备端文件 =="
check_changed "${ROOT}/xreader-app/build/xr" "/home/root/xreader/xr"
check_changed "${ROOT}/device/launcher/run-reader.sh" "/home/root/xreader/run-reader.sh"
check_changed "${ROOT}/xreader-app/fonts/remarkx-cjk.ttf" "/home/root/xreader/fonts/remarkx-cjk.ttf"
check_changed "${ROOT}/deploy/build/libasan.so.8" "/home/root/xreader/libasan.so.8"

echo "== 写配置 =="
# 代理无协议头时补 http://，避免设备端解析失败
if [ -n "$PROXY" ] && ! printf '%s' "$PROXY" | grep -q '://'; then
    PROXY="http://${PROXY}"
fi
{
    printf '{\n'
    printf '  "proxy": "%s",\n' "$PROXY"
    printf '  "cookies": "/home/root/xreader/cookies.json"\n'
    printf '}\n'
} > "${ROOT}/deploy/build/config.json"
check_changed "${ROOT}/deploy/build/config.json" "/home/root/xreader/config.json"
check_changed "$LOCAL_COOKIE" "/home/root/xreader/cookies.json"

echo "== 比较长按守护 =="
check_changed "${ROOT}/deploy/build/hello-hotkey" "/home/root/hello-launch/hello-hotkey"
check_changed "${ROOT}/device/launcher/hello-launch.sh" "/home/root/hello-launch/hello-launch.sh"
check_changed "${ROOT}/device/launcher/hello-hotkey.service" "/home/root/hello-launch/hello-hotkey.service"

# 只有文件有变化时才停服务/杀阅读器（Linux 不允许覆盖正在执行的二进制 ETXTBSY）；
# 全部未变化则不动运行中的阅读器
if [ "$CHANGED" = "1" ]; then
    echo "== 停止正在运行的阅读器/守护 =="
    device 'systemctl stop hello-hotkey 2>/dev/null || true
killall -9 xr 2>/dev/null || true
sleep 1
systemctl start xochitl 2>/dev/null || true'
    echo "== 拷贝更新的文件 =="
    copy_all
fi

echo "== 安装服务并启用 =="
device 'chmod +x /home/root/xreader/run-reader.sh /home/root/hello-launch/*.sh /home/root/hello-launch/hello-hotkey
ln -sf /home/root/hello-launch/hello-hotkey.service /etc/systemd/system/hello-hotkey.service
systemctl daemon-reload
systemctl enable hello-hotkey >/dev/null 2>&1
systemctl start hello-hotkey'

# ---------- 校验 ----------
echo "== 校验 =="
device 'ls -l /home/root/xreader/xr /home/root/xreader/fonts/remarkx-cjk.ttf /home/root/xreader/config.json /home/root/xreader/cookies.json /home/root/hello-launch/hello-hotkey
systemctl is-active hello-hotkey
pgrep -x xochitl >/dev/null && echo "xochitl 运行中（原声界面正常）"'

echo
echo "安装完成。长按顶部中央 3 秒进入阅读器。"
echo "代理: ${PROXY:-<未配置>}；Cookie 已部署到 /home/root/xreader/cookies.json"
