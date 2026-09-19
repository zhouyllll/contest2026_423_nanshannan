# xTS 3.1.1 12h 待机（KASAN + showinfo）：进行中

- 开始：2026-09-19 19:40 +08；串口抓 43800s（12h10m），预计 2026-09-20 07:51 结束。
- 镜像：e7ddde1 + `board/kickpi-k7/configs/xts-kasan-longrun.config`（MM_KASAN generic、SYSTEM_RESMONITOR；
  为放进 FIT 区去掉 ostest/scanftest/cxxtest），`uname`：`ed8f69ad-dirty Sep 19 2026 19:35:37`。
  nuttx.bin 4,050,944 B，FIT 4,055,552 B（trust 区余 271 扇区）。KASAN 生效的旁证：ELF 含 27 个 `__asan_*`/`kasan_*`
  符号；堆总量 63,541,248 → 62,072,624（影子内存）。
- 启动后经 telnet 执行 `showinfo -i 60 &`（PID 43），每 60s 往 syslog（串口）打内存/CPU。
- 抓取：`capture.py`（`serial.raw` 原始字节、`chunks.jsonl` 到达时刻、`summary.json` 每 10 分钟刷新：
  U-Boot 横幅次数 = 重启次数、kasan/panic/assert/error 等关键词行）。
- 板上网口保持连接（启动 showinfo 用），未做其它网络操作。
