#!/usr/bin/env python3
"""从原厂 boot.img 里提取 dtb 并查询节点 —— 板级参数的权威出处。

规格书和网上的资料都可能与手上这块板对不上；原厂固件里的 dtb 是
这块板实际在用的配置，查它最可靠，也比翻 PDF 快。

用法：
    ./dtb-query.py extract <boot.img> <out.dtb>
    ./dtb-query.py find    <dtb> <关键字>       # 按节点名/compatible 搜
    ./dtb-query.py node    <dtb> <节点路径前缀>  # 打印节点全部属性

例：
    ./dtb-query.py extract ~/boot-orig.img /tmp/board.dtb
    ./dtb-query.py find /tmp/board.dtb hym8563
    ./dtb-query.py node /tmp/board.dtb /i2c@2ac50000
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dtb import DTB


def extract(img, out):
    """从 Android boot.img（header v2）里取出 dtb 段。"""
    d = open(img, 'rb').read()
    pg = struct.unpack('<I', d[36:40])[0]
    ksz = struct.unpack('<I', d[8:12])[0]
    rsz = struct.unpack('<I', d[16:20])[0]
    ssz = struct.unpack('<I', d[24:28])[0]
    recsz = struct.unpack('<I', d[1632:1636])[0]
    dtbsz = struct.unpack('<I', d[1648:1652])[0]
    pages = lambda n: (n + pg - 1) // pg
    off = pg * (1 + pages(ksz) + pages(rsz) + pages(ssz) + pages(recsz))
    dtb = d[off:off + dtbsz]
    if dtb[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit("dtb magic 不对（读到 %s），boot.img 布局可能不同" % dtb[:4].hex())
    open(out, 'wb').write(dtb)
    print("✓ %s  (%d 字节，偏移 0x%x)" % (out, dtbsz, off))


def fmt(k, v):
    if k in ('compatible', 'status', 'clock-names', 'pinctrl-names',
             'clock-output-names', 'reset-names', 'interrupt-names'):
        return str(DTB.strs(v))
    if len(v) % 4 == 0 and 0 < len(v) <= 64:
        return str(DTB.u32s(v))
    return '(%d 字节)' % len(v)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'extract':
        extract(sys.argv[2], sys.argv[3])
        return

    t = DTB(sys.argv[2])
    arg = sys.argv[3] if len(sys.argv) > 3 else ''

    if cmd == 'find':
        key = arg.lower().encode()
        for path, props in t.walk():
            if key in path.lower().encode() or key in props.get('compatible', b'').lower():
                comp = DTB.strs(props.get('compatible', b''))
                st = DTB.strs(props.get('status', b''))
                print("%-46s %-28s %s" % (path, comp[0] if comp else '',
                                          st[0] if st else ''))
    elif cmd == 'node':
        for path, props in t.walk():
            if path.startswith(arg):
                print("=== %s ===" % path)
                for k in sorted(props):
                    print("  %-20s %s" % (k, fmt(k, props[k])))
    else:
        sys.exit(__doc__)


main()
