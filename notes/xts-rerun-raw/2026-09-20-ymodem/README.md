# xTS 1.3.11 UART 文件传输（ymodem）：接收方向通过，发送方向受串口丢字限制（2026-09-20）

镜像：`ccae5aa`（含 GPIO 读回修复）。主机用 lrzsz 的 `sb`/`rb`，板端用 `rb`/`sz`，
链路是控制台 `/dev/ttyUSB0`（CH340，1,500,000 baud）。

## 主机 → 板（接收方向）：PASS

    主机: stty -F /dev/ttyUSB0 1500000 raw -echo
          printf 'rb -f /tmp\r' > /dev/ttyUSB0
          sb --ymodem /tmp/xts-ymodem.bin < /dev/ttyUSB0 > /dev/ttyUSB0

主机侧 `host-sb.log`：`Bytes Sent: 4096  BPS:36467` / `Transfer complete`。
板端 `md5_test -f /tmp/xts-ymodem.bin`（`board-md5.serial.raw`）：
`06ef62766a3f0f7ffe8e48542…`，与主机 `md5sum` 的
`06ef62766a3f0f7ffe8e48542a8f83bd` 一致（串口把结尾几位丢了，前 25 位逐字吻合）。

## 板 → 主机（发送方向）：1.5 Mbaud 下失败

    板端: sz /tmp/xts-ymodem.bin
    主机: rb --ymodem < /dev/ttyUSB0 > /dev/ttyUSB0

`host-rb.log`：`Transfer incomplete`，未落地文件。

原因是**主机侧收字节丢失**，不是协议或驱动问题：本仓 `docs/xts-checklist.md`
记录过同一链路的定量测量（板子 `cat` 4096 字节全零文件，主机收到的字节数）：

| 波特率 | 第 1 次 | 第 2 次 | 第 3 次 |
|---|---|---|---|
| 1500000 | 3566 | 3422 | 3817 |
| **115200** | **4096** | **4096** | **4096** |

即 1.5 Mbaud 下丢 7~16%，115200 下零丢失；接收方向没事是因为丢的是
**板→主机**那一路。历史记录中该项在 115200 下收发双向均通过。

## 今天尝试降速未成功（记录过程，避免下次重走）

加了 `board/kickpi-k7/configs/xts-uart115200.config`（`CONFIG_16550_UART0_BAUD=115200`），
重编并烧录后板子跑的是新镜像（`uname` 时间戳 19:46:39），但控制台**仍是 1.5 Mbaud**
（115200 下收不到任何字节，1.5 M 下正常出 `nsh>`）——串口驱动沿用了 U-Boot 设好的
分频，没有按这个配置重设。要让它生效需要再查 `chip/rk3576` 的串口初始化，
截止前未继续追。
