#!/bin/sh
systemctl stop xochitl
rm -f /tmp/epframebuffer.lock
QT_QUICK_BACKEND=epaper /home/root/xreader/xr -platform epaper
systemctl start xochitl
