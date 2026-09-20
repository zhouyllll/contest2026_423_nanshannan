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
| 1.1.4 ostest | `ostest` | 见下方单独说明 | telnet |
| 1.1.6 内存 | `mm` | **TEST COMPLETE** | telnet |
| 1.1.7 scanf | `scanftest` | **OK: 164, FAILED: 0** | telnet |
| 1.1.8 C | `hello` | `Hello, World!!` | telnet |
| 1.1.9 C++ | `helloxx` | 动态构造 / 栈上构造 / 静态构造三种实例均打印 | telnet |
| 1.1.10 popen | `popen` | `popen("help")` 输出经管道完整返回 | telnet |
| 1.1.11 pipe | `pipe` | **4 项全 PASSED**：FIFO interlock / FIFO / PIPE redirection / PIPE | telnet |
| 1.1.13 C++ 功能 | `cxxtest` | std::vector / std::string / std::map / RTTI 均执行 | telnet |
| 1.2.3 RAM 占用 | `free`（openvela）+ `ampctl exec free`（Linux 核） | openvela 63,541,248 B 总量；Linux 3,946,300 KB 总量、已用 17,860 KB | telnet，两核各一份 |
| 1.3.2 RAM 读写 | `fstest -n 10 -m /tmp` | **OK: 20, FAILED: 0** | telnet |
| 1.3.10 UART | `cmocka_driver_uart -d /dev/ttyS0` | **PASSED 1/1** | 串口 |
| 1.3.13 Timer | `cmocka_driver_oneshot -d /dev/oneshot` | **OK** drivertest_oneshot | 串口 |
| 1.3.17 Crypto | `cmocka_des3cbc` | **PASSED 1/1** | 串口 |
| 1.3.17 | `cmocka_aescbc` | **PASSED 1/1** | 串口 |
| 1.3.17 | `cmocka_aesctr` | OK test_aesctr（1 项） | 串口 |
| 1.3.17 | `cmocka_aesxts` | OK test_aesxts（1 项） | 串口 |
| 1.3.17 | `cmocka_hmac` | OK md5 / sha1 / sha256（3 项） | 串口 |
| 1.3.17 | `cmocka_hash` | OK md5 / sha1 / sha256 / sha512（4 项） | 串口 |
| 1.3.17 | `cmocka_crc32` | **PASSED 4/4** | 串口 |
| 1.3.17 | `cmocka_ecdsa` | p256 生成密钥 / 签名 / 验签 均 success，SECP256R1 case success | telnet |

## 需要说明的三处

**1.3.10 换了设备**：原文举例 `ttyS0`。先在 `/dev/ttyS1` 上跑，120 s 内停在
`[ RUN ] drivertest_uart` 不结束（该口没有对端，测试等不到回环数据），
记录保留为 `1.3.10-...ttyS1.*`；改回控制台所在的 `ttyS0` 后 1/1 PASSED。

**串口丢字**：1.5 Mbaud 下主机侧会丢字节，个别行被截断（如
`[ RUN     _uart`、`aesxts` 的汇总行缺失）。判定依据是每个子项的 `[ OK ]` 行，
它们都在；丢字只会漏报不会虚报。

**1.2.3 的取数时机**：`free` 是在本批测试进行中取的，openvela 侧 used 32.3 MB，
高于开机静止态（10.1 MB，见 `2026-09-20-short/`）——此时界面、助手、唤醒都在跑，
且 1.3.4 建的 1 MB RAM 盘仍挂着。两个数都留，不取其一。
