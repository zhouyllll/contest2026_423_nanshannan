# 前期调研：必须亲手核实的前提

> 这些问题的答案决定 [`PLAN.md`](PLAN.md) 怎么改。**在写第一行板级代码之前答完。**
> 这就是 [`notes/DEBUG-CASES.md`](notes/DEBUG-CASES.md) 方法论 ① —— **先做观测，别急着动手**。

每题写清楚：**怎么查**、**答案长什么样**、**答案会影响什么**，答完把结论填回本文件。

**★ B1 / B2 / B3 已于 2026-08-30 全部答完**，依据是 KICKPI 官方 Armbian
源码中的厂商 U-Boot defconfig，且**四个假定值全部命中**（见各条结论）。
技术上已无纸面阻塞，剩余 B4 / B5 属上板阶段。

---

## 一、已有结论（前两轮已实际验证，与芯片无关，不必重查）

| 结论 | 要点 |
|---|---|
| **构建体系** | `repo` 管理 265 个仓库；openvela 自带 `aarch64-none-elf-gcc 13.4.0`，无需自建交叉编译环境；入口是顶层 `build.sh <board>:<config>` |
| **构建环境的两个坑** | `build.sh` 找 kconfig 的路径与预编译包实际位置不一致；交叉编译器不会自动进 PATH。均已固化到 [`scripts/env.sh`](scripts/env.sh)，**必须先 source** |
| **★ 不使用设备树** | openvela 的 ARM64 端口无一使用 DTS；硬件信息硬编码在 `arch/arm64/include/<soc>/chip.h`、`hardware/<soc>_memorymap.h` 与 Kconfig。**所以主线 dtsi 是"抄参数"的来源，不是"运行时读取"的对象** |
| **端口分层与注册点** | `arch/arm64/include/<soc>/`（对外常量）+ `arch/arm64/src/<soc>/`（SoC 驱动）+ 板级目录；新 SoC/新板需在 `arch/arm64/Kconfig` 与 `boards/Kconfig` 共 6 处注册，**漏一处不会出现在 menuconfig** |
| **通用层边界** | GIC（**v2 `arm64_gicv2.c` / v3 `arm64_gicv3.c` 两条路径都有**）、PSCI、Generic Timer、MMU 均由 `arch/arm64` 通用层提供，SoC 层只需给常量。RK3576 走 **v2** |
| **失败后的清理** | 构建/配置失败会留下有毒的中间状态，重试前先 `make distclean`；移动源码树会导致绝对路径残留，见 `DEBUG-CASES.md` 案例 2 |
| **串口驱动经验** | 前身项目从零实现过一个 Meson UART（`archive/a311y2/meson-uart-spec.md`），`uart_dev_s`/`uart_ops_s`/上下半部的写法已吃透。**本轮用现成的 16550，这份经验转为"看懂配置项含义"的底气** |

> 前身项目的完整端口实现归档在 `archive/a311y2/nuttx-a311y2.patch`，可作为"一个 openvela
> ARM64 端口长什么样"的完整范例。**结构照抄，值全部作废。**

### SoC 层参数：已在纸面解决

RK3576 的 GIC / CPU 拓扑 / 定时器 / PSCI / 12 路 UART 基址与中断号
已从主线 `rk3576.dtsi` 勘察完毕，见 **[`docs/rk3576-soc-recon.md`](docs/rk3576-soc-recon.md)**。

**其中一条纠正**：RK3576 是 **GIC-400（GICv2）**，不是 GICv3。
RK3399 是 GIC-500、RK3588 是 GIC-600，都是 v3 —— **中断部分不能照抄 rk3399**。

---

## 二、待核实的问题

## B1. ★★ K7 的调试串口是哪一路 UART？

RK3576 有 12 路 UART，基址与中断号已全部拿到（见勘察文档），
**但"哪一路引到了板子的调试排针"是板级信息，SoC dtsi 里没有。**

**★ 资料已确认公开可下载，不必等板子。** 来源：
`https://doc.kickpi.cn/Products/Download/RK_Download/`

| 资料 | 链接 |
|---|---|
| **K7 原理图** | `pan.baidu.com/s/1eTUdqrw7uWMHcEePagJUQg?pwd=m92s` |
| RK3576 datasheet | `pan.baidu.com/s/1dDBC5RjBDYsi1A-VVJOhQw?pwd=winy` |
| **RK3576 SDK 源码** | `pan.baidu.com/s/1AALQW7A5220Ybw-7Q2zghg?pwd=ny5i` |
| 烧录工具 | RKDevTool / SDDiskTool / DriverAssitant（同页） |
| K7 综合资料 | `pan.baidu.com/s/1cMKQt06pWdxZcsOp1XIvQA?pwd=kpcd` |

**怎么查（按效率排序）**

1. ★ **最快**：SDK 里的板级设备树。解包后
   ```bash
   find . -name 'rk3576-kickpi*.dts*'
   grep -n 'stdout-path' <找到的 dts>          # 直接给出是哪一路 + 波特率
   grep -n -A3 '^&uart[0-9]' <找到的 dts>      # 看哪路 status = "okay"
   ```
   `stdout-path` 形如 `serial0:1500000n8`，冒号后就是波特率（顺带答了 B3）。
2. **原理图**：找调试串口排针（通常标 `DEBUG` / `UART_DBG`），看连到哪组 pin，
   再对 RK3576 pinmux 表反查是哪路 UART。与第 1 条互为交叉验证。
3. `doc.kickpi.cn` 上手指南通常直接写明串口参数。
4. 旁证：Rockchip EVB 惯例是 **uart0**（`0x2ad40000`，SPI 76 → IRQ 108）；
   主线参考板 ArmSoM Sige5 用的也是 uart0。

> ⚠️ 百度网盘需要客户端或登录，我这边下不了，这一步得你来。
> 拉下来后把 dts 或原理图的相关页贴给我，或直接跑上面的 grep。

**答案长什么样**：一个基址 + 一个中断号，例如 `uart0 / 0x2ad40000 / IRQ 108`

**影响**：直接决定 `CONFIG_16550_UART0_BASE`、`_IRQ` 和 `_lowputc.S` 里的字面量。
**填错的表现是上板完全没输出、也不报错** —— 最贵的一类返工。

**结论：已确认为 UART0，基址 0x2ad40000，IRQ 108（GIC_SPI 76 + 32）。**

依据：KICKPI 官方 Armbian 源码
`patch/u-boot/legacy/u-boot-radxa-rk35xx/defconfig/kickpi-k7-rk3576_defconfig`

```
CONFIG_DEBUG_UART=y
CONFIG_DEBUG_UART_BASE=0x2ad40000
CONFIG_DEBUG_UART_CLOCK=24000000
CONFIG_DEBUG_UART_SHIFT=2
CONFIG_BAUDRATE=1500000
```

厂商 U-Boot 的 DEBUG_UART 就是板上引出的调试口，是最直接的权威来源。
交叉验证：内核 dts `rk3576-kickpi-k7.dts` 中 uart3/4/5/6/8/10 被显式
`status = "okay"`，唯独没有 uart0 —— 因为该 dts 有 `/delete-node/ chosen;`，
控制台由 U-Boot 动态注入，正说明 uart0 是 U-Boot 侧的调试口而非普通外设。

**代码无需改动**：先前按 Rockchip 惯例填的值与厂商配置完全一致。

---

## B2. ★ UART 的输入时钟频率与波特率是多少？（★ M1 不需要，降级）

dtsi 只给了时钟句柄 `<&cru SCLK_UART0>`，**没有频率数值**（由 CRU 运行时配置）。

> **本题已从 M1 的阻塞项降级。** U-Boot 已经把调试串口的时钟、引脚与波特率
> 分频配好了（它自己就在往那个口打印日志），openvela 接手时该控制器处于
> 已初始化状态。**M1 只需按现有分频往 THR 写字节，不必知道时钟频率。**
> 本题在"自己重新配置波特率"或"启用其它 UART"时才成为阻塞。
> 参考 `arch/arm64/src/rk3399/rk3399_serial.c` 里的 `UART_SCLK 24000000`。
>
> 拿到 SDK 后可直接查实值：`grep -rn 'DEBUG_UART_CLOCK\|SCLK_UART' u-boot/configs/ kernel/arch/arm64/boot/dts/rockchip/rk3576-kickpi*`

**怎么查**

1. 进 U-Boot 命令行，`clk dump` 看 SCLK_UART0 的实际频率
2. K7 SDK 的 U-Boot 配置里 `CONFIG_DEBUG_UART_CLOCK=`
3. 旁证：Rockchip 平台 UART 时钟常见 **24 MHz** 或 **48 MHz**
4. 波特率：Rockchip 惯例是 **1500000**，不是 115200 —— 先按 1500000 试

**影响**：`CONFIG_16550_UART0_CLOCK` 与 `_BAUD`。

**结论：时钟 24 MHz、波特率 1500000，均已确认。**
同一份厂商 U-Boot defconfig：`CONFIG_DEBUG_UART_CLOCK=24000000`、
`CONFIG_BAUDRATE=1500000`。`CONFIG_DEBUG_UART_SHIFT=2` 同时确认了
`CONFIG_16550_REGINCR=4` / `REGWIDTH=32` 的填法正确。**代码无需改动。**

---

## B3. ★ 内核加载地址与 DDR 布局是什么？

**怎么查**

1. U-Boot 环境变量 `printenv`，看 `kernel_addr_r` / `fdt_addr_r`
2. K7 资料里的内存分区表
3. 旁证：rk3399 与 zcu111 的 openvela 端口都用 `CONFIG_LOAD_BASE = 0x02080000`；
   RK3576 的 DRAM 一般从 `0x40000000` 起

**答案长什么样**：`CONFIG_LOAD_BASE`、`CONFIG_RAM_START`、`CONFIG_RAM_SIZE` 三个值

**旁证（厂商 U-Boot defconfig）**：`kickpi-k7-rk3576_defconfig` 中
`CONFIG_TARGET_EVB_RK3576=y`，即沿用 RK3576 EVB 的目标配置，
继承 `include/configs/rk3576_common.h` 的 `ENV_MEM_LAYOUT_SETTINGS`
（`kernel_addr_r=0x42000000`）。同文件的
`CONFIG_FASTBOOT_BUF_ADDR=0x40c00800` 落在 0x40000000 之后，
与 DRAM 基址 0x40000000 自洽。**当前取值 0x42000000 有据，仍建议上板
`printenv kernel_addr_r` 做最终确认。**

**影响**：三处必须一致（chip.h / defconfig / 链接脚本），
**不一致的表现同样是"没输出也不报错"**。
`scripts/check-addr.sh` 就是为这个写的，已按 rk3576 改写并全部通过。

**结论：DRAM 容量已实测，加载地址待进 U-Boot 确认。**

- **DRAM 4 GB**：板上出厂 Android `cat /proc/meminfo` 报
  `MemTotal: 3989804 kB`。物理范围 `0x40000000 .. 0x140000000`。
  当前 `CONFIG_RAMBANK1_ADDR=0x42000000 + 512MB` 稳落在范围内。
- **`kernel_addr_r` 仍为推断值** `0x42000000`（来源：U-Boot 主线
  `rk3576_common.h`；旁证：厂商 defconfig 有 `CONFIG_TARGET_EVB_RK3576=y`）。

> ★ **该项的重要性可下调**：`booti` 可以显式传地址，不必依赖
> `kernel_addr_r` 这个默认值 ——
> `tftp 0x42000000 nuttx.bin` 后 `booti 0x42000000 - ${fdt_addr_r}` 即可。
> 真正的要求只有两条：能进 U-Boot 命令行、且 `0x42000000` 是有效且未被占用的
> DRAM。后者已由 DRAM 基址 `0x40000000` + 4GB 容量确认成立
> （`0x42000000` = DRAM 基址 + 32MB，是 Rockchip 的标准布局）。

---

## B4. ★★ 烧录与恢复流程怎么走？（写代码之前必须先跑通）

**这条排在写代码之前，因为你会把 eMMC 写坏很多次。**

**★ 烧录工具已确认公开下载**（RKDevTool / SDDiskTool / DriverAssitant，见
`https://doc.kickpi.cn/Products/Download/RK_Download/`），并附 RK3576 SDK 源码。

**怎么查/做**

1. K7 wiki 的系统固化章节
2. 装 `rkdeveloptool`，把板子按进 **MASKROM** 模式（K7 有独立 MASKROM 键），
   确认主机能识别
3. **完整走一遍"刷回出厂固件"** —— 这是你的安全网
4. 搞清楚 `MiniLoaderAll.bin` / `uboot.img` / `boot.img` 各是什么、烧到哪个分区

**影响**：决定 M1 的迭代速度，以及翻车后能不能自救。

**结论**：（待填）

---

## B5. ★ 串口物理通路本身是通的吗？

**在你的代码没输出时，第一件事是排除"线没接对"。**

**怎么做**：先用**出厂固件**启动，USB-TTL 接调试排针，确认能看到厂商系统的
启动日志和登录提示。看到了，说明线序、电平、波特率、串口软件全对；
之后你的代码没输出，就一定是代码的问题。

**顺带**：出厂系统起来后导出真实设备树，与主线 dtsi 逐项核对：

```sh
# 板上执行
cat /sys/firmware/fdt > /tmp/board.dtb
# 主机侧
dtc -I dtb -O dts /tmp/board.dtb > board.dts
grep -A8 'serial@\|interrupt-controller@' board.dts
```

**影响**：这是把 B1/B2/B3 的"参考值"升级为"实测值"的最快路径。

**结论**：（待填）

---

## B6. 代码怎么组织与提交？

**已知**（本轮已比前身项目明确得多）：

* `open-vela/vendor_rockchip` 仓库**已创建**，大赛分支 `dev-ai-contest-2026`
* 目录 `boards/rk3576/kickpi-k7/` **已建好**，内含官方 README 征集 PR
* 官方指定的板级结构：`Kconfig`、`include/board.h`、`src/`、`configs/<名称>/defconfig`
* 大赛规则：对公共仓的改动**不能直接提交进队伍仓库本体**，须以 patch + 可复现说明留存

**要答清楚**

* 队伍专属仓 `contest2026_<编号>_<队名>` 的 `<linkfile>` 具体怎么写、
  映射到 `vendor/rockchip/boards/rk3576/kickpi-k7/` 的哪一级
* SoC 层改动（`nuttx/arch/arm64/{include,src}/rk3576/`）只能走 patch，
  板级改动能否用真实目录 + linkfile

**影响**：决定 `bsp/` 的组织形式。前身项目用"补丁作为权威副本"（`sync-bsp.sh`）；
本轮**板级代码归属 vendor 仓库，很可能可以直接用真实目录 + linkfile，
只有 SoC 层需要补丁**。

**结论**：（待填）

---

## B7. ★★★ 赛程时间与赛道评分细则？（最高优先级，非技术）

**这条决定方案要不要缩水，比任何技术问题都靠前。**

**已知**：大赛全程 4 个月，7 月仍在报名，近 40 万奖金，
线上初赛 → 线下决赛 → 颁奖。官网是 SPA，评分细则抓不到。

**要答清楚**

* **提交截止时间**（决定 M3 之后还做不做）
* BSP 赛道的评分细则、是否强制要求 xTS 通过率
* 「有效 Skills 沉淀」的验收标准与提交形式

**怎么查**：7 月 23 日官方详解直播回放；或直接问组委会。

**影响**：时间紧就砍到 M1+M2+M6（及格线 + 文档 + PR），
M5 的 AI Demo 用最省力的 tflite-micro CPU 推理落地。

**结论**：（待填）

---

## B8. AI 开发日志与 Skills 沉淀

**结论：已完成 Skills 沉淀，日志工具已安装并验证。**（本条与芯片无关，继承自前一轮）

**① 官方技能集已安装**到 `src/.claude`（openvela 工程根目录，与项目自有 skill
分开存放，避免目录冲突）。与本项目相关的：

| skill | 作用 |
|---|---|
| `nuttx-driver-development` | 驱动实现模式（sensor/char/net/fb/LCD/USB/audio…） |
| `driver-code-reviewer` | 驱动代码质量审查，提交前自检 |
| `openvela-build` / `openvela-quickstart` | 构建与环境 |
| **`contest-log-collector`** | ★ AI 开发日志官方工具 |
| `kconfig-tweak` | 命令行改 `.config`，免开 menuconfig |
| `executor` / `tmux` | 管理 QEMU、gdb 等交互式进程 |
| `codesize` / `memdump` | 固件体积与内存分析（对应 xTS 1.2.3/1.2.4 资源占用统计） |
| `submit-pr` | 提 PR（对应 M6 的上游贡献） |
| `skill-creator` | 写 skill 的规范与校验脚本 |

**✅ 日志工具已安装**（`verify-setup.sh` 11 项全过）：

```
~/.claude/contest-collector.env      身份（TEAM_ID + GITHUB_LOGIN）
~/.claude/contest-shared/            hook 脚本
~/.claude/settings.json              注册 Stop / SessionEnd（deep merge，原配置保留）
~/.local/bin/contest-snapshot        手动补导命令
```

**★ 安装前做过目录重构。** 该工具的采集门控是「从 git 仓库根**向上**寻找 `.repo/`」，
找不到直接 `return 0` 完全不采集，手动补导工具有同样门控。原布局是项目仓在外、
`.repo/` 在其下的 `src/`，向上永远找不到 —— **装上去会是个静默不工作的工具**。
故将项目仓移入工作区 `<工作区>/contest/`，这正是大赛预期的形态。

**⚠️ 两处待更正**（在 `~/.claude/contest-collector.env`；`install.sh` 检测到文件
已存在会跳过，重跑不覆盖，需手工改）：

| 项 | 当前值 | 待更正为 |
|---|---|---|
| `TEAM_ID` | `contest2026-pending`（占位） | 报名通过后组委会分配的正式编号 |
| `GITHUB_LOGIN` | `zhouyllll`（取自 git user.name） | 确认是否为真实 GitHub 登录名 |

**⚠️ 采集是前向的**，装之前的开发过程补不回来。
**⚠️ 会话必须从 `<工作区>/contest/` 或其子目录发起**，工作区外的对话完全不采集。

**② 项目自有 Skill**：[`.claude/skills/soc-hw-recon`](.claude/skills/soc-hw-recon)
（已通过官方 `skill-creator/scripts/quick_validate.py` 校验）。

**选题依据 —— 填的是官方技能集的真实空白**：现有 skill 覆盖的是「怎么写驱动」，
没有任何一个覆盖「硬件参数从哪来、怎么确认它是对的」。对新平台适配赛道来说，
后者才是第一难关 —— 驱动写法有现成模式，但基址填错的表现是「上板没输出也没报错」，
是最贵的一类返工。

**本轮为该 Skill 增加了一个新样本**：从主线 `rk3576.dtsi` 勘察出全部 SoC 参数，
并**证伪了"RK3576 用 GICv3"这一先前判断**（`compatible = "arm,gic-400"` +
四段 reg = GICv2）。这正是该 Skill 强调的"交叉验证、别信二手结论"的实例，
应回填进 skill 的 references。

**③ 仍需向组委会确认**：「有效 Skills 沉淀」的验收标准与提交形式（见 B7）。

---

## 三、答完之后

1. 按 B1/B2/B3 的结论定稿地址常量，改写 `scripts/check-addr.sh` 为 rk3576 版
2. 按 B4 跑通 maskrom 恢复，再开始上板迭代
3. 按 B6 的结论确定 `bsp/` 的组织形式
4. 按 B7 的结论决定里程碑要不要缩水，回填 `PLAN.md` 第 2 节
5. 所有非平凡问题按 `DEBUG-CASES.md` 的格式归档 —— **它同时是交付物**

## 四、板子到位前能做的事

B1–B5 都依赖硬件或资料，但以下工作**现在就能推进**：

* ★ **M1a：在 QEMU 上跑通 `qemu-armv8a:nsh_gicv2`**，把 GICv2 路径读熟。
  这是本轮唯一的新知识点，且完全不依赖硬件
* ★ **精读 `drivers/serial/uart_16550.c` 与它的 Kconfig**，
  搞清 `REGINCR`/`REGWIDTH`/`ADDRWIDTH`/`CLOCK` 的确切语义（配错不报错）
* 对照 `boards/arm64/zynq-mpsoc/zcu111`（同为 GIC-400 的真板）读它的
  `chip.h` + defconfig + 板级初始化，这是最接近 RK3576 的现成范例
* 搭 `arch/arm64/{include,src}/rk3576/` 骨架，把 6 处 Kconfig 注册点接好，
  地址常量先填勘察文档里的参考值，编译通过即可
* 调研 `packages/ai_agent` 与 `apps/mlearning` 的可用配置（对应 M5）
* **把 B7 的问题发给组委会（最优先）**
