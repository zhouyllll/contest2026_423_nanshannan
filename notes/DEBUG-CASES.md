# 移植踩坑记录

沿用 [`~/embedded-lab/DEBUG-CASES.md`](../../embedded-lab/DEBUG-CASES.md) 的写法：
**现象 — 排查路径 — 根因 — 修复 — 验证**，最后提炼方法。

排查过程比结论值钱。结论只对这一个坑有用，方法对以后所有坑有用。

## 目录

| 章节 | 内容 |
|---|---|
| [案例 1](#案例-1构建报-gic-版本错误根因却在两层之外) | 构建报 GIC 版本错误，根因却在两层之外 ★★★☆☆ |
| [案例 2](#案例-2移动源码树后构建失败三层绝对路径残留) | 移动源码树后构建失败：三层绝对路径残留 ★★★☆☆ |
| [小坑速记](#小坑速记) | 不值得单开一案，但会浪费时间的东西 |

---

# 案例 1：构建报 GIC 版本错误，根因却在两层之外

**日期**：2026-08-20
**阶段**：M0，第一次编译 openvela 基线
**难度**：★★★☆☆（不隐蔽，但报错位置离根因隔了两层，容易往错误方向查）

## 现象

### 第一次失败：Kconfig 解析崩了

在 `src/` 下跑 `./build.sh qemu-armv8a:nsh -j4`，配置阶段就炸：

```
arch/tricore/Kconfig:117: syntax error
arch/tricore/Kconfig:116: unknown option "--help--"
.../external/zblue/Kconfig:22: syntax error
.../apps/graphics/lvgl/Kconfig:29: syntax error
.../apps/graphics/lvgl/Kconfig:28: unknown option "osource"
make: *** [tools/Unix.mk:726: olddefconfig] Error 1
ERROR: failed to refresh
Error: ############# config qemu-armv8a:nsh fail ##############
```

`---help---` 和 `osource` 都是 NuttX/Kconfig 的**合法**语法。
一个解析器同时不认识这两个，说明**不是文件的问题，是解析器不对**。

### 第二次失败：Kconfig 过了，编译阶段报 GIC

修掉 kconfig 之后配置成功（`No configuration change.`），编译立刻报：

```
/nuttx/include/arch/chip/chip.h:55:2: error: #error CONFIG_ARM64_GIC_VERSION should be 2, 3 or 4
ERROR: cc failed: 1
       command: cc -MT ./qemu_boot.o -M ... ./chip/qemu_boot.c
```

**两个可疑点，一眼可见：**

| 观测 | 为什么可疑 |
|---|---|
| 编译器是 `cc` | 这是**宿主 x86 编译器**。交叉编译 AArch64 却在用 `cc`，必然不对 |
| GIC 版本没定义 | `arch/arm64/Kconfig:394` 里 `config ARM64_GIC_VERSION` 明明写着 `default 3` |

## 排查路径

```
第一次失败
1. 是不是 Kconfig 文件本身坏了？   → ---help--- / osource 都是合法语法，排除
2. 是不是同步没完成，文件不全？    → 当时确有 24 个 git 进程在写 apps/、external/
                                     ★ 我押的是这个，后来证明不是主因
3. ★ 是不是解析器不对？            → command -v kconfig-conf → 空
                                     command -v menuconfig   → 空
                                     两个都没有，命中

第二次失败
4. GIC 版本为什么没生效？          → grep ARM64_GIC_VERSION .config → 没有这一项
5. .config 是不是不完整？          → wc -l .config → 85 行
                                     ★ 完整展开应该是几千行，命中
6. 为什么用 cc 不用交叉编译器？    → Toolchain.defs:208 CROSSDEV ?= aarch64-none-elf-
                                     command -v aarch64-none-elf-gcc → 空
                                     指望 PATH 里有，但没人往里加
```

## 根因

**三层叠加，报错只暴露了最外面那层。**

### 层一：`build.sh` 找 kconfig 的路径是错的

```bash
# src/build.sh:50-58
if [ ! -f "${ROOTDIR}/prebuilts/kconfig-frontends/bin/kconfig-conf" ] &&
   [ ! -x "$(command -v kconfig-conf)" ]; then
  pushd ${ROOTDIR}/prebuilts/kconfig-frontends     # ← 该目录根本不存在
  ./configure --prefix=... 1>/dev/null             # ← pushd 失败后这几行在 ROOTDIR 里执行
  make install 1>/dev/null
  popd
fi
export PATH=${ROOTDIR}/prebuilts/kconfig-frontends/bin:$PATH   # ← 加了个不存在的目录
```

预编译的 kconfig 实际在 **`prebuilts/build-tools/linux-x86_64/bin/`**
（`kconfig-conf`、`kconfig-mconf`、`kconfig-tweak` 全在这儿）。

脚本没有 `set -e`，`pushd` 失败后继续往下走，于是**静默地什么也没修好**。

### 层二：失败的配置留下一份有毒的 `.config`

`configure.sh` 会先把 defconfig 拷成 `.config`，再用 `olddefconfig` 展开出全部默认值。
第一步成功、第二步失败，于是留下一份**只有 85 行的半成品**：

```
.config 实际:  85 行     ← 就是 defconfig 原样拷贝
.config 应有:  几千行     ← olddefconfig 展开后
```

`ARM64_GIC_VERSION`（`default 3`）、`ARM64_TOOLCHAIN_*` 这些默认值全都不在里面。

**更麻烦的是它会被后续构建继续沿用** —— 第二次构建时 `configure.sh` 看到 `.config` 已存在，
输出了 `No configuration change.`，一句安心话，实际用的还是那份残缺文件。

### 层三：交叉编译器从来没进过 PATH

```makefile
# nuttx/arch/arm64/src/Toolchain.defs:208
CROSSDEV ?= aarch64-none-elf-
CC = $(CROSSDEV)gcc
```

指望 `aarch64-none-elf-gcc` 在 PATH 里。但 `.config` 残缺导致 toolchain 相关配置项缺失，
`CC` 退化成宿主 `cc`；即使配置正常，`build.sh` 也不会把
`prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin` 加进 PATH。**两处都要自己补。**

### 因果链

```
kconfig 不在 PATH
   └→ olddefconfig 失败
        └→ .config 停在 85 行未展开
             └→ ARM64_GIC_VERSION 等默认值全缺失
                  └→ chip.h 的 #if 分支全不匹配
                       └→ #error CONFIG_ARM64_GIC_VERSION should be 2, 3 or 4
                                  ↑ 你看到的报错在这里
```

**报错在链条末端，根因在链条起点。中间隔了两层。**

## 修复

```sh
# 1. 两条 PATH（已固化到 scripts/env.sh）
export PATH=$SRC/prebuilts/build-tools/linux-x86_64/bin:$PATH           # kconfig 全家桶
export PATH=$SRC/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH  # aarch64-none-elf-gcc 13.4.0

# 2. ★ 清掉有毒的 .config —— 不做这步，前一步白搭
cd $SRC/nuttx && make distclean

# 3. 重新构建
cd $SRC && ./build.sh qemu-armv8a:nsh -j4
```

## 验证

```
LD: nuttx
CP: nuttx.bin
```

```
- Ready to Boot Primary CPU
- Boot from EL2
- Boot from EL1
- Boot to C runtime for OS Initialize

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 322ad9f1 Aug 20 2026 17:11:36 arm64 qemu-armv8a
nsh> hello
Hello, World!!
```

`nanopi_m4:nsh`（rk3399）同样通过，`nuttx.bin` 430KB，入口 `0x2080000`。

## 方法论提炼

**① 报错位置 ≠ 根因位置。先问"这个报错要成立，前面必须发生什么"。**
`#error CONFIG_ARM64_GIC_VERSION` 要触发，前提是这个宏没定义；
它没定义的前提是 `.config` 没展开；`.config` 没展开的前提是 kconfig 工具缺失。
**顺着前提往回推，而不是盯着报错本身查。**
（对应 `embedded-lab/DEBUG-CASES.md` 方法论：矛盾即前提错）

**② 规模常识检查最便宜，最先做。**
`wc -l .config` → 85。一个几千行配置系统展开出 85 行，一眼就知道不对。
这一步只花两秒，却直接定位了问题。**量纲不对的东西，不用细看内容就知道有病。**

**③ 失败的构建会留下有毒的中间状态，重试前先清干净。**
第二次构建之所以给出误导性的报错，正是因为沿用了第一次失败留下的 `.config`。
**构建失败后的第一个动作应该是 `make distclean`，而不是直接重试。**

**④ 顺手的一致性检查：编译器名字对不对。**
交叉编译时报错行里出现 `cc` 而不是 `aarch64-none-elf-gcc`，
这本身就是一条独立的、更早的线索。**看报错时顺带看一眼命令行，经常白捡一个根因。**

**⑤ 别信"没有变化"这类安心话。**
`No configuration change.` 让人以为配置是好的。它比较的是 `savedefconfig` 的输出，
**并不校验 `.config` 是否完整展开**。安心信号也是需要验证的观测。
（对应方法论：先确认观测手段本身可靠）

## 记一笔：我押错的那个假设

第一次失败时我判断"最可能是同步没完成"，理由是当时确实有 24 个 git 进程在写 `apps/` 和 `external/`。
这个假设**合理但错误** —— 等同步全部完成后再跑，同样失败。真正的原因是 kconfig 不在 PATH。

**教训**：现场存在一个显眼的异常（同步在跑），不等于它就是原因。
**"同时发生"和"导致"是两回事。** 当时更省事的做法是直接
`command -v kconfig-conf` 验一下解析器，两秒钟就能排除或命中，
不必等一个多小时的同步跑完再验证假设。

## 附：可复现的报错特征

以后再遇到，按这两个特征串直接对号入座：

| 特征串 | 含义 | 处理 |
|---|---|---|
| `unknown option "--help--"` / `unknown option "osource"` | kconfig 解析器缺失或版本不对 | PATH 加 `prebuilts/build-tools/linux-x86_64/bin` |
| `#error CONFIG_ARM64_GIC_VERSION should be 2, 3 or 4` + 命令行里是 `cc` | `.config` 残缺 + 交叉编译器缺失 | `make distclean` + PATH 加 `prebuilts/gcc/.../aarch64-none-elf/bin` |



---

# 案例 2：移动源码树后构建失败（三层绝对路径残留）

**日期**：2026-08-20
**场景**：把 `src/`（23GB repo 检出）整体 `mv` 到新项目目录，并切换到大赛分支 `dev-ai-contest-2026`
**难度**：★★★☆☆（不难，但要连挖三层，每层的报错都指向别处）

## 现象

`mv` 之后第一次构建：

```
tools/Unix.mk:32: .../nuttx/Make.defs: No such file or directory
make: *** No rule to make target '.../nuttx/Make.defs'.  Stop.
```

但 `ls` 明明能看到 `Make.defs` 就在那儿。

## 排查路径

```
1. Make.defs 真的不存在？   → ls 看得到，但 ls -la 显示是符号链接
2. ★ 是不是断链？          → readlink 指向 /home/.../openvela-rk3568/src/...
                              旧路径！命中第一层
3. 还有多少这种链接？       → find . -type l -lname "*openvela-rk3568*"
                              8 个，全是 configure.sh 生成的
4. 清掉重编 → 新报错：      → apps/external/Kconfig:9 引用
                              '/home/.../openvela-rk3568/src/external/Chipmunk2D/Kconfig'
5. ★ 生成的 Kconfig 里也写死了绝对路径 → 命中第二层
6. 全树找 → 34 个 mkkconfig 生成的 Kconfig 带旧路径
7. 删掉重编 → 再报错：      → 编译用的是宿主 cc，.config 只有 85 行
8. ★ 日志里写着 "No configuration change." → 命中第三层
```

## 根因

**同一个原因（绝对路径 + 移动目录）在三个层次上留下残骸，每层的报错都指向别处。**

### 第一层：`configure.sh` 生成的是绝对路径符号链接

```
nuttx/Make.defs -> /home/dministrator/openvela-rk3568/src/nuttx/tools/../boards/.../scripts/Make.defs
```

共 8 个：`Make.defs`、`drivers/platform`、`include/arch`、`apps/platform/board`、
`arch/arm64/src/{board,chip}`、`arch/arm64/include/{board,chip}`。

**注意这里有个自锁**：正常清理手段是 `make distclean`，但 `Unix.mk` 第 32 行
就要 `include Make.defs` —— **distclean 自己需要那个断掉的链接**，所以清不掉。

### 第二层：`mkkconfig` 生成的 Kconfig 里写死绝对路径

`external/`、`frameworks/`、`vendor/`、`packages/`、`tests/` 下共 34 个
自动生成的 `Kconfig`，内容形如：

```
source "/home/dministrator/openvela-rk3568/src/external/Chipmunk2D/Kconfig"
```

文件头写着 `This file is autogenerated, do not edit.`，但**它们不会因为路径变了
就自动重生成**。

> ⚠️ 其中 `tests/Kconfig` 是**被 git 跟踪**的（mkkconfig 直接改了工作区里的
> 跟踪文件），删了要用 `git checkout -- Kconfig` 恢复，不能一删了之。
> 跟踪版里写的是 `source "$APPSDIR/tests/..."`，是相对的。

### 第三层：残缺的 `.config` 被静默复用

前两层失败各留下一份没展开的 `.config`（85 行，完整应为 2515 行）。
`configure.sh` 看到 `.config` 已存在，就打印 **`No configuration change.`** 并跳过重配，
于是工具链配置项全缺失，`CC` 退化成宿主 `cc`。

**这与案例 1 是同一个陷阱的第二次出现。**

## 修复

```sh
cd src
# 1. 清掉 nuttx / apps 的全部构建残骸（含断链）
git -C nuttx clean -xdff
git -C apps  clean -xdff

# 2. 删掉带旧路径的生成 Kconfig（会自动重生成）
find . -name Kconfig -o -name .kconfig | xargs grep -l "<旧路径>" | xargs rm -f
git -C tests checkout -- Kconfig     # ★ 这个是被跟踪的，要恢复不能删

# 3. 重新构建
source ../scripts/env.sh
./build.sh qemu-armv8a:nsh -j4
```

> `git clean -xdff` 前先确认工作成果已提交到分支或导出为补丁。
> 本次操作前，rk3568 端口已在 `rk3568-bsp` 分支与归档补丁中各存一份。

## 验证

```
.config 行数：85 → 2515
NuttX 0.0.0 dd92bcf4 Aug 20 2026 arm64 qemu-armv8a     ← 大赛分支提交
nsh> ps 正常列出 IDLE / hpwork / nsh_main
```

## 方法论提炼

**① 一个原因可以在多个层次留下残骸，修完一层要预期还有下一层。**
不要因为"改完了、报错变了"就以为在推进 —— 报错变了也可能只是同一个病因的下一个症状。
本次三层全部源自"绝对路径 + 移动目录"这一件事。

**② 清理工具本身可能依赖被破坏的东西。**
`make distclean` 需要 `Make.defs`，而 `Make.defs` 正是断的那个。
**当标准清理手段失效时，退到更底层的工具**（这里是 `git clean -xdff`），
而不是想办法先把清理工具修好。

**③ 删除"自动生成"的文件前，先确认它是不是被跟踪的。**
34 个生成的 Kconfig 里混着一个被 git 跟踪的 `tests/Kconfig`。
`git ls-files --error-unmatch <file>` 一句话就能判定，比事后恢复省事。

**④ 又一次：别信 "No configuration change."**
案例 1 已经记过这条，这次它换了个场景再次出现。
**同一条方法论第二次救场，说明它值得写进检查清单**：
构建异常时，先 `wc -l .config` 看规模对不对，再看别的。

**⑤ 迁移目录这类操作，先想清楚有没有绝对路径。**
更省事的做法是：**移动前先 `make distclean`**，把生成物清干净再搬。
事后清理要挖三层，事前清理只要一条命令。

---

# 小坑速记

不值得单开一个案例，但踩过一次就该记下来的东西。

## `build.sh` 会用 `savedefconfig` 覆写你的 defconfig

**现象**：往 `boards/.../configs/nsh/defconfig` 里写的注释，构建一次之后全没了。

**原因**：`build.sh` 在编译结束后会跑 `make savedefconfig`，
按当前 `.config` 重新生成一份最小化的 defconfig 覆盖回去。
配置项的**值**会保留，**注释和排版**一律丢弃。

**处理**：defconfig 里别写注释，写了也留不住。
配置项的出处和理由写到代码头文件或 `notes/` 里。

**顺带**：这也意味着 defconfig 会被自动规范化（排序、去掉冗余项），
所以构建后 `git diff` 看到 defconfig 有改动是正常的，不是你改错了。

## Kconfig 的 `comment` 指令会出现在生成的 `.config` 里

在 `Kconfig` 里写 `comment "..."`，这行会原样出现在生成的 `.config`
中（以 `#` 开头）。不是 bug，但 grep `.config` 时会混进来，
`grep -E "^CONFIG_"` 可以过滤掉。

---

## 案例 3：U-Boot 遗留的 HCR_EL2.TGE 导致 EL2→EL1 的 eret 非法

**平台**：KICKPI-K7 / RK3576，openvela 大赛分支
**日期**：2026-08-31
**性质**：`arch/arm64` 通用层的真 bug，不限于本平台

### 现象

U-Boot 的 `booti` 跳转成功，NuttX 早期打印正常，但随即崩溃：

```
Starting kernel ...
- Ready to Boot Primary CPU
- Boot from EL2
Bad mode in "Synchronous Abort" handler, esr 0x3a000000
* Reason: Exception from an Illegal execution state, or a PC or SP alignment fault
* PC = ffffffff82a670ec
```

异常被 **U-Boot 的 EL2 向量**捕获（call trace 全在 `0x402xxxxx`，即 U-Boot 本体），
说明故障发生在 NuttX 尚未接管异常向量的阶段。`ESR = 0x3a000000` → EC = `0xE`
= **Illegal Execution State**。

### 排查路径

**① 先排除加载地址。** 崩溃寄存器里 `x25` 随链接地址变化（`0x404004c4` /
`0x406804c4`），一度以为是 `booti` 按 Image header 的 `text_offset`
（`arm64_head.S` 硬编码 `0x480000`）把镜像重定位到了别处。
改链接地址到 `bi_dram[0].start + text_offset = 0x40680000` 后，早期打印变成乱码
—— 反而证明**原来的 `0x40400000` 才是运行地址**：Rockchip 的 U-Boot 2017.09
不做重定位，就在 `mmc read` 的落点原地执行。改回。

**② 无栈单字符打点。** `arm64_lowputc` 不使用栈，可在启动任何阶段调用。
定义宏在 EL 切换路径上逐步插桩：

```asm
#define MARK(ch)  mov x3, x30 ; mov w0, ch ; bl arm64_lowputc ; mov x30, x3
```

> ★ 必须用**单字符**。该板串口丢字节严重（Android 阶段可见
> `** 215 console messages dropped **`），长字符串会被截断误导。
> 第一轮就因 `- Boot fr` 的截断，误判为「崩在打印中途」。

输出 `12E2- Boot from EL2 abcd` → 精确定位：`arm64_boot_el2_init()` 已返回、
`spsr_el2`/`elr_el2` 均已写好，**崩在 `eret` 这一条指令上**。

**③ 逐项排除 eret 的合法性前提。** 在 `eret` 前回读并打印：

| 检查 | 结果 |
|---|---|
| `CurrentEL` | `2` —— 确实在 EL2 ✅ |
| `HCR_EL2.RW`（bit 31） | `R` —— 已置位 ✅ |
| `SPSR_EL2.M[3:0]` | `4` —— EL1t，合法 ✅ |

常规原因全部排除后，只剩 ARM 手册里 Illegal Exception Return 的最后一条：
**`HCR_EL2.TGE == 1` 时禁止 `eret` 到 EL1**。加打点确认，读到 `T`。

### 根因

Rockchip 的 U-Boot 2017.09 在 EL2 运行时置了 `HCR_EL2.TGE = 1`。

而 `arch/arm64/src/common/arm64_boot.c` 的 `arm64_boot_el2_init()` 对
`HCR_EL2` 用的是 **read-modify-write**：

```c
reg = read_sysreg(hcr_el2);
reg |= HCR_RW_BIT;          /* 只补 RW，不清任何位 */
write_sysreg(reg, hcr_el2);
```

只补 `RW`、不清 `TGE`，把 bootloader 的残留状态原样带进了 `eret`。

对比 Linux 内核在同一位置是**覆盖写**：`msr hcr_el2, HCR_HOST_NVHE_FLAGS`，
不保留 bootloader 状态。

**这不是 RK3576 独有的问题——任何在 EL2 置了 TGE 的 bootloader 都会触发，
是可以向上游提交的通用修复。**

### 修复

放在 SoC 层的 `arm64_el_init()` 里，**公共代码零改动**。
`arm64_head.S` 中该 hook 的注释写着 *"Platform hook for highest EL"*，
在 `switch_el` 之前、最高 EL 上执行，正是清理 bootloader 残留状态的位置。

```c
/* arch/arm64/src/rk3576/rk3576_boot.c */
#define HCR_TGE_BIT  BIT(27)      /* 通用层 arm64_arch.h 未定义 */

void arm64_el_init(void)
{
  uint64_t reg;

  if (arm64_current_el() == MODE_EL2)
    {
      reg = read_sysreg(hcr_el2);
      reg &= ~HCR_TGE_BIT;
      write_sysreg(reg, hcr_el2);
      UP_ISB();
    }
}
```

### 验证

```
- Ready to Boot Primary CPU
- Boot from EL2
- Boot from EL1                          <- eret 成功
- Boot to C runtime for OS Initialize    <- 汇编阶段全线走通
```

### 附带教训：修好校验反而切断了调试通路

本例中途曾把 boot.img header 的 SHA1 补正确（见案例 4），使 `boot_android`
能自动加载镜像。结果 `bootdelay=0` 下，落回 U-Boot 命令行的唯一途径
——「所有启动方式都失败」——也一并消失了，板子直奔我们的镜像然后 hang，
**刷新镜像的通路被自己切断**。

> **调试期应刻意保留一个「校验失败」的 boot 分区**，让每次上电都落到 `=>`，
> 手动 `mmc read` + `booti` 加载待测镜像；等目标稳定、不再频繁改代码时，
> 再刷正确校验的镜像做自动启动。顺序反了会给自己制造麻烦。

---

## 案例 4：Android boot.img 的 SHA1 校验

**现象**：把 `nuttx.bin` 塞进 boot 分区的 kernel 段后，U-Boot 拒绝加载：

```
Hash from header:  0xe01dff745e6f727122a975e82893dbdcd8b47505
Hash real:         0xaab106ef879a53165ac4be62b6317649faa882c1
Failed to load android image
AVB verify failed
```

**根因**：Android boot.img header 偏移 576 的 `id[8]` 字段存着各段内容的 SHA1，
换了 kernel 必须重算。

**算法**（拿原镜像反算可精确复现 header 中的值）：各段数据与其长度（4 字节
小端）依次喂进 SHA1，段的顺序按 header 版本递增：

```
v0:  kernel, ramdisk, second
v1:  + recovery_dtbo
v2:  + dtb
```

已实现在 `scripts/repack-bootimg.py`。

**顺带**：该校验失败时 U-Boot 会依次尝试 `boot_fit` → `bootrkp` →
`distro_bootcmd`，全部落空后掉进 `=>` 命令行 —— 这正是案例 3 里被利用、
后来又被自己切断的那条调试通路。
