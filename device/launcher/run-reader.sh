#!/bin/sh
# run-reader.sh — 阅读器启动入口（由 hello-hotkey 长按触发）
# 长按顶部中央 → 直接进入阅读器；退出时自动拉回原生 xochitl。
pkill -f "xreader/xr" 2>/dev/null
sleep 1
systemctl stop xochitl
rm -f /tmp/epframebuffer.lock /tmp/epd.lock
export QT_QUICK_BACKEND=epaper
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty

/home/root/xreader/xr -platform epaper
systemctl start xochitl
