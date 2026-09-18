#!/usr/bin/env bash
# 生成 kickpi_ui 用的中文字库 app/kickpi_ui/lv_font_k7_cjk_20.c
#
# 字符集：ASCII + GB2312 一级汉字（3755 个常用字）+ 常用中文标点，共 3877 字形，
# 20px / 4bpp，约 430KB 进 .rodata（FIT 窗口 8192 扇区，加上后仍有余量）。
#
# 字体来源（都允许再分发）：
#   ASCII        Montserrat-Medium  （OFL，LVGL 自带，和界面其余英文字体一致）
#   “”‘’…—·×÷°  DejaVuSans         （LVGL 自带；前两者都没有这几个符号）
#   汉字/全角标点 DroidSansFallbackFull（Apache-2.0，Debian fonts-droid-fallback）
#
# DroidSansFallback 是纯 CJK 的回退字体，**没有拉丁字母**，所以不能单用它。
#
# 依赖：node（npx 拉 lv_font_conv@1.5.3）、apt-get download（不需要 root）。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LVGL_FONTS=${LVGL_FONTS:-$ROOT/../apps/graphics/lvgl/lvgl/scripts/built_in_font}
WORK=${WORK:-$HOME/rk3576-amp/font}
OUT=$ROOT/app/kickpi_ui/lv_font_k7_cjk_20.c
PUNCT="“”‘’…—·×÷°"

mkdir -p "$WORK"
cd "$WORK"
DROID=pkg/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf
if [[ ! -f $DROID ]]; then
  apt-get download fonts-droid-fallback
  dpkg -x fonts-droid-fallback_*.deb pkg
fi

python3 - "$PUNCT" > chars_cjk.txt <<'PY'
import sys
out = []
for hi in range(0xB0, 0xD8):            # GB2312 一级汉字区
    for lo in range(0xA1, 0xFF):
        try:
            out.append(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            pass
out += [c for c in '，。！？、；：（）《》【】～％＋－' if c not in sys.argv[1]]
sys.stdout.write(''.join(out))
PY

npx -y lv_font_conv@1.5.3 --size 20 --bpp 4 --format lvgl --no-compress \
  --font "$LVGL_FONTS/Montserrat-Medium.ttf" -r 0x20-0x7E \
  --font "$LVGL_FONTS/DejaVuSans.ttf" --symbols "$PUNCT" \
  --font "$DROID" --symbols "$(cat chars_cjk.txt)" \
  --lv-font-name lv_font_k7_cjk_20 -o "$OUT"

# 头部那行 Opts 里是 3000 多个汉字，没有参考价值，换成一句说明
python3 - "$OUT" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
s = re.sub(r' \* Opts: .*\n',
           ' * Opts: 由 scripts/gen-cjk-font.sh 生成（ASCII + GB2312 一级汉字 + 常用标点）\n'
           ' * Fonts: Montserrat (OFL), DejaVu Sans, Droid Sans Fallback (Apache-2.0)\n',
           s, count=1)
open(p, 'w', encoding='utf-8').write(s)
PY
echo "生成 $OUT"
