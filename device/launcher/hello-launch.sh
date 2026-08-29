#!/bin/sh
systemctl stop xochitl
rm -f /tmp/epframebuffer.lock
export LD_LIBRARY_PATH=/home/root/xreader
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
QT_QUICK_BACKEND=epaper /home/root/xreader/xr -platform epaper
systemctl start xochitl
