# xTS 必测项清单（= BSP 赛道开发路线图）

来源：`docs/refs/openvela_xts_test_cases.md`，openvela 社区维护团队 V1.0 / 2026-05-19。

## 怎么用这份清单

文档把用例分两类：

- **通用自测用例 —— 必测项**（下面一、二、三节）
- **品类自测用例 —— 按产品特性选测**（WiFi、蓝牙、音视频、GUI、OTA、文件系统压测……）

**必测项就是你的开发顺序。**它测什么，你就按顺序实现什么。每项都给了需要打开的 `CONFIG_*` 和在 nsh 里敲的命令，不用自己发明验收方式。

注意：**WiFi 属于选测**，不在及格线内。方案 B 的 `ai_agent` 联网是加分项，不是必需项。

## 一、系统内核（13 项）

内核跑起来后基本能过，是"移植成功"的第一个客观信号。

| # | 用例 | nsh 命令 |
|---|---|---|
| 1.1.1 | 系统内存管理 | `cmocka_mm_test` |
| 1.1.2 | 系统调度 | `cmocka_sched_test` |
| 1.1.3 | 系统调用 | `cmocka_syscall_test` |
| 1.1.4 | Kernel-ostest | |
| 1.1.5 | Kernel-getprime | |
| 1.1.6 | Kernel-mm 内存 | |
| 1.1.7 | Kernel-scanftest | |
| 1.1.8 | Kernel-C | |
| 1.1.9 | Kernel-Cxx | |
| 1.1.10 | Kernel-popen | |
| 1.1.11 | Kernel-pipe | |
| 1.1.12 | Kernel-md5 | |
| 1.1.13 | Kernel-C++ 功能 | |

公共前提配置：
```
CONFIG_TESTING_CMOCKA=y
CONFIG_TESTS_TESTSUITES=y
CONFIG_TESTS_TESTSUITES_STACKSIZE=16384
CONFIG_ARCH_SETJMP_H=y
CONFIG_BUILTIN=y
CONFIG_NSH_BUILTIN_APPS=y
CONFIG_SCHED_HAVE_PARENT=y
CONFIG_SCHED_LPWORK=y
```
各用例再加各自的 `CONFIG_CM_*_TEST=y`。

## 二、驱动 BSP（15 项）—— 这是主战场

| # | 用例 | 依赖的驱动 | 状态 |
|---|---|---|---|
| 1.3.1 | 烧写测试 | 启动链路 / 烧录流程 | ☐ |
| 1.3.2 | RAM 读写 | DDR + MMU | ☐ |
| 1.3.3 | RAM 读写性能 | 同上 | ☐ |
| 1.3.4 | RAM 随机读写 | 同上 | ☐ |
| 1.3.5 | Flash 功能 | eMMC / SPI Flash + MTD | ☐ |
| 1.3.6 | **GPIO 功能** | GPIO 驱动 | ☐ |
| 1.3.7 | **I2C / SPI 功能** | I2C、SPI 驱动 | ☐ |
| 1.3.10 | **UART 串口功能** | DW 8250 串口 | ☐ |
| 1.3.11 | UART 文件传输 | 串口 + 文件系统 | ☐ |
| 1.3.12 | RTC 时钟 | RTC 驱动 | ☐ |
| 1.3.13 | **Timer 定时器** | arch_alarm / 定时器 | ☐ |
| 1.3.14 | 时间一致性 | 同上 | ☐ |
| 1.3.15 | Watchdog | 看门狗驱动 | ☐ |
| 1.3.16 | RNG | 硬件随机数 | ☐ |
| 1.3.17 | Crypto | 加解密引擎 | ☐ |

已知的 cmocka 驱动测试命令：
`cmocka_driver_gpio` `cmocka_driver_uart` `cmocka_driver_rtc` `cmocka_driver_oneshot`
`cmocka_driver_watchdog` `cmocka_driver_pwm` `cmocka_driver_block` `cmocka_driver_audio`
`cmocka_driver_framebuffer` `cmocka_driver_touchpanel` `cmocka_driver_relay`

## 三、系统应用 + 性能 + 稳定性（7 项）

| # | 用例 | 说明 |
|---|---|---|
| 1.2.1 | Reboot 启动异常 | 反复重启不挂 |
| 1.2.2 | Cold boot 启动异常 | 冷启动不挂 |
| 1.2.3 | 系统 RAM 资源占用统计 | 需要出数据 |
| 1.2.4 | 系统 Flash 资源占用统计 | 需要出数据 |
| 2.1.3 | **Cold Boot 启动时间** | 性能指标，可做对比图 |
| 2.1.4 | **Reboot 启动时间** | 同上 |
| 3.1.1 | 12h 待机稳定性 | 需要预留一整天 |

## 四、选测项（方案 B 相关）

| 类别 | 数量 | 与方案 B 的关系 |
|---|---|---|
| WiFi（2.4G / 5G / wapi / 吞吐 / 兼容性） | 约 60 项 | `ai_agent` 联网前提。工作量大 |
| 蓝牙 BLE（广播 / 扫描 / 配对 / 共存） | 约 20 项 | 暂不涉及 |
| 文件系统（vela_fs_*） | 约 20 项 | 存配置和模型需要 |
| Audio / Video / GUI / LVGL / Framebuffer | 约 25 项 | 无屏方案暂不涉及 |
| OTA 全包升级 | 若干 | 暂不涉及 |
| NetApp（curl / ftpd / scp） | 若干 | 联网后顺带 |

## 里程碑映射

| 里程碑 | 对应 xTS |
|---|---|
| M0 编译产物 + 启动到 `__start` | — |
| M1 串口出字 | 1.3.10 |
| M2 起到 nsh | 一、系统内核 13 项全部可跑 |
| M3 定时器 + 中断 | 1.3.13、1.3.14 |
| M4 GPIO + I2C/SPI | 1.3.6、1.3.7 |
| M5 存储 + 文件系统 | 1.3.5、1.3.11 |
| M6 RTC / Watchdog / RNG | 1.3.12、1.3.15、1.3.16 |
| S1 网络 + ai_agent | 选测 WiFi / NetApp |
| 全程 | 1.2.x、2.1.x 启动时间与资源占用 |
