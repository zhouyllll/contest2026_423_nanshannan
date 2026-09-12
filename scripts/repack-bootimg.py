#!/usr/bin/env python3
"""把 Android boot.img 里的 kernel 换成指定文件（通常是 nuttx.bin），其余段原样保留。

用途：KICKPI-K7 的出厂 U-Boot 静默、拿不到控制台，但它的 bootcmd 第一步是
      boot_android，会读 boot 分区的 boot.img、取出 kernel 段并 booti。
      openvela 的 nuttx.bin 本身就是合法的 ARM64 Linux Image（偏移 0x38 魔数
      ARMd），因此把它塞进 kernel 段即可被加载，无需 U-Boot 控制台。

保留原 header 的 page_size / 各 addr / cmdline / os_version，只改 kernel_size，
并按 page_size 重新排布后续各段（ramdisk / second / recovery_dtbo / dtb）。

  ./repack-bootimg.py <原boot.img> <新kernel> <输出boot.img>
                      [--dtb <file>] [--cmdline <str>]

AMP 阶段多出两个可选参数。换 kernel 的同时必须能换 dtb：AMP 用的
rk3576-kickpi-k7-amp.dtb 只保留 A72 的四个 cpu 节点，而原厂那份八个核
全在 —— Linux 会按它去 PSCI 拉起 A53，而 A53 上正跑着 openvela。
cmdline 也要能换，原厂那句里的 console=ttyFIQ0 在 AMP 下是关掉的。
"""
import struct, sys, pathlib, hashlib

HDR_MAGIC = b'ANDROID!'


def pad(n, page):
    return (n + page - 1) // page * page


def main():
    argv = sys.argv[1:]
    newdtb = newcmdline = None
    while len(argv) > 3:
        if argv[3] == '--dtb' and len(argv) > 4:
            newdtb = pathlib.Path(argv[4]); del argv[3:5]
        elif argv[3] == '--cmdline' and len(argv) > 4:
            newcmdline = argv[4]; del argv[3:5]
        else:
            print(__doc__); return 1
    if len(argv) != 3:
        print(__doc__); return 1
    src, newk, dst = map(pathlib.Path, argv)
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

    if newdtb is not None:
        dtb = newdtb.read_bytes()
        if dtb[:4] != b'\xd0\x0d\xfe\xed':
            print(f"✗ {newdtb} 不是 FDT（魔数 {dtb[:4]!r}）"); return 1

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
    if hver >= 2 and newdtb is not None:
        struct.pack_into('<I', hdr, 1648, len(dtb))
    if newcmdline is not None:
        # cmdline 在 header 偏移 64，512 字节，NUL 结尾
        cl = newcmdline.encode()
        if len(cl) >= 512:
            print("✗ cmdline 超过 511 字节"); return 1
        hdr[64:64 + 512] = cl + b'\0' * (512 - len(cl))

    h = hashlib.sha1()
    order = [(knew, len(knew)), (ramdisk, rsz), (second, ssz)]
    if hver >= 1:
        order.append((recovery_dtbo, rec_sz))
    if hver >= 2:
        order.append((dtb, len(dtb)))
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
        if newdtb is not None:
            print(f"    dtb      {dtb_sz:>10} -> {len(dtb):<10} ({newdtb.name})")
        else:
            print(f"    dtb      {dtb_sz:>10}     保留")
    if newcmdline is not None:
        print(f"    cmdline  已替换: {newcmdline}")
    print(f"    总大小   {len(d):>10} -> {len(out)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
