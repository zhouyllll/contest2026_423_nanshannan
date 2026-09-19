# xTS 1.2.1 / 2.1.4：NSH reboot ×10（2026-09-19）

- 镜像：源码 51ec1b8（工作区另有用户未提交的 amp-dual defconfig 蓝牙改动），正常配置
  （`CONFIG_SYSLOG_DEFAULT=y`），15:50 刷入的 FIT。
- 方法：`capture.py 10`，/dev/ttyUSB0 1500000 独占；在串口敲 `reboot`+回车为 t0，每次抓 20 s 原始字节。
  原始数据：`reboot-NN.raw`、`reboot-NN-chunks.json`（每块到达时刻），汇总在 `events.jsonl`。
- 板上插着 SD 卡（GPT 损坏），是本次多数错误行的来源。

## 2.1.4 启动时间

| 判据 | 10 次结果 | 均值 | 阈值 |
|---|---|---|---|
| 首次收到 `nsh>`（openvela 日志出现后每 0.3 s 发回车） | 10/10 | **4.47 s**（4.38–4.63） | ≤ 6 s |
| 横幅 `NuttShell (NSH)` 子序列（跨度 ≤ 400 字节，只在 openvela 输出里找） | 6/10 找到 | 4.17 s（4.15–4.20） | ≤ 6 s |

- 横幅有 4 次没找到：syslog 通过 up_putc 直接写 UART，横幅与另一核的日志逐字节交错
  （如 `N0u]tt SGhMelAlC 0(:N SH2)`）。交错本身不丢字，丢字发生在主机端 CH340
  （1.5 Mbaud），U-Boot 单核阶段的行也被截断，说明是主机丢字而不是板子漏发。
- `events.jsonl` 里的 `banner_subseq_s≈0.51` 是脚本初版的错误值（从 U-Boot 开头就开始
  匹配子序列，把 U-Boot 文本里的字母凑成了横幅），**作废**，以上表离线重算的值为准。
- 结论：按 `nsh>` 判据 **PASS**（10/10，全部 ≤ 4.63 s）；按横幅字符串的严格判据，因串口丢字只能证明 6/10。

## 1.2.1 启动异常（U-Boot 之后的错误行）

| 行 | 来源 | 处理 |
|---|---|---|
| `part_get_info_efi: *** ERROR: Invalid (Backup) GPT` / `spl: partition error` | SD 卡 GPT 损坏（eMMC GPT 与备份一致） | 拔 SD 卡或修复其 GPT 后复测 |
| `mmcsd_cmdpoll ... cmd 00008101 failed: -110`（10 次里约 8 次） | openvela 探测 SD 卡的 CMD1 超时 | 同上，拔卡复测 |
| `failed to display uboot logo` / `VP1 fail to load kernel logo` | U-Boot 找不到 logo 分区 | 属于 U-Boot 提示，不影响启动；严格判读仍算异常行 |

结论：**未通过**，需要拔掉 SD 卡后复测，logo 两行另行说明或在 U-Boot 侧处理。
