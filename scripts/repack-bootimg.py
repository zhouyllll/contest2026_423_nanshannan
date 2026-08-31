#!/usr/bin/env python3
"""把 Android boot.img 里的 kernel 换成指定文件（通常是 nuttx.bin），其余段原样保留。

用途：KICKPI-K7 的出厂 U-Boot 静默、拿不到控制台，但它的 bootcmd 第一步是
      boot_android，会读 boot 分区的 boot.img、取出 kernel 段并 booti。
      openvela 的 nuttx.bin 本身就是合法的 ARM64 Linux Image（偏移 0x38 魔数
      ARMd），因此把它塞进 kernel 段即可被加载，无需 U-Boot 控制台。

保留原 header 的 page_size / 各 addr / cmdline / os_version，只改 kernel_size，
并按 page_size 重新排布后续各段（ramdisk / second / recovery_dtbo / dtb）。

  ./repack-bootimg.py <原boot.img> <新kernel> <输出boot.img>
"""
import struct, sys, pathlib, hashlib

HDR_MAGIC = b'ANDROID!'


def pad(n, page):
    return (n + page - 1) // page * page


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 1
    src, newk, dst = map(pathlib.Path, sys.argv[1:4])
    d = src.read_bytes()
    if d[:8] != HDR_MAGIC:
        print(f"✗ {src} 不是 Android boot.img（魔数 {d[:8]!r}）"); return 1

    (ksz, kaddr, rsz, raddr, ssz, saddr,
     tags, page, hver, osver) = struct.unpack('<10I', d[8:48])
    if hver > 2:
        print(f"✗ 只支持 header v0-v2，本镜像是 v{hver}"); return 1

    rec_sz = rec_off = hdr_sz = dtb_sz = dtb_addr = 0
    if hver >= 1:
        rec_sz, rec_off, hdr_sz = struct.unpack('<IQI', d[1632:1648])
    if hver >= 2:
        dtb_sz, dtb_addr = struct.unpack('<IQ', d[1648:1660])

    # 按 page 对齐依次切出各段
    off = page
    def take(size):
        nonlocal off
        seg = d[off:off + size]
        off += pad(size, page)
        return seg
    kernel, ramdisk, second = take(ksz), take(rsz), take(ssz)
    recovery_dtbo = take(rec_sz) if hver >= 1 else b''
    dtb = take(dtb_sz) if hver >= 2 else b''

    knew = newk.read_bytes()
    if knew[0x38:0x3c] != b'ARM\x64':
        print(f"⚠ {newk} 偏移 0x38 不是 ARM64 Image 魔数（读到 {knew[0x38:0x3c]!r}），"
              f"U-Boot 的 booti 可能拒绝加载")

    # 重建 header：改 kernel_size，并重算 id[] 里的 SHA1
    #
    # ★ Rockchip 的 U-Boot boot_android 会校验这个 SHA1，不更新就会报
    #     Hash from header: 0x...  /  Hash real: 0x...
    #     Failed to load android image
    #   摘要内容 = 各段数据与其长度(4B LE)依次拼接，段的顺序按 header 版本：
    #     v0: kernel, ramdisk, second
    #     v1: + recovery_dtbo
    #     v2: + dtb
    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(knew))

    h = hashlib.sha1()
    order = [(knew, len(knew)), (ramdisk, rsz), (second, ssz)]
    if hver >= 1:
        order.append((recovery_dtbo, rec_sz))
    if hver >= 2:
        order.append((dtb, dtb_sz))
    for seg, size in order:
        h.update(seg)
        h.update(struct.pack('<I', size))
    hdr[576:576 + 20] = h.digest()
    hdr[576 + 20:576 + 32] = b'\0' * 12

    out = bytes(hdr)
    for seg in (knew, ramdisk, second) + ((recovery_dtbo,) if hver >= 1 else ()) \
                                       + ((dtb,) if hver >= 2 else ()):
        out += seg + b'\0' * (pad(len(seg), page) - len(seg))

    dst.write_bytes(out)
    print(f"✓ {dst}")
    print(f"    header v{hver}, page_size {page}")
    print(f"    kernel   {ksz:>10} -> {len(knew):<10} ({newk.name})")
    print(f"    ramdisk  {rsz:>10}     保留")
    print(f"    second   {ssz:>10}     保留")
    if hver >= 2:
        print(f"    dtb      {dtb_sz:>10}     保留")
    print(f"    总大小   {len(d):>10} -> {len(out)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
