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

---

## 案例 5：链接地址填成了 U-Boot 搬运前的地址

**现象**

汇编启动阶段全线正常，`arm64_chip_boot()` 里插的 A–F 六个打点也全部
打出：

```
- Ready to Boot Primary CPU
- Boot from EL2
- Boot from EL1
- Boot to C runtime for OS Initialize
ABCD□EF
```

但 `arm64_chip_boot()` 返回后进入 `nx_start()` 就静默挂死，没有任何
异常报告、没有 U-Boot 的崩溃转储。

**定位**

打点证明「代码能跑」，所以一开始把嫌疑放在 `nx_start()` 内部的某个
初始化上。真正的线索在 U-Boot 自己的日志里，之前一直没注意：

```
== DO RELOCATE == Kernel from 0x40400000 to 0x40480000
```

对照 ARM64 Linux Image 启动协议：

```
DRAM 基址 0x40000000 + Image 头 text_offset 0x480000 = 0x40480000
```

text_offset 位于 Image 头偏移 8（8 字节小端），NuttX 在
`arch/arm64/src/common/arm64_head.S` 里硬编码为 `0x480000`。用
`python3 -c "print(hex(struct.unpack('<Q', open('nuttx.bin','rb').read()[8:16])[0]))"`
可直接读出，实测正是 `0x480000`。

也就是说 U-Boot **确实按协议搬运**，`mmc read` 落点 `0x40400000` 只是
中转站，CPU 真正取指的地址是 `0x40480000`。而链接脚本写的是
`0x40400000`。

**为什么前面一切「看起来正常」**

这正是本案例最有迷惑性的地方。搬运是 memcpy，搬完之后
`0x40400000` 上仍残留一份内容完全相同的原始副本。于是：

- PC 相对寻址的代码在 `0x40480000` 正常执行；
- 所有 `.data` / `.bss` / 向量表的**绝对地址**引用都打在 `0x40400000`
  那份副本上 —— 内容一模一样，读写都「对」；
- `VBAR_EL1` 指向副本里的向量表，也是有效代码。

所以汇编阶段和 A–F 全部通过，完全看不出异常。

**真正的爆点**

堆的起始地址由链接符号 `_ebss` 决定，按 `0x40400000` 算约在
`0x4044C000`（镜像 311296 字节 ≈ 0x4C000），堆一直延伸到
`RAMBANK1_ADDR + RAMBANK1_SIZE`。

而正在执行的代码就在 `0x40480000` —— **落在堆区内部**。
`nx_start()` 初始化堆并开始分配，第一批分配就把自己正在执行的指令
覆盖掉，表现为静默挂死。

挂在 `nx_start()` 而不是更早，是因为在此之前没有人碰过堆。

**修复**

链接地址改成搬运后的地址，三处同步：

| 位置 | 改前 | 改后 |
|---|---|---|
| `board/kickpi-k7/scripts/dramboot.ld` | `. = 0x40400000;` | `. = 0x40480000;` |
| `arch/arm64/include/rk3576/chip.h` `CONFIG_LOAD_BASE` | `0x40400000` | `0x40480000` |
| `arch/arm64/include/rk3576/chip.h` `CONFIG_RAMBANK1_ADDR` | `0x40400000` | `0x40480000` |
| `configs/nsh/defconfig` `CONFIG_RAM_START` | `0x40400000` | `0x40480000` |

`scripts/check-addr.sh` 会校验这几处的一致性。

验证方法：`readelf -h nuttx | grep Entry` 应等于 U-Boot 日志里
`DO RELOCATE ... to` 后面的那个地址。

**教训**

1. **`kernel_addr_r` 不是执行地址。** 对 ARM64 Image，执行地址 =
   DRAM 基址 + Image 头里的 text_offset。链接地址要填后者。
   判据就在 U-Boot 自己的日志里（`DO RELOCATE` 那一行），不需要猜。

2. **之前得出过一个相反的错误结论**：曾把链接地址改成 `0x40680000`
   试图迁就 text_offset，结果串口输出变成乱码，于是判定
   「Rockchip U-Boot 2017.09 不搬运，原地执行」。那次改错的原因是
   算错了基准 —— 应该是 DRAM 基址 `0x40000000` 而不是加载地址
   `0x40400000`（`0x40400000 + 0x280000` 这类算法都是错的）。
   一次失败的实验推出了一个过强的结论，还把它写进了注释，
   反过来阻碍了后面的排查。**实验证伪的是那个具体取值，不是整个机制。**

3. **「打点全过」不等于「地址正确」。** 残留副本会让链接地址错误
   在很长一段启动路径上完全隐形，直到第一次动态内存分配。
   遇到「早期一切正常、进 C 运行时才挂」的情况，优先核对
   `readelf -h` 的入口地址与引导器日志里的实际执行地址。

---

## 案例 6：编译读的是 .config，不是 defconfig —— 陈旧配置把堆撑到映射区外

**现象**

汇编启动、`arm64_chip_boot()` 全部通过，进入 `nx_start()` 后走到
`memory_initialize()` 就静默挂死。打点序列停在 `GH`：

```
ABCD□EFGH
```

（G = 进入 `nx_start`，H = `tasklist_initialize()` 返回，
I = `memory_initialize()` 返回 —— I 始终不出现）

**定位**

打点只能告诉你"卡在哪个函数"，不能告诉你"为什么"。这一步不要继续
猜，直接把参数打出来。用 `arm64_lowputc` 拼一个最小十六进制打印
（无需堆、无需 printf、任何阶段可用）：

```c
static void dbg_hex(unsigned long v)
{
  int i;
  for (i = 60; i >= 0; i -= 4)
    {
      int d = (v >> i) & 0xf;
      arm64_lowputc(d < 10 ? '0' + d : 'a' + d - 10);
    }
  arm64_lowputc(' ');
}
```

插在 `up_allocate_heap()` 之后、`kumm_initialize()` 之前，实测：

```
<00000000404cc000 0000000021b34000 ...>
   heap_start          heap_size
```

`heap_size = 0x21b34000` ≈ **539 MB**，而 MMU 只映射了 64 MB。

反推：`up_allocate_heap()` 里 `heap_size = CONFIG_RAM_END - g_idle_topstack`，
所以 `CONFIG_RAM_END = 0x404cc000 + 0x21b34000 = 0x62000000`。

`CONFIG_RAM_END` 由 `tools/mkconfig` 自动生成：

```c
#define CONFIG_RAM_END (CONFIG_RAM_START + CONFIG_RAM_SIZE)
```

查 `nuttx/.config`：

```
CONFIG_RAM_START=0x42000000      ← 早已否定的主线 U-Boot 值
CONFIG_RAM_SIZE=536870912        ← 512MB
```

`0x42000000 + 0x20000000 = 0x62000000`，与实测精确吻合。

**根因**

**编译真正读的是 `nuttx/.config`，`configs/nsh/defconfig` 只是模板。**
改 defconfig 不会自动生效，必须重新配置。我改了 defconfig 就直接
`make`，`.config` 里的旧值一直在用。

堆区 `[0x404cc000, 0x62000000)` 远超映射的
`[0x40480000, 0x44480000)`，`kumm_initialize()` 初始化堆尾的空闲链表
节点时写到未映射地址 → data abort → 静默挂死。

之所以"静默"：MMU 已开启、异常向量表已是 NuttX 自己的，U-Boot 的
崩溃转储不再生效，而此时 syslog 尚未就绪，异常处理打不出任何东西。

**修复**

```
CONFIG_RAM_START=0x40480000
CONFIG_RAM_SIZE=67108864        # 64MB，RAM_END = 0x44480000 = 映射末端
```

改完 `.config` 后必须重新生成 `config.h`（见下方陷阱），再全量重编译。

**★ 陷阱：`make olddefconfig` 在本工程跑不通**

upstream 自带的 `arch/tricore/Kconfig` 有语法错误（`---help---` 被解析
成 unknown option），级联导致 `drivers/hwtracing/tricoreht/Kconfig`
的 `endif` 跨文件失配，最终在 apps 段 `endmenu` 处报
`unexpected end statement`。

更糟的是 **它失败时会先把 `include/nuttx/config.h` 删掉**，
于是下一次 `make` 直接崩在缺头文件上。

绕法 —— 只生成 `config.h`，不解析 Kconfig：

```sh
cd nuttx && make include/nuttx/config.h
```

该目标直接用 `tools/mkconfig` 从 `.config` 转换，不碰 Kconfig。

**教训**

1. **改 `defconfig` ≠ 改配置。** 判据只有一个：
   `grep CONFIG_XXX nuttx/include/nuttx/config.h`。
   `.config` 和 `config.h` 才是编译看到的东西。

2. **校验脚本必须校验实际生效的对象。** `scripts/check-addr.sh`
   原先只比对 `defconfig`，对 `.config` 里这个 512MB 的错误值
   全程报"✓ 三处一致"—— 一个只检查模板的校验器比没有校验器更危险，
   因为它给了虚假的信心。已加入第 0 组：`.config` vs `defconfig`
   关键项比对，外加 `config.h` 是否比 `.config` 陈旧的时间戳检查。

3. **打点定位到函数，打印定位到原因。** 六个字符的打点把范围缩到
   `memory_initialize()`，但真正一锤定音的是把 `heap_size` 打出来 ——
   一个数字直接反推出 `CONFIG_RAM_END`，进而锁定陈旧的 `.config`。
   在没有 printf 的启动早期，`arm64_lowputc` + 手写十六进制转换
   是成本最低、可用范围最广的手段。

4. **同一个现象可以有多个成因叠加。** 案例 5 的链接地址错误是真实
   存在的（入口地址与 U-Boot 搬运目标不符），修了它现象却没变 ——
   因为还有本案例这个更靠后的坑。**修复后现象不变，不代表修错了**，
   要用独立判据确认（这里是 `readelf -h` 的入口地址），
   然后继续往下查。

---

## 案例 7：U-Boot 遗留的 HCR_EL2.IMO 把 IRQ 劫持到 EL2

**现象**

案例 5、6 修完后，OS 能完整初始化：堆、GIC、arch timer、`nx_bringup()`
全部通过，进入空闲循环并打印出第一个 `.`，随即崩溃：

```
"Synchronous Abort" handler, esr 0x02000000
* Reason:  Exception from an unknown reason
* PC    =  0000000040200c98
* LR    =  ffffffff82a68bb0
```

`PC` 在镜像范围（`0x40480000`–`0x404cc000`）之外，且两次复现完全一致。
转储格式（`Reloc Off`、`Copy info from "Call trace..."`）是 **U-Boot 的**，
不是 NuttX 的 —— 说明异常是在 **EL2** 被接住的。

**定位**

不猜，直接把三个系统寄存器读出来（在进空闲循环前）：

```c
__asm__ volatile ("mrs %0, vbar_el1"  : "=r" (v));   /* 0x404a7000 */
__asm__ volatile ("mrs %0, CurrentEL" : "=r" (v));   /* >>2 == 1   */
__asm__ volatile ("mrs %0, daif"      : "=r" (v));   /* 0x240      */
```

| 寄存器 | 实测 | 判读 |
|---|---|---|
| `VBAR_EL1` | `0x404a7000` | 等于 `nm nuttx` 里的 `_vector_table`，向量表装对了 |
| `CurrentEL >> 2` | `1` | 确实在 EL1 |
| `DAIF` | `0x240` | D=1、F=1、A=0、**I=0（IRQ 已打开）** |

向量表正确、异常级正确、IRQ 已使能，异常却被 EL2 接走 —— 在 ARMv8
架构上只剩一种可能：**`HCR_EL2.IMO=1`**，把物理 IRQ 路由到了 EL2。

U-Boot 在 EL2 运行时置 `IMO`/`FMO`/`AMO` 以便自己接管中断，交接给 OS
时没有清。第一次 arch timer 中断到达即跳进 U-Boot 早已失效的 IRQ 路径，
落到一片零内存，执行零指令触发同步异常，于是打出上面那份转储。

**修复**

与案例 3 的 `TGE` 在同一处（`arm64_el_init()`，`arm64_head.S` 的
"Platform hook for highest EL" 回调）：

```c
reg = read_sysreg(hcr_el2);
reg &= ~(HCR_TGE_BIT |                    /* 允许 eret 到 EL1 */
         HCR_FMO_BIT | HCR_IMO_BIT |      /* FIQ / IRQ 交回 EL1 */
         HCR_AMO_BIT);                    /* SError 交回 EL1 */
write_sysreg(reg, hcr_el2);
UP_ISB();
```

`HCR_FMO_BIT` / `HCR_IMO_BIT` / `HCR_AMO_BIT` 通用层
（`arch/arm64/src/common/arm64_arch.h`）早已定义，只是没人清。

**教训**

1. **这是第二个同源缺陷。** `arch/arm64/src/common/arm64_boot.c` 对
   `HCR_EL2` 用的是 read-modify-write，只 `|= RW | ATA`，其余位原样
   继承 bootloader 的状态：

   ```c
   reg = read_sysreg(hcr_el2);
   reg |= HCR_RW_BIT;
   write_sysreg(reg, hcr_el2);
   ```

   Linux 在同一位置是**覆盖写**，不保留引导器状态。
   案例 3（TGE）和本案例（IMO）是同一个设计缺陷的两次发作。
   这更像是通用层值得修的地方，而不是每个 SoC 各打一次补丁 ——
   适合作为独立的上游 PR。

2. **"转储是谁打的"是一条高价值线索。** 崩溃信息的**格式**说明了
   异常在哪一级被接住。看到 U-Boot 的格式而不是 NuttX 的，
   立刻就能把范围缩到"路由到了 EL2"，而不是漫无目的地查 OS 内部。

3. **三个正常读数比一个异常读数更有力。** `VBAR_EL1`、`CurrentEL`、
   `DAIF` 全部正常，恰恰把可能性压缩到了唯一解。排除法要求把
   "看起来没问题"的东西也测一遍，不能只测怀疑的那个。

---

## 案例 8：REGINCR 与 REGWIDTH 相乘，串口寄存器整体偏移 4 倍

**现象**

案例 7 修完后系统完全不崩了，空闲循环稳定运行，但**串口一个字都不出**
（`arm64_lowputc` 的裸打印正常，驱动的输出为零）。

**定位**

分三步收敛，每一步都用读数而不是推测：

**第一步：中断到底来没来。** 在 `arm64_doirq()` 入口把 IRQ 号打成两位
十六进制：

```
#1b #1b #1b ...   （71954 次，全部是 27 = arch timer 的 PPI）
```

**没有一次 `#6c`（108 = UART0）**。GIC 分发、CPU 接口、定时器中断、
调度器 tick 全部正常 —— 中断链路本身是通的，唯独这个 SPI 不来。

**第二步：驱动做没做该做的。** 在 16550 驱动三处打点：

```
S      u16550_setup()   跑了
A      u16550_attach()  跑了  ← 设备被打开，说明 nsh 起来了
tTtT   u16550_txint()   在正常开关 TX 中断
```

驱动侧行为完全正常。

**第三步：读 GIC 和 UART 的真实寄存器。**

| 寄存器 | 实测 | 判读 |
|---|---|---|
| `GICD_TYPER` | `0x0000fcef` | 512 条中断线，足够 |
| `GICD_ISENABLER3` | `0x00001000` | bit12 = IRQ 108，**已使能** |
| `GICD_ITARGETSR` | `0x01010101` | 目标 **CPU0** |
| `GICC_PMR` | `0x000000f0` | SPI 优先级 0x80 能通过 |
| **`GICD_ISPENDR3`** | **`0x00000000`** | **IRQ 108 未挂起 → UART 根本没拉中断线** |
| `IER` (`+0x04`) | `0x00000000` | 写入之后**仍然是 0** |
| `+0x10`（当成 MCR 读） | `0x01` → `0x03` | ← **关键转折** |

`+0x10` 那个值从 `0x01` 变成 `0x03`，我最初把它当作 MCR，还以为
"寄存器写入正常"。真相是：**那不是 MCR，是被写偏了位置的 IER**。
`0x03` = `ERBFI | ETBEI`，正是驱动想设的收发中断使能值。

**根因**

`drivers/serial/uart_16550.c` 的 `u16550_serialout()`：

```c
offset *= (priv->regincr * sizeof(uart_datawidth_t));
```

**`REGINCR` 是与 `REGWIDTH` 相乘的，不是二选一。**

`REGWIDTH=32` 使 `uart_datawidth_t` 为 `uint32_t`（`sizeof` = 4）。
若再填 `REGINCR=4`，步长就是 4 × 4 = **16 字节**：

| 寄存器 | 索引 | 错误步长(16) | 正确步长(4) |
|---|---|---|---|
| IER | 1 | `+0x10`（撞上 MCR） | `+0x04` |
| LCR | 3 | `+0x30` | `+0x0c` |
| MCR | 4 | `+0x40` | `+0x10` |

dtsi 的 `reg-shift = 2`（即 4 字节间隔）应当**由 `REGWIDTH=32` 单独
承担**，`REGINCR` 保持默认值 **1**。

佐证：`drivers/serial/Kconfig-16550` 中 `16550_REGINCR` 的 `default 1`；
同为 32 位宽的参考板 `boards/arm64/vdk/vdk-armv8r` 的 defconfig
根本没有设置 `CONFIG_16550_REGINCR`，用的就是默认 1。

**为什么前面一切"看起来正常"**

- `u16550_setup()` 的波特率配置写到了错误偏移，从未生效 ——
  但 U-Boot 已经把 UART 配成了同样的 1500000，所以裸打印一直正常；
- `arm64_lowputc` 是自己写的汇编，用显式偏移，不走驱动，
  因此启动阶段的输出全程可靠，反而掩盖了驱动是坏的；
- 驱动的每一步都"成功返回"，没有任何错误码。

**修复**

```
CONFIG_16550_REGINCR=1        # 原为 4
CONFIG_16550_REGWIDTH=32      # 不变
```

改 `.config` 后须 `cd nuttx && make include/nuttx/config.h`（见案例 6）。

**教训**

1. **两个配置项相乘时，不要各自"看起来对"就收工。** `REGINCR=4`
   和 `REGWIDTH=32` 单独看都能对应上 `reg-shift=2`/`reg-io-width=4`，
   合起来却错了 4 倍。判据应该是最终算出的**步长**，不是单个字段。

2. **`check-addr.sh` 第二次为错误背书。** 它原本硬编码断言
   `REGINCR=4 且 REGWIDTH=32` 才算通过 —— 等于把 bug 固化成了
   "校验通过"。上一次是它只看 defconfig 不看 `.config`（案例 6）。
   **校验器的断言必须有出处**（这里是 `u16550_serialout()` 的那行
   乘法和 Kconfig 的 `default 1`），不能凭对硬件的印象写。
   已改为断言 `REGINCR=1`，并把相乘关系写进注释。

3. **"读数变化了"不等于"写对了"。** 看到 `+0x10` 从 `01` 变 `03`，
   第一反应是"写入正常"，实际它证明的恰恰相反 —— 变化的是**不该
   变化的那个地址**。核对读数时，先确认自己读的是不是那个寄存器。

4. **裸打印通道会掩盖驱动缺陷。** `arm64_lowputc` 全程可用，让人
   一直以为"串口是好的"。调试早期启动固然离不开它，但它跑通
   **不构成**串口驱动可用的证据 —— 两者走的是完全不同的代码路径。

---

## 案例 9：I2C 不通 —— 功能时钟未使能 + 引脚复用偏移算错

**现象**

I2C 驱动写完后，扫描总线全空。加诊断后：

```
I2C2  hit=00  ipd=00  fcnt=0     ← 控制器状态机完全没动
I2C1  hit=0a  ipd=02  fcnt=1     ← 有传输，但每个地址都"应答"
```

同一份驱动代码，两条总线两种表现。

**第一层：PCLK 与功能时钟是两回事**

初始化里有一个自检：往 CLKDIV 写值再读回，一致就认为"寄存器块活着"。
这个自检**通过了**，但总线依然不动。

原因是它只验证了 **PCLK**（APB 寄存器访问时钟）。I2C 还有一路独立的
**功能时钟 `clk_i2c2`** 驱动 SCL —— 它被门控时，寄存器读写一切正常，
但总线上没有任何波形。自检"通过"而设备是死的。

判据是 `FCNT`（已传输字节数）：

| 现象 | 结论 |
|---|---|
| `FCNT = 0` 且 `IPD = 0` | 功能时钟没开，状态机不动 |
| `FCNT > 0`，无 `NAKRCV` | 时钟在跑，但 SDA 读不到真实电平（复用问题） |
| `IPD` 含 `NAKRCV` | 时序正常，从机确实没应答 |

I2C1 是 `FCNT=1` 且无 NAK —— U-Boot 开过它的时钟，但引脚复用在
移交时已不是 I2C 功能，SDA 在应答位被读成低电平，于是每个地址都
"ACK"。**"全部地址都有响应"是引脚没接通的典型特征，不是总线正常。**

**第二层：找时钟门控位时的一次方法错误**

原厂 dtb 里 `i2c@2ac50000` 的 `clocks = <&cru 123>, <&cru 111>`。
拿这两个数字去查**主线**的 `rockchip,rk3576-cru.h`，得到
`CLK_I2C8`/`PCLK_I2C8`，于是判定"这个地址其实是 I2C8"。

**这是错的。** 该 dtb 来自 Rockchip 自己的 BSP 内核，用的是厂商版
CRU 头文件，编号与主线是两套独立分配的体系。改用主线自己的
`rk3576.dtsi` 核对，`i2c2: i2c@2ac50000` —— 原映射本来就是对的。

> **交叉引用两份资料前，先确认它们出自同一体系。**
> 厂商 dtb 只能配厂商头文件，主线 dtsi 才能配主线头文件。
> 拿 A 的数据查 B 的表，得到的一致性是假的。

正确做法：用主线 `clk-rk3576.c` 查 I2C2 的门控位 ——

```
GATE(PCLK_I2C2, ...           CLKGATE_CON(12), 1)
COMPOSITE_NODIV(CLK_I2C2, ... CLKSEL_CON(57), 2, 2, ... CLKGATE_CON(12), 13)
```

顺带解决了另一个此前靠假设的量：`CLK_I2Cn` 的源是
`mux_200m_100m_50m_24m_p` 四选一，**读 CLKSEL 的 mux 位就能知道实际
频率**，不必再"假定不超过 200MHz"。

**第三层：引脚复用的 RK3576 专有特例**

配完复用后立刻读回核对，结果：

```
I2C2: 复用 SCL=gpio0-15→0  SDA=gpio0-16→9
                      ↑ 写了 9，读回 0
```

SDA 成功、SCL 失败，且两者只差一个引脚号 —— 直接指向"与引脚号相关
的偏移特例"。查主线 `pinctrl-rockchip.c` 的 `rockchip_set_mux()`：

```c
if (ctrl->type == RK3576) {
    if ((bank->bank_num == 0) && (pin >= RK_PB4) && (pin <= RK_PB7))
        reg += 0x1ff4;  /* GPIO0_IOC_GPIO0B_IOMUX_SEL_H */
}
```

`RK_PB4`=12、`RK_PB7`=15。SCL 正是 gpio0 pin 15，落在区间内：

| 引脚 | 通用算法 | 加特例后 | 结果 |
|---|---|---|---|
| SCL gpio0-15 | `0x8 + 4 = 0xC` | `+0x1ff4 = 0x2000` | 原先写错了地方 |
| SDA gpio0-16 | `0x2004` | 不适用 | 原先就正确 |

**教训**

1. **"寄存器能读写"不等于"外设能工作"。** 一个外设常有多路时钟，
   寄存器访问时钟（PCLK/APB）与功能时钟是分开门控的。自检要挑
   能覆盖目标能力的观测量 —— 这里 `FCNT` 比 CLKDIV 回读有效得多。

2. **配完寄存器一定要读回核对。** 写错地址不会报错，落在别处也不会
   报错。本例中读回值把范围从"时序/时钟/器件"一下收敛到"pin15 的
   偏移算错"，用时三十秒。这已是本端口第三次靠读回抓到问题
   （前两次是 GPIO 的 VER_ID、I2C 的 CLKDIV）。

3. **对照组比单点测试信息量大得多。** 同一份驱动跑两条总线，
   I2C1 与 I2C2 的差异本身就是证据：它同时排除了"驱动逻辑全错"
   和"两条总线同一个毛病"，把问题拆成了两层。

4. **"全部地址都响应"要当成失败读。** 扫描结果过于完美时先怀疑
   测量本身 —— 这里它意味着 SDA 恒为低，即引脚根本没接到总线上。

---

## 案例 10：GMAC 的 DMA 软复位不完成 —— 六轮逐项补，与一次核全的差距

**现象**

```
GMAC0: DMA 软复位超时 DMA_MODE=0x00000001
```

写 `DMA_MODE.SWR` 后该位永不自清。而 `MAC_VERSION=0x3051`、
`HW_FEATURE0=0x1a1173f7` 都读得到，寄存器块看起来完全正常。

**这个现象有至少六个可能成因，表现完全相同**

Synopsys 手册：软复位只有在**所有时钟域**的复位都撤销后才完成，
其中包括来自 PHY 的接收时钟。因此下列任一缺失都是同一个现象：

| # | 缺什么 | 本端口第几轮补上 |
|---|---|---|
| 1 | 功能时钟（ACLK/PCLK 之外的 125M_SRC、RMII、PTP） | 1、4 |
| 2 | 模块软复位未撤销（SRST_A_GMAC0/SRST_P_GMAC0） | 2 |
| 3 | PHY 硬件复位未释放（板级 GPIO） | 3 |
| 4 | GRF 未选接口模式（RGMII/RMII） | 4 |
| 5 | RGMII 引脚复用未配置（14 个脚，含 RXCLK/TXCLK） | 5 |
| 6 | 时钟只开了门控、没设分频（125M_SRC 的 5 位分频器） | 6 |

**我是怎么走成六轮的**

每一轮都是"想一个最可疑的原因 → 补上 → 不中 → 再想下一个"。
六项全是真实缺失，所以每轮都能自圆其说，也就每轮都没有反思方法。

**一次核全的做法**

打开 dtsi 的 gmac0 节点，把每个属性对着实现列一张表：

```
clocks         五路：125M_SRC / RMII_CRU / PCLK / ACLK / PTP_REF
resets         SRST_A_GMAC0
power-domains  PD_SDGMAC
rockchip,grf   sdgmac_grf（接口模式与时钟选择）
pinctrl-0      五组 14 个引脚
```

五项属性一次列全，第一轮就能补完。**dtsi 本身就是一份完整的需求清单**，
逐条核对的成本远低于逐轮试错。

**还有一层：每路时钟要核"配全了没有"，不只是"开了没有"**

第 5 轮我已经吸取了"逐条核对 dtsi"的教训，却在第 6 轮又漏了分频 ——
因为我只核对了 `clocks` **列表里的每一路是否开启**，没核对
**每一路是否配置完整**。Rockchip 的 COMPOSITE 时钟包含三部分：

```
门控 CLKGATE_CON   选源 CLKSEL_CON 的 mux 位   分频 CLKSEL_CON 的 div 位
```

只开门控而不设分频，输出频率是未定义的。核对时钟必须三项都看。

**一个结构性教训：先验证前提，再查细节**

前五轮都在回答"什么条件缺了导致 SWR 卡住"，却从未验证过前提：
**DMA 寄存器块本身是不是活的**。第六轮才用一个纯数据寄存器
（`DMA_CH0_TXDESC_RL`，写 0x3f 读回）确认了 DMA 块可写。

若当初它不可写，前五轮的方向就整个错了 —— 而我一直拿 **MAC 块**
（偏移 0x0000–0x0FFF）的健康状态，推断 **DMA 块**（偏移 0x1000+）
的状态，这个推断从没被检验。

这与案例 8 里"把 +0x10 当成 MCR 读、见它变化就以为写入正常"是同一个
错误：**读数要先确认读的是不是那个东西**。

**教训**

1. **外设不工作时，先把 dtsi 的每个属性与实现逐条核对**，再开始想
   "最可能是什么"。dtsi 是需求清单，不是参考资料。

2. **核对时钟要看三项**：门控、选源、分频。"这路时钟开了"不等于
   "这路时钟对了"。

3. **修了不中的第二次，就该停下来取读数**，而不是猜第三个。
   本案例第一次取诊断读数是在第六轮。

4. **先验证结构性前提**（寄存器块是否可读写），再查具体配置。
   用 A 部分的健康状态推断 B 部分，是未经检验的假设。

## 案例 11：找不到的触摸 —— 一份原理图解开两个死结

### 现象

屏物理接上后扫遍 I2C1~I2C9 都没有 GT9xx。当时的结论是"缺厂商的
`rk3576-kickpi-k7-android-mipi-5-720-1280-F050008M01.dtsi`，四个关键信息
（触摸总线号、供电轨 GPIO、面板初始化序列、背光控制）都在里面"，
只能等技术支持。

### 转折：换一份资料

厂商 dtsi 一直没拿到，但**原理图里有同样的信息**：
`K7_V1.1_20241211_SCH.pdf` 第 27 页 "Single-MIPI LCM" 给出 30pin FPC
的全部信号，同一份 PDF 里还有芯片引脚复用表。

拿不到 A 资料时，先想清楚**需要的是哪个事实**，再找还有哪些资料能给出
同一个事实 —— 而不是围着 A 打转。

抽取方法：`gs -sDEVICE=txtwrite` 出文本（复用表是矢量文字，能直接 grep），
`gs -sDEVICE=png16m -r600` 出高清图再裁剪（连线关系只能看图）。

### 两个叠加的原因

**其一：触摸在 I2C0 上，而 I2C0 我从没扫过。**

FPC Pin23/24 是 `I2C0_SCL_M1_TP` / `I2C0_SDA_M1_TP`。而我在
`rk3576_i2c.c` 里写过一行注释：

    （I2C0 在 PMU 域，时钟基址不同，暂不加。）

漏掉的那一条恰好就是要找的那一条。**"因为麻烦而跳过"和"已排除"是两回事**，
但排查时很容易把前者记成后者 —— 尤其当跳过的理由写得很有道理时。

I2C0 基址 0x27300000 在常开域，时钟归 PMU CRU（主 CRU + 0x20000）管：

    GATE(PCLK_I2C0, ... RK3576_PMU_CLKGATE_CON(5), 1)
    COMPOSITE_NODIV(CLK_I2C0, ... RK3576_PMU_CLKSEL_CON(6), 7, 2,
                                  RK3576_PMU_CLKGATE_CON(5), 2)

**其二：触摸没电。**

`VCC3V3_LCD_S0` 由 `LCD_PWREN_H`(GPIO0_C6) 经 Q5002(S8050) + Q5100(P-MOS)
开关。不拉高则屏和触摸都无供电 —— 扫遍所有总线都不会应答，而这一点
**在软件上无法与"器件不存在"区分**。

### 一个顺带被推翻的设计假设

原理图上 FPC Pin18 `LCD_TE` 被打叉，未连线。没有 TE 信号，DSI 命令模式
无法与屏刷新同步 —— **这块屏必须走视频模式**，而我的 DSI2 当时配的是
命令模式。这个事实不看原理图不会知道，光看面板手册也看不出来。

### 假信号：注册成功 ≠ 器件存在

补上 I2C0 和电源轨后，日志出现：

    触摸: /dev/input0 就绪（GT9xx @I2C0:0x5d, 中断模式）

差点就当成功了。但 `gt9xx_register()` **只做注册，不访问器件** ——
这行日志无论芯片在不在都会打印。

补了真判据（读 GT9xx 产品 ID 寄存器 0x8140）后，结果是
**0x5d 与 0x14 均 -ENXIO**。触摸确实不应答。

教训与本项目反复出现的那条一致：**日志要打的是观测到的事实，不是
执行到的位置。**"就绪"是位置，"产品号=xxx"才是事实。

### 判据也会失效：内部上拉盖住了外部上拉

为定位 NAK，设计了一个应当能证伪的观测。转接板
（Rockchip RK_IF_RK3568_FPC）的 1.8V 由板上 U1 从 `VCC3V3_LCD` 生成，
触摸 I2C 主板侧的两个 10K 上拉接的正是这个 1.8V，所以：

    PWREN=0 -> 无 3V3_LCD -> 无 1V8 -> 上拉消失，SCL/SDA 应读到 0
    PWREN=1 -> 上拉恢复，应读到 1

实测两次都是 1/1。**判据无效** —— 高电平来自 SoC 内部上拉，把外部上拉
在不在完全盖住了。做外部上拉在位判断，必须先 `rk3576_pinmux_setpull(NONE)`。

同一轮还自造了一个假象：判据把 pin17/18 切成 GPIO 后**忘了切回 I2C 功能**，
于是错误码从 -ENXIO 变成 -ETIMEDOUT。差点当成"换了个新问题"去追。
**改了引脚复用做诊断，诊断完必须还原**，否则后面所有现象都是自己造的。

### 当前状态

供电链已完全查清（见 board.h 顶部）：`VCC_1V8` 与 `VCC2V8_TP` 都由
转接板从 `VCC3V3_LCD` 生成，链路上没有第二个使能。修正后的判据待上板。

尚存的物理可能：转接板改版记录里 V3.0(2024_12_3) 才"将触摸兼容在 LCD 的
FPC 座里面"，早期版本的触摸走独立的 J2(10pin) / J6(6pin) 座子。若手上是
早期版本且触摸排线没插到 J2/J6，总线上确实没有器件 —— 这一条软件查不了。

## 案例 12：背光亮、无显示 —— 五个真缺陷，只有一个是断点

### 现象

屏接上后背光亮起，但没有任何图像。串口里显示链路每一步都报成功。

背光亮本身就是一条有用的信息：背光升压的使能来自 `LCD0_PWREN_H`，
所以它亮说明 `LCD_PWREN` 驱动正常、`VCC3V3_LCD` 已经起来。**故障被
限定在视频通路内**，不必再查电源。

### 依次找到的五处缺陷

1. **`rk3576_dsi2_set_video_mode()` 从来没被调用。** appinit 配完 DSI
   就返回了，控制器停在命令模式，不接收 VOP 送来的像素。
2. **水平时序单位错。** `HSA/HBP/HACT/HLINE` 不是像素数，是 PHY 高速
   时钟域的 16 位定点时间：`H*_TIME = 像素数 × phy_hs_clk × 65536 ÷ pixclk`。
   HLINE 正确值 20,348,928，我写的是 828 —— 差四个数量级。
3. **`MANUAL_MODE_CFG` 没开。** 名字带 `_MAN_` 的那批寄存器只在手动模式
   下生效，不开的话上面所有时序**全部被忽略**。
4. **垂直时序只写了 VSA**，缺 VBP/VACT/VFP；`DSI_VID_TX_CFG`、
   `IPI_PIX_PKT_CFG` 也没写。
5. **`PHY_IPI_RATIO` / `PHY_SYS_RATIO` 完全没写。** DSI 内部三个时钟域
   的比值，控制器靠它对齐速率。算这两个值需要 `sys_clk`，而
   `CLK_DSIHOST0` 是带分频器的 COMPOSITE，我只开了门控没配选源分频 ——
   又一次踩中自己记过的"Rockchip 时钟三件套"。

**每一处都是真缺陷，改完现象一点没变。**

### 真正的断点：一个从没写过的寄存器

前五处全在链路的**下游**。上游的断点是 VOP2 的
`SYS_CTRL_MIPI0_INFACE_CTRL`（0x180）：

    bit[3:2] mipi_port_sel   00=VP0  01=VP1  1x=VP2

读到的值 `0x80100037`，bit[3:2] = **01 = VP1**。而我的时序、彩条、
扫描判据全在 VP0 上。MIPI 接口一直在从一个空的视频端口取像素。

这个值是引导器留下的，我从没写过它 —— 它一直在我心里挂着"未验证"
的标签，却一轮轮先去修那些已经能看见的地方。

### 第二个自带伪装的错误

改完路由，`MODE_STATUS` 从 0x1 变成了 0x0。这个变化才暴露出：
参考驱动的 `enum mode_ctrl` 是

    IDLE=0  AUTOCALC=1  COMMAND=2  VIDEO=3  DATA_STREAM=4

而我按顺序猜成了 `CMD=0, VIDEO=1`。**DSI 从来没进过视频模式**，
一直在 AUTOCALC。而先前那行 `MODE_STATUS=0x00000001` 我读成了
"成功进入视频模式" —— 它其实是 AUTOCALC 的编号。

两个错误刚好互相印证：枚举值猜错，又拿读回值当确认，于是"确认"
反过来为错误背书。**枚举值必须抄来源，读回值只有在知道期望值时
才是判据。**

### 教训

- **修好一个真缺陷，不等于修好了这个故障。** 五处真缺陷改完现象
  不变，说明它们都不在断点上。现象不变时应该怀疑"改的地方不对"，
  而不是继续在同一段里找第六处。
- **自己标记为"未验证"的前提，要优先于"可能有问题"的代码。**
  路由这一条我很早就知道没验证过，却一直在改看得见的地方。
- **引导器留下的寄存器值，凡是本驱动要依赖的，都必须显式写一遍。**
  本轮的 MIPI 路由和 DSI 时钟分频都属于这一类。
- 现象不变时，值得回头问：这一轮的改动**本应**带来什么可观测的变化？
  `MODE_STATUS` 由 1 变 0 就是这样一个本不该被忽略的信号。

## 案例 13：SD 卡写不进去 —— 两个字节计数器把三种可能分开

### 背景

SD 卡（DesignWare MSHC，与 eMMC 的 SDHCI 不是同一种 IP）识别已经通过，
`/dev/mmcsd1` 就绪，读也正常，唯独写不进去：

    ERROR: DWMMC 等 DATA_OVER 超时
    mmcsd_writesingle: ERROR: CMD24 transfer failed: -5

### 关键：先造出能区分的观测

"等 DATA_OVER 超时"这一句现象，背后至少有三种完全不同的原因：

  1. 我根本没把数据推进 FIFO
  2. 推进去了，但 FIFO 没有往卡里排
  3. 都到了卡上，只是卡没给出结束信号

三者的修法毫不相干，靠猜要试很多轮。控制器正好有两个独立的字节计数器
可以把它们分开：

    TBBCNT (0x60)  主机 <-> FIFO 已传字节数
    TCBCNT (0x5c)  FIFO <-> 卡   已传字节数

把这两个数和 RINTSTS/STATUS 一起打进超时日志，一次读数就能定位。

### 三个真缺陷，依次被读数指出来

**其一：写命令的调用顺序（判据 BLKSIZ=0 BYTCNT=0）**

NuttX 写单块的默认顺序是

    CMD24 -> BLOCKSETUP -> WAITENABLE -> SENDSETUP

而 DW 控制器要求**发命令之前** BLKSIZ/BYTCNT 已经写好。按默认顺序，
sendcmd 执行时缓冲区还没挂上，长度寄存器是 0，命令就不带数据阶段。

NuttX 本来就为这种控制器留了开关：capabilities 报
`SDIO_CAPS_DMABEFOREWRITE`，mmcsd 就改成先 setup 再发命令。
★ 这个标志名字里带 DMA，实际控制的只是**调用顺序**，与用不用 DMA 无关，
仅看名字会以为不适用。

**其二：FIFO 地址算错（判据 TBBCNT=0）**

改对顺序后 BLKSIZ/BYTCNT 正常了，RINTSTS 出现 TXDR（控制器在要数据），
但 TBBCNT 仍然是 0 —— 数据一个字节也没进 FIFO。

我按**数据总线宽度**选的 FIFO 偏移（32 位 0x100 / 64 位 0x200）。
实际规则是按 **IP 版本**：

    dw_mmc.c:
      else if (host->verid < DW_MMC_240A)
              host->fifo_reg = host->regs + DATA_OFFSET;       // 0x100
      else    host->fifo_reg = host->regs + DATA_240A_OFFSET;  // 0x200

本板 VERID=0x5342270a，版本号取低 16 位 = 0x270a >= 0x240A，应该用 0x200。

同一份代码里还有一句注释点破了后果：240A 之后 0x100 变成了 CDTHRCTL
寄存器（"that register offset is in the FIFO region"）。也就是说，
我一直在把像素……不，把块数据写进 CDTHRCTL —— 不报错，但一个字节也
到不了卡上。

**其三：FIFO 深度取自 dtb 而非硬件**

水位设得超过实际深度，FIFO 永远达不到阈值。改用 FIFOTH 复位值反推的
实测值（复位时 RX 水位是 depth/2-1）。

### 教训

- **现象唯一、原因多样时，先造判据再动手改。** 这一轮三个缺陷都是靠
  一次读数定位的，没有一次是猜中的；对比 GMAC 那次（没有能区分的
  观测）来回猜了四轮仍未解决。
- **不要按"看起来合理"的规则去推寄存器布局。** FIFO 偏移按数据宽度
  分是个很自然的猜想，而且 0x100/0x200 这两个值恰好都存在，猜错了
  也不会报错。规则必须抄来源。
- **标志名会误导。** `SDIO_CAPS_DMABEFOREWRITE` 管的是顺序不是 DMA。
  拿不准时看它在框架里的**使用处**，比看名字可靠。

## 案例 14：GPIO 中断子项挂死 —— 缺回调不是返回错误，是直接断言

### 现象

`cmocka_driver_gpio` 四个子项里，`drivertest_gpio_interrupt` 先是
`fd_in < 0`（板上只注册了两个输出脚，没有 `/dev/gpio2`），补上输入脚后
改为断言失败并打印栈回溯：

    Assertion failed : at file: ioexpander/gpio.c:560

### 第一层：上层用 DEBUGASSERT 检查回调，不是返回错误

`drivers/ioexpander/gpio.c` 处理 `GPIOC_SETPINTYPE` 时：

    DEBUGASSERT(dev->gp_ops->go_setpintype != NULL);
    ret = dev->gp_ops->go_setpintype(dev, pintype);

缺回调不会得到 `-ENOTSUP`，而是直接断言 —— 现象是整个任务崩掉并打印
一大段栈回溯，和"功能不支持"完全不同。实现 `gpio_dev_s` 时
**go_read 一个不够**：只要用例会切引脚类型，
`go_setpintype` / `go_attach` / `go_enable` 就得一起给。

### 第二层：补齐回调后板子挂死

补上三个回调后，用例跑到中断子项时板子失去响应（串口无输出）。

选的输入脚是 TP_INT_L（GPIO0_C5）—— 触摸中断脚。选它的理由是板上有
10K 外部上拉、读它不干扰别的东西；但**忽略了触摸驱动也在用这一脚**：
`kickpi_k7_touch.c` 已经 attach 了自己的中断处理，用例再把它配成双边沿
并使能，两个使用者对同一根中断线做了不同的配置。

这与本项目早先那次"gpio3-3 既当触摸中断又是 GMAC1 PHY 复位"是同一类
错误：**选引脚时只看了电气特性，没查它是否已被占用。**

### 第三层：换脚之后仍然挂死

从厂商的 `rk3576-kickpi-k7c-extend-40pin.dtsi` 里取了 GPIO4_B3
（40 针扩展口，板上无器件），冲突消失 —— 四个子项都能启动，不再在
`setpintype` 处崩溃。但中断子项依旧让板子失去响应。

也就是说引脚占用只是**第一个**原因，中断路径本身还有问题。候选：

- 用例把引脚配成双边沿后自己驱动它产生边沿，而 `go_enable` 里
  attach 与 config 的先后可能不对
- bank4 的 GIC 中断号或 EOI 处理与 bank0 有差异（触摸用的是 bank0，
  工作正常；bank4 尚未被任何中断使用过）

**尚未定位**，本项不计为通过。

### 教训

- 板级资源要先查占用再使用。电气上可用 ≠ 空闲。
- 给测试用的输入脚应当选**确实空闲**的引脚（如未接器件的扩展口引脚）。
- 一个现象修掉一层原因后仍然存在，说明原来至少有两层。不要因为
  "找到了一个真原因"就认定问题已解决 —— 这一点在本项目的显示链路上
  已经吃过一次亏（案例 12：五个真缺陷都不是断点）。

---

## 案例 15：自配窗口出斜纹 —— 先找一个"已知正确"的原点

### 现象

接管 U-Boot 的显示链路后，把 ESMART1 图层扩成全屏（720x1280）并指向
自己 kmm 分配的帧缓冲，屏上的画面是错的，而且错得不一致：

| 往帧缓冲写的图案 | 屏上看到的 |
|---|---|
| 纯白 / 纯黑 | 正常 |
| 上半白、下半黑（按行变化） | 大致正常 |
| 左半白、右半黑、32 像素竖条纹（按列变化） | 斜纹 |
| 100x100 的方块 | 断续的横带 |

### 走过的弯路：在自己的配置里逐个寄存器试

一开始的做法是盯着自己写的那几个寄存器猜：是不是跨距的单位理解错了？
是不是缩放系数没设 1:1？是不是格式沿用了 U-Boot 的 RGB888？每猜一个就
改一次代码、烧一次固件、看一眼屏。烧一次几分钟，而候选有十几个。

更糟的是**这些猜测互相不独立**：改格式的同时跨距的含义也变了，一次改动
同时动了两个变量，结果无论好坏都推不出结论。

### 转折：U-Boot 的那组参数是唯一被实测证明可用的

出厂固件在这块屏上显示过 logo。它留在寄存器里的那组值——
`MST=0xfdf00000 VIR=654 ACT=654x270`——是这条链路上**唯一一份有实测背书**
的配置。于是：

1. 在驱动第一次覆盖之前把这六个寄存器存下来（`g_uboot_win`）；
2. 加一条 `rk3576_vop2_restore_uboot()` 随时退回去；
3. 给 `0xfdf00000` 起 4MB 补一条 MMU 映射，让 CPU 能往它的帧缓冲里写
   ——这块内存在 NuttX 的 64MB 之外，默认访问不到。

`cam ub 2` 的结果是**正的竖条纹**。这一步一次性排除了半张嫌疑名单：

- 显示通路（VOP2 -> DSI -> D-PHY -> 面板）没问题
- 写入侧、`up_flush_dcache` 的缓存刷回没问题
- 错的一定在 fb_setup 相对 U-Boot 改动的那几项之中

### 现在的做法：一次烧写，七次实验

`cam ub` 之后还剩五个变量同时在动：帧缓冲地址、宽、高、跨距、显示起点。
五个里任何一个错了，屏上都是"斜纹 + 断续横带"，看现象分不出是哪一个。

所以做了 `cam morph <0-6>`：以 U-Boot 的参数为原点，每一步只挪动一个
变量，改完就画竖条纹。第一个出斜纹的步骤直接指名元凶。

放大后的窗口一律仍然写进 U-Boot 那块帧缓冲（4MB 装得下 720x1280x4），
这样"换内存"和"换几何"才是两个独立的变量。

★ 关键的一点是**把实验做成运行时参数而不是编译期常量**。七个组合如果
靠改代码，就是七次烧写；做成 nsh 子命令，一次烧写全跑完。

### 顺带证伪的两条

- **格式不是 RGB888。** 按 TRM，ARGB8888 的 `vir_stride` 就等于像素宽，
  而 RGB888 是 `(w*3/4)+(w%3)`。U-Boot 的 VIR=654、窗口宽正是 654，
  只能是 ARGB8888。此前"U-Boot 用 RGB888 导致每行错开"的推断是错的。
- **寄存器偏移和字段位置全对。** 拿 TRM Part2 的 ESMART0 寄存器表逐条
  核过：`VIR`=0x1C、`ACT_INFO`=0x20（高度在 [28:16]、宽度在 [12:0]）、
  `SCL_CTRL`=0x30、`SCL_FACTOR_YRGB`=0x34（复位值 0x10001000 正是 1:1）。
  头文件里的定义与之一致，不是抄错芯片。

### 根因：DSP_ST 多加了一个消隐段

七次实验还没来得及跑，官方 SDK 到手了，答案直接在里面。

`DSP_ST`（显示起点）我们是这么算的：

```c
hact_st = VP1_HACT_ST_END >> 16;      /* = hsync_len + hback_porch */
vact_st = VP1_VACT_ST_END >> 16;
DSP_ST  = (vact_st << 16) | hact_st;  /* 把消隐段算了进去 */
```

厂商 SDK 里**两处互相独立的实现都不加消隐段**：

```c
/* u-boot/drivers/video/drm/rockchip_vop2.c  vop2_set_smart_win() */
dsp_stx = crtc_x;   dsp_sty = crtc_y;
dsp_st  = dsp_sty << 16 | (dsp_stx & 0xffff);

/* kernel-6.1 .../rockchip_drm_vop2.c  vop2_win_atomic_update() */
dsp_stx = dst->x1;  dsp_sty = dst->y1;
dsp_st  = dsp_sty << 16 | (dsp_stx & 0xffff);
```

RK3576 TRM 的字段说明也是这么写的 ——「Display image horizon/vertical
offset **in panel**」。**DSP_ST 是相对有效区的，全屏摆放就是 0。**

"加回消隐段"是 RK3568 的老写法（mainline 的 rk3568 路径里确实有
`dest->x1 + crtc_htotal - crtc_hsync_start`），照搬到 RK3576 就错了。

### 为什么这个错误会表现成斜纹

多加一个消隐量，后果不是"整幅图平移一点"：窗口的 `DSP_ST + DSP_INFO`
越过了有效区右边界和下边界，被绕回，于是每一行都比上一行多错开固定的
像素数。四个现象一次全解释了：

| 图案 | 现象 | 为什么 |
|---|---|---|
| 纯色 | 正常 | 整片同色，错位看不出来 |
| 上白下黑 | 大致正常 | 只对**行**方向敏感，水平错位不影响判断 |
| 竖条纹 / 左右分半 | 斜纹 | 每行水平错开固定量，累积成斜率 |
| 100x100 方块 | 断续横带 | 绕回把方块的每一行甩到不同的 x 上 |

而 U-Boot 那个 654x270 的窗口起点是它自己算的、落在有效区内，所以
`cam ub 2` 出的是正的竖条纹 —— 这也反过来印证了这条解释。

### 板上证实（2026-09-04）

诊断来自读代码，而"读代码得出的结论"和"在这块板上成立"是两件事，
所以留了 `cam morph 7` 把那个错误值单独写回去做对照。

本板 VP1 时序解出来是 **有效区起点 (47, 27)、尺寸 720x1280**，
也就是旧代码往 `DSP_ST` 里写的是 `0x001b002f`。

实测：

| `REGION0_DSP_OFFSET` | 屏上 |
|---|---|
| `0x00000000` | **正的竖条纹** |
| `0x001b002f` | **斜纹** |

两次的寄存器 dump 除这一个之外**逐字节相同** —— VIR=0x2d0、
ACT/DSP_INFO=0x04ff02cf、MST=0x405b0700、SCL_CTRL=0、PORT_SEL=1。
单变量对照成立，根因就是 `DSP_ST`。

顺带定下了一个先前只能猜的硬件行为：窗口超出有效区时 RK3576 的 VOP2
是**绕回**，不是裁掉。裁掉的话只会整幅右移 47 像素，竖条纹仍然是竖的。

★ 值得留意的是：写这条诊断时我一度把"绕回"讲得很肯定，其实当时并无
依据 —— 两种硬件行为都讲得通，只是其中一种恰好能解释现象。**"能解释
现象"不等于"被证实"**，中间差的就是这次对照实验。第 7 步的价值不在于
修好它，而在于把"改完好了"变成一个有对照组的结论。

### 教训

- 遇到"改了一堆东西之后不工作"，先找一个**已知正确的原点**，而不是在
  错误状态里逐个试。原厂固件留在寄存器里的值就是这样一个原点。
- 覆盖别人的配置之前先把它存下来。存一份的成本是六个 `getreg32`，
  不存的代价是唯一的正确答案被永久擦掉。
- 调试用的开关做成运行时参数。烧写一次的成本决定了"一次只改一个变量"
  这条原则能不能真的执行下去。
- **同系列芯片的驱动不能照搬。** DSP_ST 的语义 RK3568 与 RK3576 不同，
  寄存器名字、偏移、位宽全都一样，只有含义变了 —— 这类差异编译期和
  运行期都不会报错，只能靠比对厂商实现发现。
- **有厂商 SDK 就先读 SDK。** 这一条查了半天 TRM、设计了七步二分实验，
  而 SDK 里两处实现摆在那里，三分钟就能对出来。TRM 说的是寄存器"是
  什么"，SDK 说的是它"该怎么用"，后者才是驱动要抄的东西。
  SDK 位置见 `docs/refs/README.md`。

---

## 案例 16：板子每 95 秒自己重启 —— 一个把自己伪装成别人的缺陷

### 现象

开了看门狗 automonitor 之后，板子每 95 秒复位一次，**空闲时也一样**。
但一开始并不是这么发现的，而是一连串"某条命令之后板子重启了"：

    cam fbinfo 之后重启了     -> 去查帧缓冲 ioctl
    cam show 之后重启了       -> 去查送屏循环有没有越界
    cmocka_driver_oneshot 挂住 -> 去查 oneshot 用例

这三条追下去的结论**全是错的**。重启跟那条命令毫无关系，只是撞上了
95 秒的周期。

### 转折：先证明它是不是周期性的

停下所有猜测，什么命令都不发，挂在串口上静置 150 秒：

    [ 40s] 出现启动横幅
    [134s] 出现启动横幅

94 秒一次，与命令无关。这一个观察把前面三条线索一次性全部作废，也把
问题从"某个模块"改写成"某个所有人都依赖的东西"。

### 往下追：不是看门狗的问题，是时基的问题

95 秒 ≈ 看门狗 89.5 秒超时 + 启动时间 —— 狗没被喂。在喂狗函数里打日志，
一次都没进来。再往上一层，`work_queue(LPWORK, ..., 30 秒)` 返回 0，但
worker 永不执行。于是怀疑范围从看门狗扩大到"延时机制"：

    sleep 3   -> 25 秒不返回

**系统时基是死的。** 而且死得非常隐蔽：

| 依赖什么 | 表现 |
|---|---|
| 串口、nsh、所有命令 | 正常 —— 靠 UART 中断唤醒，不需要时基 |
| `up_mdelay()` | 正常 —— 忙等读计数器，不需要时基 |
| `sleep` / `usleep` | 永不返回 |
| 延时 `work_queue` | 永不触发 |

### 根因：/dev/oneshot 抢了调度器的定时器

板级注册 `/dev/oneshot` 时，下半部直接用了 `arm64_oneshot_initialize()`，
理由是"ARM 通用定时器是 SoC 无关的那个，本板不需要额外硬件"。理由没错，
错在**那个下半部不是空闲的**：

```c
/* arm64_arch_timer.c */
static struct oneshot_lowerhalf_s g_arm64_oneshot_lowerhalf;   /* 唯一实例 */
void up_timer_initialize(void)
{ up_alarm_set_lowerhalf(arm64_oneshot_initialize()); }        /* 内核已占用 */
```

`struct oneshot_lowerhalf_s` 只有**一对** `callback/arg`，谁后设谁赢。
注册 `/dev/oneshot` 顶掉了调度器的定时回调；第二次调
`arm64_oneshot_initialize()` 里的 `set_compare(UINT64_MAX)` 还会当场取消
已挂起的闹钟。

单变量对照（只摘掉这段注册）：

| | 注册 | 摘掉 |
|---|---|---|
| `sleep 3` / `sleep 8` | 25s 不返回 | 3.3s / 8.4s |
| 喂狗 | 从不发生 | 每 30 秒一次 |
| 自发重启 | 每 95 秒 | 120 秒内 0 次 |

### 修法：给 /dev/oneshot 一路自己的硬件

`rk3576_timer.c`，TIMER_NS_0 的 CH0 做闹钟、CH1 做自由运行计数器。
CH1 不接中断，也就不需要知道它的 GIC 号（设备树只声明了 CH0 的）——
**用一个不需要中断的通道当计数源，就绕开了一个没有出处的假设。**

两条只有 TRM 有、Linux 驱动没有的信息：通道间距 **0x1000**（不是设备树
reg 长度暗示的 0x20），CONTROL **bit3 选计数方向**（Linux 匹配的
`rockchip,rk3288-timer` 从不碰这一位，因为老芯片只能向下数）。

### 教训

- **"某个操作之后出问题"要先证明因果，再查原因。** 最省事的证法是把那个
  操作**不做**：静置观察。这一步花 150 秒，省掉了三条错误的排查线。
- **广播式的故障先怀疑公共设施。** 时基、时钟、内存、堆 —— 这些坏了会
  在离现场很远的地方发作。本项目这一节里连着两次：CIF 越界写坏堆，
  崩在 `mm_malloc`；时基死掉，表现成"某条命令让板子重启"。
- **复用"现成的下半部"之前先问它有没有主人。** 单例 + 单回调槽的结构，
  第二个使用者不会得到错误，只会安静地顶掉第一个。

### ★ 附带的一个方法论错误：抓取工具把结论带偏了两次

排查中用的串口抓取脚本是"读到连续 N 秒无新字节就返回"。这个策略会在
输出有停顿时提前收工，**把后半段吃掉**。因此出现过两次错误结论：

- 看到 `[ RUN ] drivertest_oneshot` 之后没有下文，判定"用例挂死"——
  实际上它跑完了，只是汇总行被吃掉；
- 看到 `INTSTATUS=0x00000000` 而 ISR 计数那几个字丢了，判定"定时器没
  产生中断"，据此在代码里写下"使能位必须与模式位分两次写"的注释。
  后来单独对比过，两种写法都能触发 —— **那条注释是错的，已改正。**

第二次尤其糟糕：它把一个没有依据的结论写进了代码注释，而注释会被后来
的人当作已经验证过的事实。改用后台连续抓取（`cap.py`）之后不再丢字。

**判据的可靠性本身也要验证。** 用不可靠的观测去下结论，比没有观测更坏 ——
没有观测至少知道自己不知道。

---

## 案例 17：网口不通 —— 把"配置对了"当成"信号出来了"，以及一次因果倒置

### 现象

两个网口用网线对接，`rk3576_gmac_probe()` 两个端口都以 `-110` 失败。
`DMA 软复位超时`，采样到 RXCLK/TXCLK 恒低。

### 走过的弯路（按发生顺序）

**① 因果倒置：把 RXCLK 当成根因。**

日志显示 DW GMAC 的 DMA 软复位在等时钟，而 RGMII 的 RXCLK 由 PHY 送来，
于是我写下"RXCLK 是唯一根因"，接着几轮都在查"为什么没有 RXCLK"。

但 **RGMII 的 RXCLK 是 PHY 在链路建立之后才输出的**。所以"没有 RXCLK"
是链路建不起来的*结果*。顺着结果往上游查，方向是反的，怎么查都到不了头。

> 时钟类信号"没有"时，先问它由谁产生、在什么条件下产生。

**② 从注释推断硬件，得出相反结论。**

`rk3576-kickpi-ethernet-gmac0.dtsi` 里 pinctrl-0 把 `&ethm0_clk0_25m_out`
注释掉了。我据此断定"PHY 自带晶振，不需要 SoC 送 25MHz"。

错了。被注释掉的只是**引脚组**；时钟是通过 PHY 节点的
`clocks = <&cru REFCLKO25M_GMAC0_OUT>` 属性经 CRU 使能的，
`dwmac-rk.c` 的 `gmac_clk_enable()` 里有 `clk_prepare_enable(clk_phy)`，
位置在 `set_to_rgmii()` 与放 PHY 复位**之前**。

> dts 里一处被注释，只说明"这条路径不走"，不说明"这个功能不存在"。
> 同一个功能可能有第二条实现路径。

**③ 把"寄存器回读正确"当成"信号出来了"。**

补上 25M 之后我看到 `回读 div=39 sel=1 引脚功能=3`，就宣布 25M 就绪。
但那只证明 CRU 和 IOMUX 的位写进去了。真正的判据是**焊盘上有没有跳变**：
复用态下 GPIO 输入缓冲仍能读到焊盘电平（RXCLK/TXCLK 的采样用的就是这个
办法），采样 200 次数跳变即可。实测 g3-4 跳变 6 次、g2-30 跳变 7 次，
这才算证实。

顺带：使能前读到 `div=39 sel=1` —— 分频和选源**本来就是** CPLL/40=25MHz，
只有引脚是 GPIO。说明 U-Boot 已经配过一部分。

**④ 又一次"没验证就下结论"，而且立刻被自己的数据打脸。**

PHY 软复位后 BMCR 读回 `0x0000`，我判断"写函数把 0 写进去了"。
但随后加的写通道自检显示 ANAR 写 `0x0141` 读回 `0x0141`，写 BMCR=0x1200
读回 `0x1000`（bit9 重启位自清属正常）——**写通道完全正常**，判断是错的。

代价其实很小（多一轮烧录），因为这次是先做自检再下结论。前几次是先下
结论再验证，代价就是好几轮走错方向。**顺序本身就是方法。**

### 修掉的真缺陷

| 缺陷 | 后果 |
|---|---|
| 引脚表只有 eth0m0 一份，pinmux 块写死 `if (port == 0)` | GMAC1 的引脚从未配置，MDIO 读回全 0 |
| 时钟采样循环写死 `GMAC0_PIN_BANK`/`clkpins[]` | GMAC1 报的是 GMAC0 的引脚状态——一份张冠李戴的读数 |
| 注释称"GMAC1 在 CLKSEL_CON(30) 的相邻位域"，且整段只在 port==0 执行 | GMAC1 的 125M 分频从未设过；实际在 **CON(31)[4:0]** |
| 从未使能 `REFCLKO25M_GMACn_OUT`，25M 引脚停在 GPIO | PHY 参考时钟没接出去 |
| 无 MDIO 写函数 | 无法做 PHY 层实验 |

其中第二条最值得记：**错误的读数比没有读数更危险。** 它看起来很具体，
会被当成证据。做对照实验前要先确认对照组是活的。

### 当前状态：固件侧已逐项证实，剩下的是物理层

| 判据 | 结果 |
|---|---|
| MDIO 地址扫描 0~31 | 只有 addr=0 应答，ID=`0x7b744411` —— 总线健康、地址正确、单颗 PHY |
| MDIO 写通道自检 | ANAR 写 `0x0141` 读回 `0x0141`，正常 |
| PHY 状态 | `BMCR=0x1140`：自协商开、无 power-down、无 isolate |
| `BMSR=0x7949` | 逐位解出为一颗正常的千兆 PHY（100/10 全半双工 + 扩展状态） |
| 本端通告 | `ANAR=0x01e1`、`GBCR=0x0200`（通告 1000-T 全双工） |
| 25M 参考时钟 | 两个焊盘均实测到跳变 |
| 两颗 PHY 复位 | 均已释放（GMAC1 的 GPIO3_A3 此前定义了却从没被调用） |
| 软复位 + 强制重启自协商，持续 6 秒 | `LP(reg5)=0x0000`、`ANER(reg6) bit0=0` |

最后一行是关键：**ANER bit0 = 0 表示"从未收到对端的 FLP"**，
`LP=0x0000` 表示对端能力寄存器空。两个口都是如此。

即：本端 PHY 活着、配置正确、参考时钟就位、自协商在跑，**但线上一点
对端能量都收不到**。这已经不是寄存器配置问题。

### 待排除（物理侧）

- 网线是否真的插到位、是否是好线
- 把其中一个口接到已知可用的路由器/交换机/PC —— 若灯亮且 BMSR bit2 置位，
  说明本端模拟侧正常，问题在两口对接这个组合
- 25M 焊盘（GPIO3_A4 / GPIO2_D6）是否真的走线到 PHY —— dtsi 把它注释掉，
  可能意味着这块板的 PHY 另有晶振，那这一路是空的（无害但也无用）

### 尚未配置（等链路起来再做，避免一次动多个变量）

RGMII 的 TX/RX 延时线，在 `ioc_grf`（0x26040000）：
GMAC0 用 CON2/CON3（+0x6408/+0x640c），GMAC1 用 CON4/CON5（+0x6410/+0x6414）；
dtsi 给的是 `tx_delay = <0x21>`（GMAC0）/ `<0x20>`（GMAC1），
`phy-mode = "rgmii-rxid"` 即关掉 SoC 内部的 RX 延时。
延时只影响数据采样，不影响链路建立，所以放在链路之后调。

---

## 案例 18：TFTP put 必崩 —— 只有"能工作的服务器"才暴露得出来的空指针

**现象**：板子 `put` 到主机就 panic 复位。主机侧只落下一个 0 字节的文件。

```
default_fatal_handler: (IFSC/DFSC) level 2 translation fault
ESR_ELn: 0x96000046      FAR_ELn: 0x0      ELR_ELn: 0x404b69cc
```

`FAR_ELn = 0`、ESR 的 WnR 位是写 —— 往地址 0 写。`addr2line` 落在
`apps/netutils/tftpc/tftpc_put.c:233`：

```c
*blockno = rblockno;          /* blockno 是 NULL */
```

**根因**：同文件 315 行，等 WRQ 的第一个 ACK 时故意传了 `NULL` —— 那个块号
必然是 0，调用方不关心。但 `tftp_rcvack()` 无条件解引用。

**为什么一直没被发现**：

> 这个缺陷需要一个**真的会回 ACK 的服务器**才触发。所有"传给不可达
> 主机"的测试都干净地返回 `ENETUNREACH`，因为那条路收不到 ACK，
> 永远走不到出错的那一行。

我自己也差点被这一点骗过去：先用不可达地址试，看到干净的错误返回，
一度判断 `put` 本身没问题。**"错误路径工作正常"不能推出"成功路径工作
正常"** —— 恰恰相反，只在成功路径上的代码，被错误路径的测试完整地
遮蔽着。

已修，归档为 `bsp/upstream/tftpc-null-blockno.patch`。

---

## 案例 19：V4L2 的 S_FMT 全部返回 EINVAL —— 两套命名空间的值在比较

**现象**：`/dev/video0` 注册成功、能 open、`VIDIOC_QUERYCAP` 正常，但
`VIDIOC_S_FMT` 无论什么尺寸都返回 EINVAL —— 连传感器的原生尺寸
1932x1096 也一样。

**歧路**：我先假设是尺寸不被接受（ai_agent 要 1280x720，而我们只声称
支持 1932x1096），花时间做了"采集尺寸 / 输出尺寸分离 + 缩放"。那个改动
本身是必要的，但**它不是当前失败的原因** —— 原生尺寸也失败这一条，
本来就该立刻推翻"尺寸问题"这个假设。看到"所有输入都失败"时，
应该先找**与输入无关**的共同因子。

**根因**：上半部在下发之前会翻译格式：

```c
convert_to_imgsensorfmt()  V4L2_PIX_FMT_JPEG -> IMGSENSOR_PIX_FMT_JPEG (2)
convert_to_imgdatafmt()    V4L2_PIX_FMT_JPEG -> IMGDATA_PIX_FMT_JPEG   (2)
```

所以 `validate_frame_setting` 收到的是 `IMGSENSOR_PIX_FMT_*` / 
`IMGDATA_PIX_FMT_*` 这套 **0..10 的小整数枚举**，而我们拿 V4L2 的
fourcc（`V4L2_PIX_FMT_JPEG` = 'JPEG' 的 32 位值）去比，永远不相等。

**顺带纠正一个更早的错误设计**：这套枚举里**没有 Bayer RAW**，
`capture_try_fmt` 的 switch 也不认 `V4L2_PIX_FMT_SBGGR10`，会落到
`default: return -EINVAL`。也就是说"通过 V4L2 出 RAW"在这个框架版本里
**不可表达**。我原先写的 SBGGR10 分支是死代码，而且是有害的死代码 ——
它让接口看起来支持一件其实做不到的事。已删除；RAW 仍走板级 `cam` 命令。

**教训**：跨层接口上，**类型名相同不代表值域相同**。`pixelformat` 这个
字段名在 V4L2、imgsensor、imgdata 三层都叫同一个名字，值域却是三套。
编译器不会报错，因为都是整数。


---

## 案例 20：LLM 明明回了，agent 却报超时 —— 用墙钟量耗时，撞上 TLS 改钟

**现象**：`ask` 之后日志里两行紧挨着，互相矛盾：

```
[llm] Response: 72 bytes text, 0 tool calls, finish=end_turn
[agent] LLM watchdog: call took 2747053661 ms (limit 60s), treating as timeout
[trace] END status=timeout iters=1 elapsed=1772273579s
```

响应**已经成功收到了**，看门狗却把它当超时丢掉。2747053661 ms 是 31 天，
`elapsed` 是个 Unix 纪元量级的数 —— 这两个数字本身就是"时钟跳变"的指纹，
不是"网络很慢"能产生的量级。

**根因**：同一次调用里有两处对时间的操作打架。

```
vela_tls.c:258   clock_settime(CLOCK_REALTIME, ...)   ← 握手前把墙钟往前推
agent_loop.c     gettimeofday(&tv_start) ... gettimeofday(&tv_end)
```

日志里 `[vela_tls] Clock too old, forcing to 26...` 就是那次推。板子上电时
墙钟从 0 开始，比服务器证书的 notBefore 还早，TLS 层为了让校验通过强行
把钟设成一个合理值。起始时间戳取于跳变之前、结束时间戳取于之后，
差值就是跳变本身的大小。

**修法**：耗时一律用 `CLOCK_MONOTONIC`。agent_loop.c 里这 10 处
`gettimeofday` 全是"起止相减"，**语义上本来就不该用墙钟** —— 除了 TLS
改钟，NTP 校时、手动设日期落在调用中间都会产生同样的错误。已改为
`mono_timeofday()`，归档为 `bsp/upstream/agent-monotonic-latency.patch`。

**教训**：`gettimeofday` 回答的是"现在几点"，不是"过了多久"。这两个问题
在墙钟稳定时答案恰好一致，所以错误用法能长期不暴露 —— 直到某个组件
合法地调了一次钟。**一个只在"另一件正确的事发生时"才出错的缺陷，
最难归因**：这里改钟是 TLS 该做的、响应是成功的，唯一错的是计时方式。

---

## 案例 21：自动化替用户"顺手做一步"，把可恢复变成不可恢复

**现象**：`flash.sh` 触发不了下载模式。串口一看，板子停在 ai_agent 的
`vela>` 提示符 —— 脚本发的 `loader` 被 agent 当成未知命令吃掉了，
所以板子根本没进下载模式，而失败要到 `rkdeveloptool` 找不到设备时
才暴露出来。

诊断是对的。**接下来做错了**：我在脚本里加了"先发 quit 退出 agent"。
结果 agent 退出时把控制台一起带走了 —— 串口从此零回显，USB 也不枚举，
板子既没进下载模式也回不到 nsh。原本"手动敲个 quit 再跑一次"就能解决，
变成必须按 RESET。

**教训**：自动化在**前台状态不确定**的地方替用户动手，风险不对称。
成功省下一次手动输入，失败搭进去一次物理操作 —— 而脚本恰恰无法确认
前台是什么。现在脚本只**报告**"提示符是 vela> 而不是 nsh>，先敲 quit"，
把这一步留给能看见屏幕的人。


---

## 案例 22：三次误判同一件事 —— "看起来没输出"有三种完全不同的原因

跑过 ai_agent 之后板子无法烧录。同一个表象，我连着给出三个诊断，
前两个都是错的。

**第一次（错）**："控制台死了。" —— 实际是 **agent 的微信通道每秒刷屏**
把提示符淹了，而收尾逻辑是"等连续 N 秒没有新字节"，刷屏从不间断，
于是永远等不到收尾、看起来一片空白。串口一直在收字节。

**第二次（错）**："`quit` 会带走控制台，所以别用。" —— 这是纯猜测，没有
依据。我据此在 flash.sh 里改用 `restart`，把一个可恢复的情况变成了
必须按 RESET。

**第三次（还是错）**：读了 `cmd_quit()`，看见它 `break` 出 CLI 循环，
就宣布"验证过了，quit 是安全的"。**但"函数里 break 了"不等于"进程
退出了"**：`agent_request_shutdown()` 只是置 `g_shutdown_requested`
标志，weixin 等后台线程根本不检查它，进程不下来，NSH 也就拿不回
控制台 —— 表现为**只出不进**：日志照打，输入毫无回显。

**真实分类**：串口"没反应"至少有三种，必须先分清再动手。

| 现象 | 含义 | 处置 |
|---|---|---|
| 收到 0 字节 | 板子挂死或在下载模式 | 查 USB 是否枚举 |
| 收到日志、输入无回显 | 前台进程占着控制台但不读 | 只能复位 |
| 日志刷屏、看似无输出 | 收尾逻辑等不到静默 | 改成定时读 + 过滤 |

**教训**：
1. 读代码能证伪，但**证实要看到端到端的行为**。看见 `break` 只证明了
   循环退出，没证明进程退出、更没证明控制台交还。
2. 前两次误判的共同点是**在够不着板子的时候继续推断**。诊断的每一步
   都该有一个能观察到的判据；没有判据时该停下来要一次物理操作，
   而不是换一个同样不了解的命令再试。

**操作规律**：跑过 ai_agent 之后，烧录前需要一次物理复位。所以 agent
的验证要安排在一轮烧录测试的**最后**。


---

## 案例 23：把未验证的硬件启动流程放进启动路径，代价是整块板子进不去

**经过**：蓝牙固件加载（`load_bcm4343x_firmware`）接在
`board_app_initialize()` 里。开启 UART 硬件流控之后，它在等一个不会来的
CTS，**永远不返回**。

而 `board_app_initialize()` 跑在 nsh 任务上下文 —— 它一卡，控制台就起不来：
串口零字节、`loader` 命令发不进去、USB 也不枚举下载设备。板子只能靠
**MASKROM** 恢复，而不是按一下 RESET（RESET 只会再启动同一个卡死的固件）。

**这是设计问题，不是调试问题**。风险完全不对称：

| | 放启动路径 | 做成命令 |
|---|---|---|
| 成功时收益 | 省一条 `bt init` | — |
| 失败时代价 | 整块板子进不去，要 MASKROM | 那条命令不返回，板子照常可烧 |

**尤其讽刺的是**：同一轮里做 HDMI 探针时，我专门为"访问未上电外设会总线
挂死"写了防护、并把顺序编进注释；轮到蓝牙却把一个**同样未经验证**的流程
直接挂进了启动。防的是具体的那一个坑，没有把"未验证的东西不进启动路径"
提炼成规则。

**规则**：新硬件的 bring-up 一律先做成手动命令（如 `hdmi probe`、
`bt init`、`cam` 系列），验证稳定之后再考虑进启动路径。启动路径上的每
一步都必须是**已经证明会返回**的。


---

## 案例 24：一个"决定性判据"本身是假阳性 —— 和驱动抢寄存器

调蓝牙无应答时，我加了 16550 的内部回环自测（MCR bit4），理由是它
**完全不经过引脚和模组**，能一句话分开"我们发不出去"和"模组不应答"。

结果读回 0x00，我据此宣布："UART4 自己就不通，前面查的电源、复位、时序
全是错方向。"

**这个结论是错的。** 补一行寄存器对照就看穿了：

```
UART4@2ad70000: LSR=60 MCR=00 USR=06 | UART0 对照: LSR=60
```

`LSR=0x60`(THRE+TEMT) 与**已知可用**的 UART0 完全一致 —— UART4 有时钟、
能访问。回环失败的原因是：`/dev/ttyS1` 已经注册，NuttX 串口驱动的接收
中断开着，回环把字节送回 RBR 的瞬间**驱动的 ISR 先把它取走了**，轮询
自然读到 0。我和驱动在抢同一个寄存器。

**教训**

1. **判据也要有判据。** 我给回环的理由（"不经过引脚"）只论证了它的
   *特异性*，没论证它的*有效性* —— 在一个已被驱动接管的设备上做裸寄存器
   轮询，前提根本不成立。加判据时要问："这个测试在当前环境下成立吗？"
2. **对照组比绝对值有用得多。** 单看"读回 0x00"什么也说明不了；和一个
   **已知正确**的对象（UART0）并排一放，答案立刻出来。这和案例 15
   「先找一个已知正确的原点」是同一件事，我又忘了用。
3. 假阳性的代价不只是白跑一轮 —— 它让我**回头否定了前面正确的排查**
   （电源、复位、时序），差点把有效结论一起丢掉。


---

## 案例 25：dtb 里写着芯片型号，而它是错的 —— 三天的方向由一个字符串决定

**现象**：蓝牙 HCI Reset 永远无应答，WiFi 的 SDIO CMD5 拿不到有效 OCR。

**我做过的排查**（全部有回读判据，全部通过）：

| 环节 | 判据 |
|---|---|
| PMIC 供电 | RK806 EN0-EN5 = `0f 0f 03 0f 0f 07`，所有 BUCK/LDO 使能 |
| UART4 时钟复位 | `LSR=0x60`，与已知可用的 UART0 一致 |
| ttyS1→UART4 | 写入后 `TFL` 由 0 变 1 |
| 引脚复用 | TX/RX/CTS/RTS 回读全为 9 |
| 使能脚电平 | BT_RST/BT_WAKE/WIFI_EN 回读全为 1 |
| 波特率 | divisor=13 → 115384bps |
| 上电时序 | 逐条照抄原厂 `rfkill-bt.c` |

**每一条都对，模组就是不应答。** 于是刷回原厂 Android 做对照 —— 一行日志
就结束了这一切：

```
skw_sdio_probe: vendor=0x1ffe, device=0x6621, clock=200000000
lsmod: swt6621s_wifi / skw_sdio_lite
```

板上是 **SeekWave SKW6621S**，不是 Broadcom。而且 **WiFi 和蓝牙都走
SDIO，没有 UART 蓝牙**。原厂 Android 里 `/sys/class/bluetooth/` 是空的。

**根因**：我的芯片识别来自原厂 dtb 的

```
/wireless-wlan   wifi_chip_type = "ap6256"
```

这一句**是错的**（多半是从参考设计抄来没改）。基于它，我：
- 从 SDK 取了 BCM4345C5.hcd 并编进镜像（70KB，白搭）
- 照抄了 Broadcom 的 HCI-over-UART 上电时序
- 为 UART4 配了时钟、复位、引脚、流控
- 在"模组为什么不应答"上做了七项交叉验证

**教训**

1. **dtb 里的"事实"分两类，可信度差很远。**
   `reg`/`interrupts`/`clocks`/`pinctrl` 是**驱动实际使用**的，写错了系统
   就跑不起来，所以高度可信 —— 这个项目一路靠它们拿到的参数从没出过错。
   而 `wifi_chip_type` 这种**给用户态当提示**的字符串，没有任何东西会
   校验它，抄错了也照样能跑。**取参数看前者，认型号别信后者。**

2. **认型号要用"会被硬件打脸"的证据**：SDIO 的 vendor/device ID、I2C 探
   到的芯片 ID、驱动模块名 —— 这些错了就工作不了。字符串不是证据。

3. **对照组早该上。** 用户在调网口时教过同一招（"刷回去看看 Linux 能不能
   把网口拉起来，就能一刀切开固件缺配置和硬件问题"），那次奏效。这次我
   把它排在了所有软件排查之后，而它本该排在**第一位** —— 它一次就能同时
   回答"硬件好不好"和"这到底是什么芯片"，成本只是一次烧录。

4. 前面那七项验证并没有白做：它们把"我们这一侧"彻底排除干净了，正是这个
   干净的排除让"那就不是我们的问题"成为可信的判断，才促成了刷回原厂。
   **但顺序错了**：应该先用对照组定性，再用这些判据定位。

