#!/bin/sh
# setup-book.sh — 在设备书架上生成「remarkx 阅读器」特殊书（点开即启动阅读器）
#
# 原理：直接手写 xochitl 3.x 的 .content 会被判定无效并清除，
#       所以克隆设备上一本最小的现有 PDF 文档（结构/页数完全一致），
#       仅替换 PDF 二进制（N 页封面图）与可见名称。
# 依赖：本机 relay/.venv（PIL 生成封面 PDF）+ ssh 免密登录设备
# 用法：sh device/launcher/setup-book.sh [root@192.168.3.135]
set -e
TARGET="${1:-root@192.168.3.135}"
HERE=$(cd "$(dirname "$0")" && pwd)
PY="$HERE/../../relay/.venv/bin/python"

echo "[1/4] 在设备上找页数最少的 PDF 文档作为结构模板..."
SRC=$(ssh root@${TARGET#*@} 2>/dev/null || true; ssh -o BatchMode=yes "$TARGET" '
  for m in /home/root/.local/share/remarkable/xochitl/*.metadata; do
    c=${m%.metadata}.content
    [ -f "$c" ] || continue
    n=$(grep -o "\"pageCount\": [0-9]*" "$c" | grep -o "[0-9]*")
    echo "$n $c"
  done | sort -n | head -n1 | cut -d" " -f2 | xargs -r basename | sed "s/.content//"')
echo "    模板: $SRC"
[ -n "$SRC" ] || { echo "未找到 PDF 文档"; exit 1; }
PAGES=$(ssh -o BatchMode=yes "$TARGET" "grep -o '\"pageCount\": [0-9]*' \
  /home/root/.local/share/remarkable/xochitl/$SRC.content | grep -o '[0-9]*'")
echo "    页数: $PAGES"

echo "[2/4] 生成本地封面 PDF（$PAGES 页）..."
"$PY" - "$PAGES" <<'PYEOF'
import sys
from PIL import Image, ImageDraw, ImageFont
n = int(sys.argv[1])
fp = '/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc'
pages = []
for pno in range(n):
    img = Image.new('RGB', (1404, 1872), 'white')
    d = ImageDraw.Draw(img)
    f1 = ImageFont.truetype(fp, 120)
    f2 = ImageFont.truetype(fp, 44)
    f3 = ImageFont.truetype(fp, 30)
    def center(t, y, f, fill=(26, 26, 26)):
        w = d.textlength(t, font=f)
        d.text(((1404 - w) / 2, y), t, font=f, fill=fill)
    if pno == 0:
        center('remarkx', 520, f1)
        center('阅 读 器', 690, f1)
        center('— 点按打开 X 阅读器 —', 1000, f2, (90, 90, 90))
        center('阅读进度自动保存 · 退出返回书架', 1080, f3, (140, 140, 140))
    else:
        center('remarkx 阅读器', 900, f2, (160, 160, 160))
    pages.append(img)
pages[0].save('/tmp/rmxb.pdf', 'PDF', resolution=226,
              save_all=True, append_images=pages[1:])
PYEOF

echo "[3/4] 克隆结构并上传..."
UUID=$(cat /proc/sys/kernel/random/uuid)
scp -o BatchMode=yes /tmp/rmxb.pdf "$TARGET:/tmp/rmxb.pdf"
ssh -o BatchMode=yes "$TARGET" "
D=/home/root/.local/share/remarkable/xochitl; S=$SRC; N=$UUID
cp \$D/\$S.content \$D/\$N.content
cp \$D/\$S.metadata \$D/\$N.metadata
mv /tmp/rmxb.pdf \$D/\$N.pdf
sed -i 's/\"visibleName\": \"[^\"]*\"/\"visibleName\": \"remarkx 阅读器\"/' \$D/\$N.metadata
sed -i 's/\"lastOpened\": \"[^\"]*\"/\"lastOpened\": \"0\"/' \$D/\$N.metadata
sed -i 's/\"lastOpenedPage\": [0-9]*/\"lastOpenedPage\": 0/' \$D/\$N.metadata
sed -i 's/^LastOpen=.*/LastOpen=/' /home/root/.config/remarkable/xochitl.conf
echo \$N > /home/root/xreader/shelfbook.uuid
systemctl restart xochitl"

echo "[4/4] 重启触发守护..."
ssh -o BatchMode=yes "$TARGET" 'systemctl restart hello-hotkey'
echo "完成！书架中应出现「remarkx 阅读器」，点开即启动。"
