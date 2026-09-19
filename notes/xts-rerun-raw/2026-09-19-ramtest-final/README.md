# xTS 1.3.3 RAM 读写性能：PASS（2026-09-19）

- 镜像：38aa775 + nuttx 工作区打 `bsp/upstream/posixspawn-enoent-not-error.patch`；CONFIG_TESTING_RAMTEST=y。
- 步骤按原文：① `free` 取最大空闲块（maxfree）；② `ramtest [-w|h|b] -s <size>`。三种宽度都跑。
- 经 telnet NSH 执行（`scripts/xts-netsh.py`，原始字节 `*.raw`、文本 `*.txt`、`events.jsonl`）；
  同时独占串口抓 syslog（`serial-during-ramtest-{w,h,b}.raw`）。

| 项 | 值 |
|---|---|
| `free` 的 maxfree | 53,415,488 B |
| ramtest `-s` | **53,349,952 B**（maxfree − 64 KB） |
| `-w` / `-h` / `-b` | 各 6 个阶段全部走完（marching 1/0、3 组图样、地址即数据），无 `ERROR: Address ... Found ... Expected`，rc=0 |
| 测试后 `free` 的 maxused | 10.1 MB → 63.5 MB（证明 53 MB 确实被分配并测试） |
| 测试期间串口 syslog | 0 字节（无任何报错） |

## 为什么不是 maxfree 本身

`-s 53415488` 直接 `malloc failed`（原始记录在 `../2026-09-19-ramtest/`）：ramtest 作为任务启动时，
它的 8 KB 栈和 TCB 也从同一个堆分配，轮到 `malloc(size)` 时最大块已经变小，再加块头开销。
减 64 KB 留出这部分，被测区域仍占最大空闲块的 99.9%。

## 顺带修掉的日志噪声

上一轮（`../2026-09-19-ramtest/`）每条 NSH 命令都有一行 `nxposix_spawn_exec: ERROR: exec failed: 2`：
NSH 先把 `free`/`echo` 当程序文件 `posix_spawnp`，ENOENT 后才回退到自带命令。已降为 info，见补丁说明。
