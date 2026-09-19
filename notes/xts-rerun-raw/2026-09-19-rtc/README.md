# xTS 1.3.12 RTC：cmocka_driver_rtc 3/3 PASSED（2026-09-19，连跑 3 次）

- 镜像：正常配置 + `board/kickpi-k7/configs/xts-driver-tests.config`（RTC_ALARM、RTC_PERIODIC、RTC_IOCTL、
  SIG_EVTHREAD、TESTING_NIST_STS）；nuttx `drivers/timers/hym8563.c` 带 wdog 闹钟（nuttx ed8f69ad，
  已导出到 `bsp/nuttx-drivers.patch`）。`uname`：`3b51eaa1-dirty Sep 19 2026 17:25:46`。
- 原文配置里的 `CONFIG_DRIVERS_RTC`、`CONFIG_CMOCKA` 本树不存在，对应已开的 `RTC_DRIVER`、`TESTING_CMOCKA`。
- 经 telnet NSH 执行 `cmocka_driver_rtc`；cmocka 结果走 syslog，所以证据在串口
  `serial-during-cmocka_driver_rtc{,-2,-3}.raw`（1.5 Mbaud 主机侧有丢字，如 `[ RUN     [CPU0]`，
  三份互相印证；第 3 份完整）。

| 次 | drivertest_rtc_api | drivertest_rtc_alarm | drivertest_rtc_periodic | 汇总 |
|---|---|---|---|---|
| 1 | OK | OK | OK | PASSED 3 |
| 2 | OK | OK | OK | PASSED 3 |
| 3 | OK | OK | OK | 3 test(s) run（"PASSED" 字样被丢字截断，各项均 OK） |

## 闹钟怎么过的 ±10ms

HYM8563 闹钟寄存器只到分钟、走时只到秒；测试要求 5s 闹钟误差 ±10ms（`DEFAULT_TIME_OUT`、
`RTC_DEFAULT_DEVIATION`）。驱动在 settime 放开 STOP 时记"RTC 秒 ↔ 系统节拍"锚点，闹钟换算成节拍用
`wd_start_abstick`，周期唤醒用 `wd_start_next`。说明见 hym8563.c 文件头；代价是不能断电唤醒
（本板 INT 脚未接 SoC）。

测试把 RTC 设成 2000-01-01；测后已 `date -s` 设为当前 UTC（测前板上时间本就不准：2026-03-02）。
