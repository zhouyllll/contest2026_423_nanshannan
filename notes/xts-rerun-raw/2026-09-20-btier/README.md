# B 档用例按原文重跑：保留网络 + 串口两份原始记录（2026-09-20）

目的：把 `notes/xts-final-2026-09-20.md` 里 B 档（“功能通过但没留完整原始日志”）
的用例按原文命令重跑，升到 A 档。

- 镜像：`ecc7cba` 正常镜像（`uname`: `ed8f69ad-dirty Sep 20 2026 07:35:35`），与
  1.2.1/2.1.4/3.1.1 等 A 档证据同一份。
- 脚本：`run.py`（逐条执行 + 双路取证）、`summarize.py`（从原始记录提取结果关键字）。
- 每条用例两份记录：`<用例>-<命令>.net.txt`（telnet 会话）、
  `<用例>-<命令>.serial.raw`（执行期间串口原始字节）；`events.jsonl` 记命令、耗时与字节数。
- **为什么要两份**：cmocka 系列把结果打到 syslog（只在串口可见），普通程序打到 stdout
  （只在 telnet 可见）。09-19 的 RTC 复测就因为只看 telnet 而一度以为“没有输出”。

## 结果

| 用例 | 原文命令 | 结果 | 证据 |
| --- | --- | --- | --- |
| 1.1.2 调度 | `cmocka_sched_test` | **PASSED 16/16** | 串口 |
| 1.1.3 系统调用 | `cmocka_syscall_test` | **PASSED 82 项** | 串口（旧记录为 74 项，用例集已扩充） |
| 1.1.4 ostest | `ostest` | **未通过**：卡在多线程自旋锁测试，根因见下 | `1.1.4-ostest.net.txt`（板上日志）、`1.1.4-ostest.serial.raw` |
| 1.1.6 内存 | `mm` | **TEST COMPLETE** | telnet |
| 1.1.7 scanf | `scanftest` | **OK: 164, FAILED: 0** | telnet |
| 1.1.8 C | `hello` | `Hello, World!!` | telnet |
| 1.1.9 C++ | `helloxx` | 动态构造 / 栈上构造 / 静态构造三种实例均打印 | telnet |
| 1.1.10 popen | `popen` | `popen("help")` 输出经管道完整返回 | telnet |
| 1.1.11 pipe | `pipe` | **4 项全 PASSED**：FIFO interlock / FIFO / PIPE redirection / PIPE | telnet |
| 1.1.13 C++ 功能 | `cxxtest` | std::vector / std::string / std::map / RTTI 均执行 | telnet |
| 1.2.3 RAM 占用 | `free`（openvela）+ `ampctl exec free`（Linux 核） | openvela 63,541,248 B 总量；Linux 3,946,300 KB 总量、已用 17,860 KB | telnet，两核各一份 |
| 1.3.2 RAM 读写 | `fstest -n 10 -m /tmp` | **OK: 20, FAILED: 0** | telnet |
| 1.3.6 GPIO | `cmocka_driver_gpio -i /dev/gpio3 -o /dev/gpio2` | **4/4**：bool / loop / rw / interrupt | 串口（`rw` 子项修驱动后才过，见下） |
| 1.3.10 UART | `cmocka_driver_uart -d /dev/ttyS0`（原文举例的设备） | **PASSED 1/1** | 串口 |
| 1.3.13 Timer | `cmocka_driver_oneshot -d /dev/oneshot` | **OK** drivertest_oneshot | 串口 |
| 1.3.17 Crypto | `cmocka_des3cbc` | **PASSED 1/1** | 串口 |
| 1.3.17 | `cmocka_aescbc` | **PASSED 1/1** | 串口 |
| 1.3.17 | `cmocka_aesctr` | OK test_aesctr（1 项） | 串口 |
| 1.3.17 | `cmocka_aesxts` | OK test_aesxts（1 项） | 串口 |
| 1.3.17 | `cmocka_hmac` | OK md5 / sha1 / sha256（3 项） | 串口 |
| 1.3.17 | `cmocka_hash` | OK md5 / sha1 / sha256 / sha512（4 项） | 串口 |
| 1.3.17 | `cmocka_crc32` | **PASSED 4/4** | 串口 |
| 1.3.17 | `cmocka_ecdsa` | p256 生成密钥 / 签名 / 验签 均 success，SECP256R1 case success | telnet |

## 1.1.4 ostest 为什么不过（根因已定位）

三轮结果一致：日志写到 33,813 字节后不再增长，最后一行是第二轮锁测试的
`Test type: spinlock`；串口上**没有**任何断言、崩溃或错误输出。

`ps` 抓到了现场：

    609  43 ---(0x00000002) 255 RR  pthread - Running  ostest   ← 亲和性 CPU1
    610  43 ---(0x00000004) 255 RR  pthread - Ready    ostest   ← 亲和性 CPU2

`apps/testing/ostest/spinlock.c` 的 `run_test_thread()` 按
`cpu_set = 1u << ((i + 1) % CONFIG_SMP_NCPUS)` 把第 i 个工作线程依次绑到
CPU1、CPU2、CPU3。第一轮 `thread_num=1` 只用 CPU1 之外的一个核，正常出结果；
第二轮 `thread_num=2` 起，必然有线程落在 **CPU1**。

本板**双系统下 CPU1 不接任务**（已知问题，见 `notes/HANDOFF.md` 7.2：CPU1 能启动到
IDLE，但绑上去的线程不运行；单系统下四核均正常），所以该线程永远不推进，
主线程等 join 而挂住。旧清单里“ostest 24 个套件通过”应是在**单系统镜像**上跑的。

结论：在提交的双系统镜像上，1.1.4 **未通过**，且原因不在 ostest 本身，而是
CPU1 的既有缺陷。修好 CPU1 之前，这一项不会过。

## 1.3.6 暴露的驱动缺陷：输出脚读回读错了寄存器

首轮 3/4，`drivertest_gpio_rw` 失败：写 `'1'`(49) 立刻读回得到 `'0'`(48)。
该子项对**输出设备**写值后从同一 fd 读回并要求相等。

排除“引脚没驱动”：同一根杜邦线上的 `drivertest_gpio_interrupt` 是过的，
说明输出脚确实驱动了输入脚。问题在读的路径：`kickpi_gpout_read()` 读的是
`EXT_PORT`（引脚实际电平），而本板 GPIO4_A4 配成输出后该寄存器恒读 0 ——
这个脚做输出时输入缓冲不工作。

修法：输出脚读回改读输出数据寄存器 `SWPORT_DR`（新增 `rk3576_gpio_read_output()`），
即“我驱动成了什么”；输入脚仍读 `EXT_PORT`。修完 4/4，`outvalue is 49, invalue is 49`。

（原文命令写的是 `-a/-b`，本树的 `cmocka_driver_gpio` 用 `-i` 输入 / `-o` 输出；
设备取板上配对的 `/dev/gpio3` = testpin（GPIO4_A6，排针第 7 脚）与
`/dev/gpio2` = testpin-out（GPIO4_A4，排针第 5 脚），两脚用杜邦线短接。）

## 需要说明的两处

**串口丢字**：1.5 Mbaud 下主机侧会丢字节，个别行被截断（如
`[ RUN     _uart`、`aesxts` 的汇总行缺失）。判定依据是每个子项的 `[ OK ]` 行，
它们都在；丢字只会漏报不会虚报。

**1.2.3 的取数时机**：`free` 是在本批测试进行中取的，openvela 侧 used 32.3 MB，
高于开机静止态（10.1 MB，见 `2026-09-20-short/`）——此时界面、助手、唤醒都在跑，
且 1.3.4 建的 1 MB RAM 盘仍挂着。两个数都留，不取其一。
