# xTS 3.1.1 12h 待机（KASAN + showinfo）：PASS

- 时间：2026-09-19 19:40 → 2026-09-20 07:32 +08，连续抓串口 43800s（12h10m）。
- 镜像：e7ddde1 + `board/kickpi-k7/configs/xts-kasan-longrun.config`（MM_KASAN generic、SYSTEM_RESMONITOR；
  为放进 FIT 区去掉 ostest/scanftest/cxxtest）。`uname`：`ed8f69ad-dirty Sep 19 2026 19:35:37`。
  nuttx.bin 4,050,944 B，FIT 4,055,552 B（trust 区 8192 扇区，余 271）。
  KASAN 生效旁证：ELF 有 27 个 `__asan_*`/`kasan_*` 符号；堆 63,541,248 → 62,072,624（影子内存）。
- 负载：启动后 telnet 执行 `showinfo -i 60 &`（PID 43），每 60s 打一次内存/CPU；其余待机，网口接着但无操作。
- 原始数据：`serial.raw`（106,393 B）、`chunks.jsonl`（每块到达时刻）、`summary.json`。

## 结果

| 判据 | 结果 |
|---|---|
| 重启（`U-Boot 2017` 横幅再现） | **0 次** |
| kasan / panic / assert / crash / ESR= / error / fail 等关键词行 | **0 行** |
| showinfo 记录 | 713 条，间隔 59.7–63.5s，**无 >75s 的空档**（卡死会表现为空档） |
| 堆 used | 首 10,123,520 → 末 10,110,448 B，全程波动 13,072 B，前后半段均值差 −198 B（无泄漏趋势） |

结论：12h 待机**无报错、无崩溃、无重启** → PASS。

注：串口 1.5 Mbaud 有主机侧丢字，713 条记录里能严格解析出数值的 132 条（其余行被丢字粘连，
如 `largest` 少一位）；上表的内存统计只用这 132 条。丢字不影响"是否出现异常关键词/是否重启"的判定
——  那要求的是**出现**某些字节，丢字只会使漏报，而结果是 0 行，且 713 条心跳证明系统一直在跑。
