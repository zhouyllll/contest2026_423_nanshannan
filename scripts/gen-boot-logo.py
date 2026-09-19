#!/usr/bin/env python3
"""生成开机 logo（U-Boot 显示）并打包成 Rockchip resource 镜像。

用法：scripts/gen-boot-logo.py [输出目录]      默认 ~/rk3576-amp/out/logo
产物：logo.bmp、logo_kernel.bmp（同一张图，U-Boot → bootamp 之间不闪）、
      resource-logo.img（写 dtbo 分区 LBA 40960，见 amp/uboot/0011）。

★ 格式照出厂 logo：8 位调色板 + RLE8 压缩（kernel-6.1/logo.bmp 就是
  654x270 bpp8 comp1）。U-Boot 的 bmpdecoder 对这种格式是验证过的；全屏
  720x1280 的 24 位图要 2.7MB，两张就放不进 4MB 的分区，而 RLE8 下
  大片纯色背景只有几十 KB。U-Boot 解码成 RGB565 显示，所以只用平涂色，
  不用渐变（565 下会出色带）。
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 720, 1280
BG = (10, 14, 28)
ACCENT = (64, 156, 255)
WHITE = (240, 244, 250)
GREY = (140, 150, 170)

HOME = Path.home()
CJK = HOME / 'rk3576-amp/font/pkg/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf'
LATIN = Path(__file__).resolve().parents[1] / '../apps/graphics/lvgl/lvgl/scripts/built_in_font/Montserrat-Medium.ttf'
RESOURCE_TOOL = HOME / 'rk3576-amp/u-boot/tools/resource_tool'


def center(draw, y, text, font, fill):
    x0, y0, x1, y1 = draw.textbbox((0, 0), text, font=font)
    draw.text(((W - (x1 - x0)) // 2 - x0, y), text, font=font, fill=fill)


def render():
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    big = ImageFont.truetype(str(LATIN), 112)
    mid = ImageFont.truetype(str(LATIN), 40)
    cjk = ImageFont.truetype(str(CJK), 40)
    small = ImageFont.truetype(str(LATIN), 28)

    # 两个方块代表 AMP 的两个系统
    cy = 420
    d.rounded_rectangle((W // 2 - 150, cy - 60, W // 2 - 20, cy + 60), 18,
                        outline=ACCENT, width=8)
    d.rounded_rectangle((W // 2 + 20, cy - 60, W // 2 + 150, cy + 60), 18,
                        fill=ACCENT)
    center(d, 540, 'openvela', big, WHITE)
    d.rectangle((W // 2 - 90, 690, W // 2 + 90, 696), fill=ACCENT)
    # Droid 没有"·"，中间的点自己画
    left, right, gap = '双核异构', '端侧智能', 56
    lw = d.textlength(left, font=cjk)
    rw = d.textlength(right, font=cjk)
    x = (W - (lw + gap + rw)) / 2
    d.text((x, 730), left, font=cjk, fill=WHITE)
    d.text((x + lw + gap, 730), right, font=cjk, fill=WHITE)
    cx = x + lw + gap / 2
    d.ellipse((cx - 5, 755 - 5, cx + 5, 755 + 5), fill=ACCENT)
    center(d, 800, 'KICKPI K7  ·  RK3576', mid, GREY)
    center(d, 1150, 'openvela (A53) + Linux (A72)', small, GREY)
    return img


def rle8(pal_img):
    """8 位调色板图 → BMP RLE8 数据（自下而上，逐行 EOL，最后 EOBMP）。

    ★ 最后一行后面不能再写 EOL，直接 EOBMP。U-Boot 的 libnsbmp 在 EOL 上
      先 y++，y 到了高度就返回 BMP_DATA_ERROR；显示驱动只容忍 20 万像素
      以内的"部分解码"。出厂 logo（654x270）也是多写了这个 EOL，只是
      刚好在 20 万以内；全屏 720x1280 会被整张拒掉
      （"partially decoded bmp ... can not be too large"）。
    """
    px = pal_img.load()
    out = bytearray()
    for y in range(H - 1, -1, -1):
        x = 0
        while x < W:
            v = px[x, y]
            n = 1
            while x + n < W and n < 255 and px[x + n, y] == v:
                n += 1
            out += bytes((n, v))
            x += n
        if y:
            out += b'\x00\x00'
    out += b'\x00\x01'
    return bytes(out)


def write_bmp(path, pal_img):
    palette = pal_img.getpalette()[:256 * 3]
    palette += [0] * (256 * 3 - len(palette))
    table = b''.join(bytes((palette[i * 3 + 2], palette[i * 3 + 1],
                            palette[i * 3], 0)) for i in range(256))
    data = rle8(pal_img)
    off = 14 + 40 + len(table)
    size = off + len(data)
    hdr = b'BM' + struct.pack('<IHHI', size, 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 8, 1, len(data),
                       2835, 2835, 256, 0)
    Path(path).write_bytes(hdr + info + table + data)
    return size


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else HOME / 'rk3576-amp/out/logo')
    out.mkdir(parents=True, exist_ok=True)
    pal = render().quantize(colors=64, method=Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.NONE)
    pal.convert('RGB').save(out / 'logo-preview.png')
    for name in ('logo.bmp', 'logo_kernel.bmp'):
        n = write_bmp(out / name, pal)
        print(f'{name}: {n} 字节')
    img = out / 'resource-logo.img'
    subprocess.run([str(RESOURCE_TOOL), '--pack', f'--root={out}',
                    f'--image={img}', 'logo.bmp', 'logo_kernel.bmp'],
                   check=True, cwd=out, stdout=subprocess.DEVNULL)
    print(f'{img}: {os.path.getsize(img)} 字节')


if __name__ == '__main__':
    main()
