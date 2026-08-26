#!/bin/sh
# 在 reMarkable 1 上运行（通过 SSH）：
#   ssh root@<设备IP>
#   sh install_device.sh http://192.168.1.100:8788
#
# 作用：安装 rmkit 的 simple 二进制 + xreader 脚本 + 配置。
# 安全：只写 /opt/bin/simple 和 /opt/rmx/，不碰系统文件。

set -e

RELAY_URL="${1:-http://192.168.1.100:8788}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v wget >/dev/null 2>&1 || {
    echo "缺少 wget，无法安装"; exit 1; }

mkdir -p /opt/rmx

# ---- 1) simple（rmkit 构建的 armhf 二进制）----
# 设备上的 busybox wget 不支持 HTTPS，所以不从设备直接下载：
# 在电脑上把 device/simple 一起 scp 过来（见 README），这里直接拷贝。
if [ -x /opt/bin/simple ]; then
    echo "simple 已存在，跳过"
elif [ -f "$HERE/simple" ]; then
    cp "$HERE/simple" /opt/bin/simple
    chmod +x /opt/bin/simple
    echo "simple 已从 $HERE/simple 安装"
else
    echo "缺少 simple 二进制（设备 wget 不支持 HTTPS，无法在线下载）。"
    echo "在电脑上执行："
    echo "    scp device/simple root@设备:/tmp/"
    echo "然后重跑本脚本。"
    exit 1
fi

# ---- 2) 脚本与配置 ----
cp "$HERE/xreader.sh" /opt/rmx/xreader.sh
chmod +x /opt/rmx/xreader.sh
echo "RELAY=$RELAY_URL" > /opt/rmx/config
echo "配置: RELAY=$RELAY_URL"

# ---- 3) 自检：simple 能否正常运行（3 秒后自动退出）----
echo "自检 simple（屏幕上会显示 3 秒）..."
printf '@timeout 3\nlabel 200 800 1000 80 remarkx 安装成功，3 秒后自动退出\n' \
    | /opt/bin/simple >/dev/null 2>&1 || true

echo ""
echo "安装完成。以后打开阅读器（SSH 到设备后执行）："
echo "    sh /opt/rmx/xreader.sh"
echo "或在电脑上加一个 SSH 别名（~/.ssh/config）："
echo "    Host rmx"
echo "        HostName <设备IP>"
echo "        User root"
echo "    然后: ssh rmx 'sh /opt/rmx/xreader.sh'"
