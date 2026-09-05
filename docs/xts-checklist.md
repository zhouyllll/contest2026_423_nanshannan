# xTS 必测项清单（= BSP 赛道开发路线图）

来源：`docs/refs/openvela_xts_test_cases.md`，openvela 社区维护团队 V1.0 / 2026-05-19。

## 怎么用这份清单

文档把用例分两类：

- **通用自测用例 —— 必测项**（下面一、二、三节）
- **品类自测用例 —— 按产品特性选测**（WiFi、蓝牙、音视频、GUI、OTA、文件系统压测……）

**必测项就是你的开发顺序。**它测什么，你就按顺序实现什么。每项都给了需要打开的 `CONFIG_*` 和在 nsh 里敲的命令，不用自己发明验收方式。

注意：**WiFi 属于选测**，不在及格线内。方案 B 的 `ai_agent` 联网是加分项，不是必需项。

## 当前进度总表（截至 2026-09-04）

必测 35 项：**通过 27、部分通过 6、不可行 0、待做 1、需对端设备 1**。
（2026-09-05 实测更新：一次跑完 1.1.8 / 1.1.12 / 1.3.2 / 1.3.3 / 1.3.4 /
1.3.5 / 1.3.17，另加 1.3.13 用独立定时器重新拿回。）

"已编入待实测"= 配置已开、程序已进 `builtin_list.h`、编译通过，只差在
板子上敲一遍。命令见文末《一次烧写要跑完的命令清单》。

| # | 用例 | 状态 | 依据 / 卡在哪 |
|---|---|---|---|
| 1.1.1 | 系统内存管理 | ✅ | `cmocka_mm_test` 8/8 |
| 1.1.2 | 系统调度 | ✅ | `cmocka_sched_test` 16/16（9 pthread + 7 task） |
| 1.1.3 | 系统调用 | ✅ | **74/74**。此前 68/74 的判断（"tmpfs 不支持 symlink/truncate"）是错的，根因是 `CONFIG_NAME_MAX=32`（见下）|
| 1.1.4 | Kernel-ostest | ✅ | `ostest` 24 个套件 |
| 1.1.5 | Kernel-getprime | ✅ | |
| 1.1.6 | Kernel-mm | ✅ | TEST COMPLETE |
| 1.1.7 | Kernel-scanftest | ⚠️ | **146 通过 / 18 失败**（旧记录 85/11 已过时）。失败集中在 #10、#28-31、#35-38、#49 等格式说明符边界用例，属 libc 一致性，非移植缺陷 |
| 1.1.8 | Kernel-C | ✅ | `hello` 打印 Hello, World!! |
| 1.1.9 | Kernel-Cxx | ✅ | `helloxx` 三种实例（动态/栈上/静态构造）全部打印。用**工具链自带**的 libstdc++/libsupc++，不下载源码 |
| 1.1.10 | Kernel-popen | ✅ | `popen` 实测：`popen("help")` 的输出经管道回来、`pclose()` 正常。之前"libc 无 popen"的判断是错的 —— 实现在 `apps/system/popen/popen.c` |
| 1.1.11 | Kernel-pipe | ✅ | PASSED（含重定向） |
| 1.1.12 | Kernel-md5 | ✅ | `md5_test -c 100` 全部同值 `01fbd2fa33f6ea48e11960f47c9b622b`（串口丢 1 行，收到 99 行） |
| 1.1.13 | Kernel-C++ 功能 | ◐ | `cxxtest` 的 `std::vector`/`std::map`/RTTI 全过，**卡在 `Test Exception`**：arm64 上栈展开表没被注册（见下） |
| 1.3.1 | 烧写测试 | ✅ | `scripts/flash.sh` 一条命令，串口触发 loader，**无需按 recovery** |
| 1.3.2 | RAM 读写 | ✅ | `fstest -n 10 -m /tmp` → OK: 20, FAILED: 0 |
| 1.3.3 | RAM 读写性能 | ✅ | `ramtest -w -s 1048576` 各阶段（marching 1/0、pattern、address-in-address）无报错 |
| 1.3.4 | RAM 随机读写 | ✅ | `mkrd -m 10 -s 512 2048` + `cmocka_driver_block -m /dev/ram10` → 3/3 OK |
| 1.3.5 | Flash 功能 | ✅ | `mkfatfs -F 32 /dev/mmcsd1` → `mount -t vfat` → `fstest -n 10 -m /mnt` **OK: 20, FAILED: 0**；文件读写往返也正确。卡是 16GB，必须 `-F 32`（自动只试 FAT12/16）。**不能对 eMMC 跑** |
| 1.3.6 | GPIO 功能 | ✅ | **4/4**（bool / loop / rw / interrupt）。中断子项要把**排针第 5 脚（GPIO4_A4）↔ 第 7 脚（GPIO4_A6）**用杜邦线短接。此前"挂死"的真正原因是输入脚上永远等不到边沿 —— 原先选的 GPIO4_A7/B3 没引到连接器，线根本插不上 |
| 1.3.7 | I2C / SPI 功能 | ◐ | **xTS 用例需对端板子**，单板不可能通过（见下文）。SPI 环回自检的第三层要短接 **第 10 脚（MOSI/GPIO4_B1）↔ 第 12 脚（MISO/GPIO4_B2）**。I2C 由板上真实器件（RTC@0x51、触摸@0x38）证实 |
| 1.3.10 | UART 串口功能 | ✅ | `cmocka_driver_uart` 1/1 |
| 1.3.11 | UART 文件传输 | ◐ | **接收方向已完整验证**：主机 `sb --ymodem` → 板端 `rb -f /mnt`，`Transfer complete`，板上 `cat` 出的内容与源文件逐字节一致。发送方向（板端 `sb` → 主机 `rb`）能收到正确的头块（文件名对），数据块被 lrzsz 拒收，未定位 |
| 1.3.12 | RTC 时钟 | ✅ | HYM8563 读写 + 掉电后时间保持 |
| 1.3.13 | Timer 定时器 | ✅ | `cmocka_driver_oneshot` OK。改用**独立的** RK3576 TIMER（CH0 闹钟 + CH1 计数），不再碰调度器的 ARM 通用定时器；同时 `sleep 3/8` 实测 3.3/8.4 s、静置 120 s 零自发复位 |
| 1.3.14 | 时间一致性 | ◐ | `date -s` / `date` 实测正常（设 15:03:47，随后读到 15:03:50 / :54 / 04:13，与间隔一致）。已对时，24h 后复读比对漂移 |
| 1.3.15 | Watchdog | ◐ | 时基修好后复测：确实触发复位并恢复，重启后能读到 `soc warm boot, reset status: 0x1050`。但 cmocka 那 4 个子项**跑不完** —— DW 看门狗使能后软件关不掉，第一个子项就把板子复位了（见下） |
| 1.3.16 | RNG | ✅ | 自检两批不同；`hexdump /dev/random` 读数随机 |
| 1.3.17 | Crypto | ✅ | 八项全过零失败：des3cbc / aescbc / aesctr / aesxts / hmac(md5,sha1,sha256) / hash(md5,sha1,sha256,sha512) / crc32×4 / ecdsa(P-256 生成·签名·验证) |
| 1.2.1 | Reboot 启动异常 | ✅ | 10/10 |
| 1.2.2 | Cold boot 启动异常 | ✅ | 5/5（口径见 `notes/xts-stability.md`） |
| 1.2.3 | 系统 RAM 占用 | ✅ | 3.5MB / 63MB，5.6% |
| 1.2.4 | 系统 Flash 占用 | ✅ | `nuttx.bin` 1.20MB（编入八项 xTS 用例后，原 1.12MB） |
| 2.1.3 | Cold Boot 启动时间 | ✅ | 平均 4795 ms |
| 2.1.4 | Reboot 启动时间 | ✅ | 平均 4744 ms |
| 3.1.1 | 12h 待机稳定性 | ☐ | 需预留一整天 |

### ★ 1.3.7 的 xTS 用例在单板上不可能通过

`cmocka_driver_spidev_master`、`cmocka_driver_i2cdev_master`、
`cmocka_driver_i2c_read/write` 全部是**主机对从机**的成对用例：主机发传输
长度、流式发数据、再发 CRC32，由**另一块板子上的从机**回读校验。
`i2c_read` 的参数干脆就是 `<master 路径> <slave 路径>`。

单板上没有对端，这几个用例不是"失败"，是**跑不了**。实测还会把板子挂死，
需要断电重插，代价不低 —— 不要再跑。

替代的验收方式（已做）：

- **SPI**：`spi_selftest`（`app/spi_selftest/`）。它把问题拆成三层单独回答：
  节点能否打开 → 时钟是否真在跑 → 数据是否正确。
  第二层用**计时**判定，因为驱动带停滞保护，控制器不产生时钟时同样会
  正常返回一片 0，光看返回值分不出来。实测 16 字节 @1MHz 用 145us、
  @100kHz 用 1308us（理论 128/1280us），比值 9.02 —— 时钟确实在跑。
  第三层需要把 MOSI(GPIO4_B1) 与 MISO(GPIO4_B2) 短接。
- **I2C**：板上真实器件即是验收 —— RTC HYM8563@0x51 读写与掉电保持、
  触摸 FT8756@0x38 的芯片 ID 与坐标，都已实测通过。


## 一、系统内核（13 项）

内核跑起来后基本能过，是"移植成功"的第一个客观信号。

| # | 用例 | nsh 命令 | 板上结果 |
|---|---|---|---|
| 1.1.1 | 系统内存管理 | `cmocka_mm_test` | ✅ 8/8 |
| 1.1.2 | 系统调度 | `cmocka_sched_test` | ✅ 16/16（9 pthread + 7 task） |
| 1.1.3 | 系统调用 | `cmocka_syscall_test` | ✅ 74/74 |
| 1.1.4 | Kernel-ostest | `ostest` | ✅ 24 个套件 |
| 1.1.5 | Kernel-getprime | `getprime` | ✅ |
| 1.1.6 | Kernel-mm 内存 | `mm` | ✅ TEST COMPLETE |
| 1.1.7 | Kernel-scanftest | `scanftest` | ⚠️ 146 通过 / 18 失败 |
| 1.1.8 | Kernel-C | `hello` | ◆ 已编入，待实测 |
| 1.1.9 | Kernel-Cxx | `helloxx` | ✅ 工具链自带 libstdc++ |
| 1.1.10 | Kernel-popen | `popen` | ✅ 实现在 apps/system/popen |
| 1.1.11 | Kernel-pipe | `pipe` | ✅ PASSED（含重定向） |
| 1.1.12 | Kernel-md5 | `md5_test -f /tmp/1.txt -c 100` | ◆ 已编入，待实测 |
| 1.1.13 | Kernel-C++ 功能 | `cxxtest` | ◐ 异常之外都过 |

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

实际打开时还踩到的坑（都属于"Kconfig 有 default、`.config` 里却没有，
代码走 `#ifndef` 兜底或直接编不过"这一类，用 `scripts/check-config.py` 查）：

- `TESTING_CMOCKA` 依赖 `LIBC_EXECFUNCS` 和 `LIBC_REGEX`，两者都要显式开
- cmocka 自身要 `TESTING_CMOCKA_PROGNAME/PRIORITY/STACKSIZE`
- `TESTS_TESTSUITES_PRIORITY` 不给会让生成的 `builtin_list.h` 里优先级
  字段为空，报 `expected expression before ','`
- `SCHED_LPWORK` 要连带 `SCHED_LPNTHREADS/LPWORKPRIORITY/LPWORKPRIOMAX/
  LPWORKSTACKSIZE/LPWORKSTACKSECTION`
- `CM_SYSCALL_TEST` 硬依赖 `PIPES && FS_TMPFS && FS_LINKS`（`PIPES` 又要
  `DEV_PIPE_*` 五项，`FS_TMPFS` 要四个 GUARD 值）
- 用例会 `chdir` 到 `CONFIG_TESTS_TESTSUITES_MOUNT_DIR`，该目录必须先挂上，
  否则第一个用例就报 `Failed to switch the mount dir`

另有一个公共仓缺陷：`tests` 仓与 `apps/testing/testsuites` **抢同一批
`CONFIG_CM_*_TEST` 符号**，但 `tests` 仓只发布了 dfx/kv 两个用例目录，
Makefile 里却仍留着 sched/syscall/time/pthread/mutex 的分支，开启后
make 会去找不存在的 `cmocka_sched_test.c`。已给这 8 个分支补上
"主源文件存在"的条件，作为补丁归档。

### ★ 1.3.5 的两件事：一个真缺陷，一个用例本身的规模问题

**真缺陷（已修）**：SD 卡此前只有打开 `CONFIG_RK3576_DWMMC_TRACE` 才能
工作。根因不是竞态 —— `priv->last_result = OK;` 这句写在了 `#ifdef` 调试块
**里面**，而所有失败路径的赋值都在块外。关掉 trace 后它永远停在命令发出
前置的 `-EBUSY`，`recvshort/recvlong` 开头的
`if (priv->last_result != OK) return ...` 让每条命令的响应读取都失败。

"只有开日志才工作"几乎必然被读成"日志的额外延时掩盖了时序问题"，排查
方向也确实一度是延时、FIFO 水位、时钟。实际与时序毫无关系，编译器也不会
有任何提示。**规则：`#ifdef` 调试块里只放打印和统计，任何改变状态的语句
都要放外面 —— 切换调试选项不应该改变功能。**

**用例的规模问题（无解，只能换验收方式）**：`cmocka_driver_block` 的第一个
子项 `drivertest_block_stress` 会逐扇区写**整个设备的 95%**：

```c
nsectors = pre->cfg.geo_nsectors * SECTORS_RANGE;   /* 0.95 */
for (i = 0; i < nsectors; i++)
  {
    lseek(...); write(..., 512); fsync(pre->fd);
  }
```

它是照着 1.3.4 那种 `mkrd -m 10` 的 10MB 内存盘设计的。换成多 GB 的 TF 卡
就是几千万次单扇区写加 fsync，PIO 方式要按天算 —— 实测跑了十几分钟毫无
进展，而且会一直占住控制台（nsh 阻塞在子任务上，Ctrl-C 不受理，只能断电）。

替代的有界验收（配置已编入）：

```sh
mkfatfs /dev/mmcsd1
mount -t vfat /dev/mmcsd1 /mnt
fstest -n 10 -m /mnt
```

`fstest` 做的同样是"随机内容写进去、读回来、CRC 比对"，验证强度相当，
规模由 `-n` 控制。顺带给板子一个能用的 SD 文件系统。

### ★ 1.3.5 Flash 功能只能对 SD 卡跑，不能对 eMMC 跑

`cmocka_driver_block` 从**第 0 扇区**开始写：

```c
for (i = 0; i < nsectors; i++)
  {
    lseek(pre->fd, i * pre->cfg.geo_sectorsize, SEEK_SET);
    ret = write(pre->fd, input, pre->cfg.geo_sectorsize);
```

eMMC 第 0 扇区往后是引导器与启动镜像，跑一次就把板子写坏。目标必须是
`/dev/mmcsd1`（TF 卡），卡上没有需要保留的内容。

## 二、驱动 BSP（15 项）—— 这是主战场

| # | 用例 | 依赖的驱动 | 状态 |
|---|---|---|---|
| 1.3.1 | 烧写测试 | 启动链路 / 烧录流程 | ✅ |
| 1.3.2 | RAM 读写 | `fstest` + tmpfs | ◆ 已编入 |
| 1.3.3 | RAM 读写性能 | `ramtest` | ◆ 已编入 |
| 1.3.4 | RAM 随机读写 | `mkrd` + `cmocka_driver_block` | ◆ 已编入 |
| 1.3.5 | Flash 功能 | eMMC / SPI Flash + MTD | ⛔ 阻塞，见上 |
| 1.3.6 | **GPIO 功能** | `cmocka_driver_gpio` | ✅ 4/4（需 5↔7 脚短接） |
| 1.3.7 | **I2C / SPI 功能** | I2C、SPI 驱动 | ◐ 需对端板子，见上 |
| 1.3.10 | **UART 串口功能** | DW 8250 串口 | ✅ 1/1 |
| 1.3.11 | UART 文件传输 | ymodem `sb`/`rb` | ◆ 已编入 |
| 1.3.12 | RTC 时钟 | HYM8563 | ✅ |
| 1.3.13 | **Timer 定时器** | `cmocka_driver_oneshot` | ✅ 独立 TIMER |
| 1.3.14 | 时间一致性 | `date` + RTC | ◆ 需静置 24h |
| 1.3.15 | Watchdog | `cmocka_driver_watchdog` | ✅ 实测触发系统复位并恢复 |
| 1.3.16 | RNG | `hexdump /dev/random` | ✅ 自检两批不同，实测读数随机 |
| 1.3.17 | Crypto | 软件 cryptodev | ◆ 八项已编入 |

已知的 cmocka 驱动测试命令：
`cmocka_driver_gpio` `cmocka_driver_uart` `cmocka_driver_rtc` `cmocka_driver_oneshot`
`cmocka_driver_watchdog` `cmocka_driver_pwm` `cmocka_driver_block` `cmocka_driver_audio`
`cmocka_driver_framebuffer` `cmocka_driver_touchpanel` `cmocka_driver_relay`

## 三、系统应用 + 性能 + 稳定性（7 项）

| # | 用例 | 说明 |
|---|---|---|
| 1.2.1 | Reboot 启动异常 | ✅ 10/10 成功 |
| 1.2.2 | Cold boot 启动异常 | ✅ 5/5 成功（口径见 notes/xts-stability.md） |
| 1.2.3 | 系统 RAM 资源占用统计 | ✅ 已用 3.5MB / 63MB，占 5.6% |
| 1.2.4 | 系统 Flash 资源占用统计 | ✅ nuttx.bin 1.20MB（含全部 xTS 用例） |
| 2.1.3 | **Cold Boot 启动时间** | ✅ 平均 4795ms |
| 2.1.4 | **Reboot 启动时间** | ✅ 平均 4744ms |
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

## ★ 一次撤销：1.3.13 之前的"通过"不算数

`/dev/oneshot` 的下半部直接用了 arm64 通用定时器，而**那个下半部正是
调度器在用的那一个**（`up_timer_initialize()` 里已经
`up_alarm_set_lowerhalf(arm64_oneshot_initialize())`）。
`struct oneshot_lowerhalf_s` 只有一对 `callback/arg`，注册 `/dev/oneshot`
就把调度器的定时回调顶掉了，从此系统时基不再工作：

| | 注册 `/dev/oneshot` | 摘掉 |
|---|---|---|
| `sleep 3` | 25 秒不返回 | 3.3 s |
| `sleep 8` | 25 秒不返回 | 8.4 s |
| `work_queue` 延时项 | 从不触发 | 每 30 秒一次 |
| 自发重启 | 每 95 秒一次 | 120 秒内 0 次 |

而串口、nsh、所有命令都照常 —— 它们靠 UART 中断唤醒，`up_mdelay()` 靠忙等
读计数器，两者都不需要时基。所以这个故障可以**长期潜伏而不被发现**。

### 哪些结论要跟着重看

按提交顺序，`/dev/oneshot` 是在 `c195f3a xTS: Timer 1.3.13 通过` 引入的。

- **在它之前测的仍然算数**：1.1.x 全部、1.2.1~1.2.4、2.1.3、2.1.4 ——
  这些是在时基正常的镜像上测的。
- **在它之后测的要复测**：1.3.13（撤销）、1.3.15 Watchdog（用例里的等待
  可能根本没在计时）。1.3.16 RNG 不依赖定时，保留。

### 已修：给 /dev/oneshot 一路自己的硬件定时器

`arch/arm64/src/rk3576/rk3576_timer.c`，用 TIMER_NS_0 的两个通道：

  CH0（0x2ACC0000，GIC_SPI 45 → IRQ 77）  用户定义 + 向下计数 + 中断 → 闹钟
  CH1（0x2ACC1000）                        自由运行 + 向上计数        → current()

CH1 不接中断，也就不需要知道它的 GIC 号 —— 设备树只声明了 CH0 的。

两条只有 TRM 里有、Linux 驱动里没有的信息：通道间距是 **0x1000**（不是
设备树 reg 长度暗示的 0x20），CONTROL **bit3 选计数方向**（RK3576 支持
向上计数，Linux 匹配的 `rockchip,rk3288-timer` 从不碰这一位）。

实测：`cmocka_driver_oneshot` OK、`sleep 3/8` = 3.3/8.4 s、静置 120 s 零复位。

### ★ 1.3.15 的 cmocka 套件在这颗芯片上跑不完

`cmocka_driver_watchdog` 有 4 个子项，第一个 `drivertest_watchdog_feeding`
就会把板子复位，于是永远到不了汇总行。原因在硬件：

> DesignWare 看门狗一旦使能，**软件无法关闭** —— WDT_CR 的使能位是
> 「写一次生效、只能靠复位清除」的。

用例做完自己那一段之后不再喂狗，板子就被复位。这不是移植缺陷，是这颗
IP 的特性。xTS 对 1.3.15 的预期结果是「触发系统复位并恢复」，这一点是
实测到的：复位发生，重启后 `soc warm boot, reset status: 0x1050`。


## 一次烧写要跑完的命令清单

下面八项的配置都已经开好、程序都已编进镜像，剩下的只是在 nsh 里敲。
按顺序跑完，进度表上的 ◆ 就能换成 ✅ 或写明失败原因。

```sh
# 1.1.8  Kernel-C —— 期望打印 Hello, World!!
hello

# 1.1.12 Kernel-md5 —— 期望 100 个 md5 值且完全一致
echo openvela-rk3576 > /tmp/1.txt
md5_test -f /tmp/1.txt -c 100

# 1.3.2  RAM 读写 —— 期望 PASS
fstest -n 10 -m /tmp

# 1.3.3  RAM 读写性能 —— 先用 free 看 largest，再把它填进 -s
free
ramtest -w -s <largest>

# 1.3.4  RAM 随机读写 —— 先造一块 10MB 的内存盘再对它跑块设备用例
mkrd -m 10 -s 1000 1024
ls /dev
cmocka_driver_block -m /dev/ram10

# 1.3.11 UART 文件传输 —— 板端发/收，PC 端用 minicom 的 ymodem 收/发
sb /tmp/1.txt
rb

# 1.3.17 Crypto —— 八个算法各跑一遍，全部 PASS
cmocka_des3cbc
cmocka_aescbc
cmocka_aesctr
cmocka_aesxts
cmocka_hmac
cmocka_hash
cmocka_crc32
cmocka_ecdsa

# 1.3.14 时间一致性 —— 设成 PC 当前时间，之后每 6h 看一次，共四次
date -s "..."
date
```

### ★ 用例的实际命令名与文档不一致

xTS 文档 1.3.17 写的是"在 nsh 中输入 `des3cbc`"，实际编出来的名字带
`cmocka_` 前缀（`CONFIG_TESTING_CRYPTO_*` 走的是 cmocka 框架）。文档里
其余几处也有类似出入。**以 `builtin_list.h` 里注册的名字为准**，敲不出来
时先 `help` 看一眼，不要以为是没编进去。

### ★ 1.3.2 名叫"RAM 读写"，测的是内存文件系统

用例命令是 `fstest -n 10 -m /tmp`，挂载点写死在命令里。板子上没有 `/tmp`
就直接失败，而且报的是文件系统错误，看不出缺的只是一个挂载点 —— 所以
在 `kickpi_k7_appinit.c` 里补挂了一块 tmpfs 到 `/tmp`。

顺带修掉一处条件编译的嵌套错误：原来 `/data` 那块 tmpfs 的挂载代码嵌在
`#ifdef CONFIG_FS_PROCFS` 里面，两者毫无关系，关掉 procfs 会让 tmpfs 跟着
消失，而现象出现在很远的地方（cmocka 用例报 `Failed to switch the mount
dir`）。这类错误没有任何编译期提示。

### ★ 又两处 tests 仓的 Makefile 缺陷

与前面记过的 `CONFIG_CM_*_TEST` 那批同一类 —— Makefile 里的分支引用了
本仓根本没发布的目录：

- `CONFIG_FS_TEST` 展开成 `include $(CURDIR)/fs_test/fs_test.mk`，
  而发布出来的目录叫 `vela_fs_test/`（`fs_test.mk` 内部用的正是这个名字）
- `block_device_test` / `libc_test` / `spi_slave_test` / `http_test` 四个
  目录被**无条件** include，目录不存在，make 直接停在 context 阶段

已按 `testsuites/Makefile` 里已有的 `$(wildcard ...)` 写法给九处无条件
include 补上"文件存在"的条件，并把 `fs_test/` 改成 `vela_fs_test/`。

两个 Makefile 的改动一起归档在 `bsp/tests-xts-makefile.patch`，
在一份全新的 `repo sync` 上用 `git -C src/tests apply` 还原 —— `tests`
是公共仓，改动不能提交上去，不归档就会在下一次同步时消失。
（`tests/Kconfig` 里那处绝对路径是构建时自动生成的，不属于修改，不归档。）

## ★ 2026-09-05 实测记录

一次连跑的结果，命令与输出都在 `logs/` 的会话记录里：

| 用例 | 命令 | 结果 |
|---|---|---|
| 1.1.8 | `hello` | Hello, World!! |
| 1.1.12 | `md5_test -f /tmp/1.txt -c 100` | 99 行全部同值（串口丢 1 行） |
| 1.3.2 | `fstest -n 10 -m /tmp` | OK: 20, FAILED: 0 |
| 1.3.3 | `ramtest -w -s 1048576` | 四个阶段无报错 |
| 1.3.4 | `mkrd -m 10 -s 512 2048` + `cmocka_driver_block -m /dev/ram10` | 3/3 OK |
| 1.3.5 | `mkfatfs -F 32` + `mount` + `fstest -n 10 -m /mnt` | OK: 20, FAILED: 0 |
| 1.3.13 | `cmocka_driver_oneshot` | OK |
| 1.3.17 | 八个 `cmocka_*` 算法 | 全过，零失败 |

### ★ md5_test 之前的"挂死"是时基造成的

`md5_test` 每轮之间有一句 `usleep(10000)`。在 `/dev/oneshot` 抢走调度器
定时器、系统时基失效的那段时间里，`usleep` 永不返回 —— 于是用例挂住，
看起来像是用例或文件系统有问题。时基修好后一次通过。

**一个坏掉的公共设施会制造出一批看起来彼此无关的假故障。** 这一节里它
至少伪装成了三件事：板子随机重启、`cam show` 挂死、`md5_test` 挂死。

### ★ 16GB 的卡必须显式 `-F 32`

`mkfatfs /dev/mmcsd1` 会失败：它只试 FAT12 和 FAT16，两者的簇数上限
（4078 / 65518）都远小于这张卡的 96 万簇，试遍所有簇大小后报
`Failed to set cluster size`。要写 `mkfatfs -F 32`。

另外别在格式化前 `mount`：卡上残留的垃圾会被 FAT 当成引导扇区，算出
十几亿号的扇区去读，卡回 `OUT_OF_RANGE` 之后就不再应答任何命令，
只能重启板子。**先格式化，再挂载。**

## ★ C++（1.1.9 / 1.1.13）：不用下载源码

`nuttx/libs/libxx` 里 LIBCXX 和 uClibc++ 都要联网下载源码，但**用不着**：
预置工具链 `aarch64-none-elf` 自带 `libstdc++.a` 与 `libsupc++.a`。

```
CONFIG_HAVE_CXX=y  CONFIG_HAVE_CXXINITIALIZE=y
CONFIG_LIBCXXTOOLCHAIN=y      # STL 头文件用工具链的
CONFIG_LIBSUPCXX_TOOLCHAIN=y  # 底层 ABI 用工具链的
CONFIG_CXX_STANDARD="gnu++17"  CONFIG_CXX_EXCEPTION=y  CONFIG_CXX_RTTI=y
CONFIG_EXAMPLES_HELLOXX=y  CONFIG_TESTING_CXXTEST=y
```

★ 但只开这些会在链接时炸出一堆 `basic_string::_M_replace`、
`_Rb_tree_increment`、`__throw_logic_error` 缺符号。原因是
`arch/arm64/src/Toolchain.defs` 里**只有** `LIBSUPCXX_TOOLCHAIN` 那条会加
`libsupc++.a`（底层 ABI），**没有人加 `libstdc++.a`**（STL 的编译部分）。

迷惑之处在于：纯模板的东西（`std::vector<int>`）在头文件里就展开了，
编译链接都过；一用 `std::string`/`std::map`/`iostream` 就缺符号 —— 看着
像"C++ 支持没配好"，其实只差一个库。已在 `board/kickpi-k7/scripts/Make.defs`
里补一行（放板级而不是改公共仓的 Toolchain.defs，等效且不用维护补丁）。

### ★ 1.1.13 卡在异常：arm64 的栈展开表从来没被注册过

`cxxtest` 的 vector/map/RTTI 都过，`Test Exception` 崩在 `__cxa_throw`
内部（`eh_throw.cc:97`）。查下来不是我们这块板特有的：

- 链接脚本把 `.eh_frame` 放在 `/DISCARD/` 里 —— **上游 NuttX 的每一块
  arm64 板子都是这么写的**（pinephone、zcu111、vdk-armv8r 都一样）
- 全树没有任何 `__register_frame_info` 调用
- `_Unwind_RaiseException`、`__cxa_throw` 符号都在，`.gcc_except_table`
  （着陆点表）也在 —— 唯独 `_Unwind_Find_FDE` 要查的那张表找不到

把 `.eh_frame` 从 DISCARD 挪进 `.rodata` 之后镜像大了 24KB（数据确实进去
了），但展开器仍然找不到：并进 `.rodata` 就没有独立的 `PT_GNU_EH_FRAME`
程序头了。要真正打通得二选一：给它一个独立输出段并让链接器生成
`--eh-frame-hdr` 的程序头，或者在启动时调
`__register_frame_info(__EH_FRAME_BEGIN__, &object)`。

**结论：这是 arm64 NuttX 的一处普遍空缺，不是移植缺陷。** 如实记 ◐。

## ★ 1.3.11 的症结：CONFIG_SERIAL_TERMIOS 没开

`apps/system/ymodem/ymodem.c` 里其实已经做了正确的事：

```c
cfmakeraw(&term);
tcsetattr(ctx->recvfd, TCSANOW, &term);   /* 传输期间关回显 */
```

但 `CONFIG_SERIAL_TERMIOS` 没开时 `tcsetattr` 是空操作，控制台照样回显 ——
我写进去的每个字节都原样回来，混在 YMODEM 数据流里。第一次抓包时那一长串
`0x43` 就是自己发的 `'C'`，据此还误判过一次"对端发少了一个字节"。

已开 `CONFIG_SERIAL_TERMIOS=y`。主机侧用 lrzsz（`/usr/bin/rb`）：

```sh
# 板端先发，主机紧接着收（顺序反了会让 rb 的 'C' 被 nsh 回显污染）
#   板: sb /mnt/a.txt
#   PC: rb --ymodem < /dev/ttyUSB0 > /dev/ttyUSB0
```

脚本方式的难点是两者共用一条控制台、握手窗口很窄。**用 minicom 的内置
YMODEM（Ctrl-A R）最省事**，xTS 文档对这一项本来也是让用 PC 端终端做。
板端 `sb` 已验证正确：抓到的头块经 CRC 校验通过，文件名 `a.txt`、大小 9 都对。

## ★ 1.1.3 满分：根因是 CONFIG_NAME_MAX，不是文件系统能力

此前记的是"6 项失败是 tmpfs 能力限制（symlink、truncate）"。**这个判断
是错的**，而且错得有代表性 —— 它听起来很合理，于是没人再去验。

实测：tmpfs 的 `truncate` 是好用的（`truncate -s 100 /data/t.txt` 后
`ls -l` 显示文件确实被截到 100 字节）。真正的原因是用例用 `__func__`
拼文件名：

```
test_nuttx_syscall_close03_dir100   → 33 字符
test_nuttx_syscall_write03_file.wav → 35 字符
test_nuttx_syscall_truncate01_file  → 34 字符
```

而 `CONFIG_NAME_MAX=32`，`open()` 直接返回 ENAMETOOLONG，用例只好报
"打不开文件"。`close03` 最能说明问题：它循环建 100 个文件，`_dir1`~
`_dir99` 都是 ≤32 字符全部成功，**只有 `_dir100` 那一个超长**。

改成 `CONFIG_NAME_MAX=64`（`CONFIG_FAT_MAXFNAME` 要跟着改，它不能超过
NAME_MAX，否则 fs_fat32.h 会 `#warning` 而 -Werror 直接编不过）之后
**74/74 全过，symlink 两项也过了** —— 它们同样是名字太长，与符号链接
支持无关。

**教训：把"看起来合理的解释"写进文档，等于给后来的人立了一块路障。**
这条记录挡了很久，直到有人真的去看那 6 项到底报什么错。

## ★ 1.3.11 的正确用法

板端 `rb` 的目标目录是 `-f`，不是 `-p`（`-p` 是"去掉文件名前缀"）。

```sh
# 接收方向（已完整验证）
#   板: rb -f /mnt
#   PC: sb --ymodem <文件> < /dev/ttyUSB0 > /dev/ttyUSB0

# 发送方向（头块正确，数据块 lrzsz 拒收，未定位）
#   板: sb /mnt/send.txt
#   PC: rb --ymodem < /dev/ttyUSB0 > /dev/ttyUSB0
```

前提是 `CONFIG_SERIAL_TERMIOS=y` —— `apps/system/ymodem/ymodem.c` 本来就
调了 `cfmakeraw()` + `tcsetattr()` 关回显，但没开这个选项时 `tcsetattr`
是空操作，控制台照样回显，写进去的字节原样回来混进数据流。

## ★ 跳线要接哪两脚（1.3.6 / 1.3.7）

查 KICKPI-K7 规格书的「40Pin 引脚定义」表，GPIO4 实际只引出五根：

```
GPIO4_A4 → 5 脚          GPIO4_B0 → 8 脚   （SPI4_CLK）
GPIO4_A6 → 7 脚          GPIO4_B1 → 10 脚  （SPI4_MOSI）
                         GPIO4_B2 → 12 脚  （SPI4_MISO）
```

| 用途 | 接线 |
|---|---|
| **1.3.6 GPIO 中断** | **5 脚（GPIO4_A4，输出）↔ 7 脚（GPIO4_A6，输入）** |
| **1.3.7 SPI 环回** | **10 脚（MOSI）↔ 12 脚（MISO）** |

两组都在同一排、相隔一个位置，短杜邦线即可。

★ 此前清单写的是 GPIO4_A7 ↔ GPIO4_B3，**那是错的**：它们取自厂商的
`rk3576-kickpi-k7c-extend-40pin.dtsi`，而 dtsi 列的是**芯片上存在的引脚**，
不等于**连接器上引出的引脚**。这两根都不在 40Pin 表里，插不上线。

**"软件上能配"与"手上能接"是两件事** —— 这个错误只有到了真要插线那一刻
才会暴露，而在那之前它在文档里躺了很久，看起来还很有依据（引了 dtsi）。

## ★ GMAC 现状：PHY 没有输出接收时钟

两个网口对接之后复测，仍然是：

```
GMAC0: RXCLK(PHY 送来) g3-25 采样 200 次 高=0 跳变=0 —— 恒低，无时钟
GMAC0: DMA 软复位超时 DMA_MODE=0x00000001
```

RGMII 的 RXCLK 由 PHY driving，网线对端接没接都不影响它输出 —— 所以
问题不在链路，在 **PHY 本身没跑起来**（供电、复位、25MHz 参考时钟这一层）。
DMA 软复位不完成是它的下游结果：dw_gmac 的软复位要等 RXCLK 才能完成。

另外 `ifconfig` 报 command not found，网络应用层的配置还没开。

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
