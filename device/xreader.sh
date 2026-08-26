#!/bin/sh
# xreader — reMarkable 1 上的 X 阅读器
#
# 原理：从家中转站下载渲染好的整页 PNG，用系统 SAS(simple) 全屏显示，
#       底部 4 个按钮翻页/刷新/退出。设备端不做任何解析，零依赖。
#
# 依赖：wget（系统自带）、/opt/bin/simple（rmkit，install_device.sh 安装）
# 配置：/opt/rmx/config  或环境变量 RELAY（中转站地址）

[ -f /opt/rmx/config ] && . /opt/rmx/config

RELAY="${RELAY:-http://192.168.1.100:8788}"
SIMPLE="${SIMPLE:-/opt/bin/simple}"
IMG=/tmp/rmx_page.png

W=1404
H=1872

scene() {
    # 整页图片 + 底部四个按钮（按钮由 simple 绘制，可点击）
    printf '@timeout 7200\n'
    printf 'image 0 0 %s %s %s\n' "$W" "$H" "$IMG"
    printf 'button:prev 36 1720 318 120 上一页\n'
    printf 'button:next 370 1720 318 120 下一页\n'
    printf 'button:refresh 704 1720 318 120 刷新\n'
    printf 'button:exit 1038 1720 318 120 退出\n'
}

err_scene() {
    printf 'label 100 780 1200 60 无法连接中转站 %s\n' "$RELAY"
    printf 'label 100 860 1200 48 请确认电脑已开机、remarkx 服务在运行\n'
    printf 'button:exit 502 980 400 100 退出\n'
}

fetch_page() {
    # $1 = 页码
    wget -qO "$IMG" "$RELAY/page?p=$1"
}

p=0
while :; do
    ok=0
    i=0
    while [ $i -lt 3 ]; do
        if fetch_page "$p"; then
            ok=1
            break
        fi
        i=$((i + 1))
        sleep 1
    done

    if [ $ok -eq 1 ]; then
        out=$(scene | "$SIMPLE" 2>/dev/null)
    else
        out=$(err_scene | "$SIMPLE" 2>/dev/null)
    fi

    btn=${out%%:*}
    case "$btn" in
        next)    p=$((p + 1)) ;;
        prev)
            if [ $p -gt 0 ]; then
                p=$((p - 1))
            fi
            ;;
        refresh) p=0 ;;
        *)       break ;;
    esac
done
