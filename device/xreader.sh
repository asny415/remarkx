#!/bin/sh
# xreader — reMarkable 1 上的 X 阅读器
#
# 原理：从家中转站下载渲染好的整页 PNG，用系统 SAS(simple) 全屏显示，
#       底部 4 个按钮翻页/刷新/退出。设备端不做任何解析，零依赖。
#       中转站按需抓取：首页数据过期时 relay 才请求 X（约 10~20 秒），
#       此期间设备显示"正在抓取"加载页（可取消）；翻页只读缓存，秒开。
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
    wget -q -t 3 --timeout=20 -O "$IMG" "$RELAY/page?p=$1"
}

loading_scene() {
    # $1 = 已等待秒数
    printf '@timeout 10\n'
    printf 'label 100 720 1204 56 正在从 X 抓取最新内容…\n'
    printf 'label 100 800 1204 44 已等待 %d 秒，一般 10~20 秒完成\n' "$1"
    printf 'label 100 860 1204 40 期间请勿合盖\n'
    printf 'button:cancel 502 1000 400 100 取消\n'
}

# 加载页循环：每 10 秒重绘一次并显示已等待时长，
# 后台下载完成（flag 文件出现）立即返回。
# 返回 0 = 下载完成，1 = 用户点取消，2 = 超时
loading_loop() {
    t0=$(date +%s)
    while :; do
        [ -f /tmp/rmx_fetch_done ] && return 0
        el=$(( $(date +%s) - t0 ))
        [ $el -gt 90 ] && return 2
        out=$(loading_scene "$el" | "$SIMPLE" 2>/dev/null)
        [ "$out" = "cancel" ] && return 1
    done
}

# 首页加载：先问 relay 数据是否过期（stale）。
#   过期 -> 后台抓取 + 前台加载页（可取消）
#   未过期 -> 直接取页（秒回）
# 返回 0 成功 / 1 用户取消 / 2 失败
load_first_page() {
    st=$(wget -qO- --timeout=5 "$RELAY/api/status" 2>/dev/null)
    case "$st" in
        *'"stale": true'* | *'"stale":true'*)
            rm -f /tmp/rmx_fetch_done
            ( wget -q -t 1 --timeout=90 -O "$IMG" "$RELAY/page?p=0" \
                && touch /tmp/rmx_fetch_done ) &
            wpid=$!
            loading_loop
            r=$?
            wait "$wpid" 2>/dev/null
            if [ $r -eq 1 ]; then
                kill "$wpid" 2>/dev/null
                rm -f /tmp/rmx_fetch_done
                return 1
            fi
            [ -f /tmp/rmx_fetch_done ] || return 2
            rm -f /tmp/rmx_fetch_done
            return 0
            ;;
        *)
            fetch_page 0 || return 2
            return 0
            ;;
    esac
}

p=0
first=1
while :; do
    ok=0
    if [ $first -eq 1 ]; then
        load_first_page
        case $? in
            0)  ok=1 ;;
            1)  break ;;     # 用户取消
        esac
    else
        i=0
        while [ $i -lt 3 ]; do
            if fetch_page "$p"; then
                ok=1
                break
            fi
            i=$((i + 1))
            sleep 1
        done
    fi

    if [ $ok -eq 1 ]; then
        out=$(scene | "$SIMPLE" 2>/dev/null)
    else
        out=$(err_scene | "$SIMPLE" 2>/dev/null)
    fi

    btn=${out%%:*}
    case "$btn" in
        next)
            p=$((p + 1))
            first=0
            ;;
        prev)
            if [ $p -gt 0 ]; then
                p=$((p - 1))
            fi
            first=0
            ;;
        refresh)
            p=0      # first 保持 1：下一轮重新判断是否过期
            ;;
        *)
            break
            ;;
    esac
done
