#!/bin/sh
# run-reader.sh — 阅读器启动入口（由 hello-hotkey 长按触发）
# 长按顶部中央 → 直接进入阅读器；退出时自动拉回原生 xochitl。
# 清掉残留的阅读器进程（busybox 无 pkill，用 killall；否则残留 xr 会让
# hello-hotkey 误判 reader 在跑而跳过长按）
killall -9 xr 2>/dev/null || true
systemctl stop xochitl
rm -f /tmp/epframebuffer.lock /tmp/epd.lock
export QT_QUICK_BACKEND=epaper
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty
# AddressSanitizer：xr 链接 libasan.so.8，放在 /home/root/xreader
export LD_LIBRARY_PATH=/home/root/xreader
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1

/home/root/xreader/xr -platform epaper
systemctl start xochitl
