# xTS 1.2.1 / 2.1.4 正式记录（最终）：NSH reboot ×10，带开机 logo，已拔 SD 卡

替代同日 `../2026-09-19-reboot-final/`（那一轮 U-Boot 还报 logo 缺失）。

- 镜像：a1b8537 + 8b8561c 的 openvela；U-Boot = 0009 + 0010（倒计时 0.2s）+ 0011（resource 从 dtbo 分区读）；
  dtbo 分区（LBA 40960）= `scripts/gen-boot-logo.py` 生成的 resource 镜像（logo.bmp / logo_kernel.bmp）。
- 方法：`capture.py 10`，串口 1500000 独占，t0 = 发出 reboot 回车；原始数据 `reboot-NN.raw` / `-chunks.json` / `events.jsonl`。
  `events.jsonl` 里的 `banner_subseq_s` 为初版算法，作废，下表为离线重算（openvela 输出内、跨度 ≤ 400 字节的子序列）。

## 2.1.4 启动时间：PASS

| 判据 | 结果 | 均值 | 范围 | 阈值 |
|---|---|---|---|---|
| 横幅 `NuttShell (NSH)` | 10/10 | **2.76 s** | 2.74–2.77 | ≤ 6 s |
| 首个 `nsh>` | 10/10 | **2.76 s** | 2.74–2.77 | ≤ 6 s |

logo 解码显示比无 logo 多约 0.08 s（无 logo 一轮 2.68 s）。当日初测为 4.47 s。

## 1.2.1 启动异常：openvela 0 条，U-Boot 0 条，SPL 1 条

| 行 | 次数 | 说明 |
|---|---|---|
| `spl: mmc init failed with error: -123`（-ENOMEDIUM） | 10/10 | 厂商 SPL（Jun 2025 预编译，idblock 内）按启动顺序先探 SD 槽，无卡即打印；随后从 eMMC 正常启动。改它需重编 SPL 并重写 idblock，风险与收益不相称，未改。 |

串口在 1.5 Mbaud 下有主机侧丢字，个别行被截断（同一行在其他次完整出现），不影响计数。
