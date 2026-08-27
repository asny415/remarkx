#!/bin/sh
# run-reader.sh — 阅读器启动入口（由 hello-hotkey 长按触发）
# 流程：先弹确认菜单（--menu），用户确认后进入阅读器；
#      取消/超时则回到原生 xochitl。阅读器自身退出时也会拉回 xochitl。
pkill -f "xreader/xr" 2>/dev/null
sleep 1
systemctl stop xochitl
rm -f /tmp/epframebuffer.lock /tmp/epd.lock
export QT_QUICK_BACKEND=epaper
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty

/home/root/xreader/xr -platform epaper --menu
rc=$?
if [ "$rc" -ne 0 ]; then
    systemctl start xochitl
    exit 0
fi

/home/root/xreader/xr -platform epaper
systemctl start xochitl
