# amp-dual：和 Linux 共存的那一份

和 `../amp/` 只差两行，但这两行决定了它**不能单独启动**：

| 项 | amp | amp-dual | 为什么 |
|---|---|---|---|
| `CONFIG_RAM_START` | `0x40480000` | `0x4a400000` | 0x40480000 是 Linux 的地盘；共存时 openvela 必须搬进 DTS 里 `no-map` 的保留区 |
| `CONFIG_ARM64_GICV2_SHARED_DIST` | 关 | 开 | 共存时 GIC distributor 归 Linux；单跑时归 openvela 自己，开着反而不初始化中断 |

**为什么留两份而不是改一份**

`amp` 那份能用 `flash.sh --raw` 直接烧进去单独跑，`ampctl selftest` /
`ampctl status` 随时可用 —— 传输层出问题时，它是唯一一个不需要对端就能
给出答案的环境。把它改掉，排查就只剩"双 OS 一起起不来"这一种现象，
而那是五六个原因叠在一起的结果。

两份的差异只有上面两行，`diff` 一眼看得完，不构成维护负担。

**怎么编**

```bash
cd nuttx
./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp-dual
make -j$(nproc)
```

产物 `nuttx.bin` 交给 `amp/fit/` 下的打包脚本。**不要**用 `flash.sh --raw`
烧它 —— 那条路按 ARM64 Image 协议搬到 `DRAM 基址 + text_offset`，
落点是 0x40480000，和这份的链接地址对不上，表现是完全没有输出。
