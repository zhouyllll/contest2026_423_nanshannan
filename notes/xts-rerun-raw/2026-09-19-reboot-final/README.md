# xTS 1.2.1 / 2.1.4 正式记录：NSH reboot ×10（2026-09-19，已拔 SD 卡）

- 镜像：a1b8537（TF 卡跳过 CMD1）+ 0b60ba6（U-Boot 倒计时 0.2s，uboot 分区已烧）
  + 8b8561c（触摸/TF/摄像头/界面后台初始化）。正常配置（CONFIG_SYSLOG_DEFAULT=y）。
- 方法同 `../2026-09-19-reboot/`：`capture.py 10`，串口 1500000 独占，t0 = 发出 reboot 回车。
- 原始数据：`reboot-NN.raw`、`reboot-NN-chunks.json`、`events.jsonl`。
  `events.jsonl` 的 `banner_subseq_s` 仍是初版算法（从 U-Boot 起匹配子序列），作废；下表为离线重算。

## 2.1.4 启动时间：PASS

| 判据 | 结果 | 均值 | 范围 | 阈值 |
|---|---|---|---|---|
| 横幅 `NuttShell (NSH)`（openvela 输出内、跨度 ≤ 400 字节的子序列） | 10/10 | **2.68 s** | 2.66–2.70 | ≤ 6 s |
| 首个 `nsh>` | 10/10 | **2.68 s** | 2.66–2.70 | ≤ 6 s |

对比同日早些时候（插卡、旧 U-Boot、同步初始化）：4.47 s。

## 1.2.1 启动异常：剩 3 类，均来自 bootloader，openvela 侧 0 条

| 行 | 次数 | 来源 |
|---|---|---|
| `spl: mmc init failed with error: -123`（-ENOMEDIUM） | 10/10 | 厂商 SPL 按启动顺序先探 MMC2（SD 槽），无卡 |
| `failed to display uboot logo` | 10/10 | resource 里没有 logo.bmp |
| `VP1 fail to load kernel logo` | 10/10 | 同上，logo_kernel.bmp |

openvela 侧：`SD 卡: 未插卡，跳过 (-19)` 为 INFO；原 `mmcsd ... 00008101 failed: -110` 已消失。
