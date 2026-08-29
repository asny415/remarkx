#!/bin/bash
# install-remarkable.sh — 一键部署 remarkx 阅读器到 reMarkable 设备
#
# 用法：
#   ./install-remarkable.sh <设备IP> [relay地址]
#
#   设备IP      reMarkable 的局域网 IP（需已开启开发者模式/SSH）
#   relay地址   中转站 URL，默认自动取本机局域网 IP 的 8788 端口
#
# 认证：优先 SSH 密钥；否则若装了 sshpass 且设了 SSHPASS 环境变量用密码；
#       都不满足则 ssh 交互式输入密码（reMarkable 默认 root）。
#
# 部署内容：
#   /home/root/xreader/xr           阅读器（单文件，QML 已内嵌）
#   /home/root/xreader/run-reader.sh hello-hotkey 触发的启动脚本
#   /home/root/xreader/config       中转站地址
#   /home/root/hello-launch/hello-hotkey       长按守护进程
#   /home/root/hello-launch/hello-hotkey.service 并注册为开机自启

set -euo pipefail

IP="${1:?用法: $0 <设备IP> [relay地址]}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

# ---------- 中转站地址 ----------
if [ -n "${2:-}" ]; then
    RELAY="$2"
else
    LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
    RELAY="http://${LAN_IP:-192.168.1.100}:8788"
fi
printf '中转站地址: %s\n' "$RELAY"

# ---------- 本地构建 ----------
# 注意：xr 需在 Remarkable SDK 环境（environment-setup-cortexa7hf...）中构建
echo "== 构建 xr =="
if [ ! -d "${ROOT}/xreader-app/build" ]; then
    (cd "${ROOT}/xreader-app" && cmake -B build) \
        || { echo "构建配置失败——请先在 SDK 环境执行: cd xreader-app && cmake -B build"; exit 1; }
fi
cmake --build "${ROOT}/xreader-app/build" >/dev/null
test -x "${ROOT}/xreader-app/build/xr" || { echo "构建 xr 失败（需在 SDK 环境中运行本脚本）"; exit 1; }

# hello-hotkey 需交叉编译为 armv7l（用本机编译器会 203/EXEC 跑不起来）
echo "== 构建 hello-hotkey (armv7l) =="
RM_SDK="${RM_SDK:-/home/wwq/remarkable-sdk}"
CROSS_GCC="$RM_SDK/sysroots/aarch64-codexsdk-linux/usr/bin/arm-remarkable-linux-gnueabi/arm-remarkable-linux-gnueabi-gcc"
TARGET_SYSROOT="$RM_SDK/sysroots/cortexa7hf-neon-remarkable-linux-gnueabi"
mkdir -p "${ROOT}/deploy/build"
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
# 先停守护进程、杀阅读器：Linux 不允许覆盖正在执行的二进制（ETXTBSY）
echo "== 停止正在运行的阅读器/守护 =="
device 'systemctl stop hello-hotkey 2>/dev/null || true
pkill -f "xreader/xr" 2>/dev/null || true
sleep 1
# 阅读器会停掉 xochitl，这里拉回原生界面，避免设备停在无 UI 状态
systemctl start xochitl 2>/dev/null || true
mkdir -p /home/root/xreader/book /home/root/hello-launch'

echo "== 拷贝阅读器 =="
push "${ROOT}/xreader-app/build/xr" "root@${IP}:/home/root/xreader/xr"
push "${ROOT}/device/launcher/run-reader.sh" "root@${IP}:/home/root/xreader/run-reader.sh"
printf '%s' "$RELAY" > "${ROOT}/deploy/build/config"
push "${ROOT}/deploy/build/config" "root@${IP}:/home/root/xreader/config"

echo "== 拷贝长按守护 =="
push "${ROOT}/deploy/build/hello-hotkey" "root@${IP}:/home/root/hello-launch/hello-hotkey"
push "${ROOT}/device/launcher/hello-launch.sh" "root@${IP}:/home/root/hello-launch/hello-launch.sh"
push "${ROOT}/device/launcher/hello-hotkey.service" "root@${IP}:/home/root/hello-launch/hello-hotkey.service"

echo "== 安装服务并启用 =="
device 'chmod +x /home/root/xreader/run-reader.sh /home/root/hello-launch/*.sh /home/root/hello-launch/hello-hotkey
ln -sf /home/root/hello-launch/hello-hotkey.service /etc/systemd/system/hello-hotkey.service
systemctl daemon-reload
systemctl enable hello-hotkey >/dev/null 2>&1
systemctl start hello-hotkey'

# ---------- 校验 ----------
echo "== 校验 =="
device 'ls -l /home/root/xreader/xr /home/root/hello-launch/hello-hotkey /home/root/xreader/config
systemctl is-active hello-hotkey
pgrep -x xochitl >/dev/null && echo "xochitl 运行中（原声界面正常）"'

echo
echo "安装完成。长按顶部中央 3 秒即可进入阅读器。"
echo "中转站: ${RELAY}（请确认该地址设备能访问、relay 在运行）"
