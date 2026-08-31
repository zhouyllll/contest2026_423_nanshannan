# 上游 PR：arm64 通用层的 HCR_EL2 交接缺陷

本文备好一个可直接提交到 `open-vela/nuttx` 的 PR。补丁本体见
`bsp/upstream-hcr-el2.patch`（基线 `dd92bcf4`，2 文件 24 行）。

这不是 RK3576 的特例，而是 `arch/arm64` 通用层的问题：任何从运行在
EL2 的 bootloader（U-Boot 是最常见的一种）进入 NuttX 的 arm64 平台
都会踩到。我们在适配 KICKPI-K7 的过程中先后被同一处代码坑了两次。

## 缺陷

`arch/arm64/src/common/arm64_boot.c` 的 `arm64_boot_el2_init()` 里，
相邻的三个系统寄存器写法不一致：

```c
/* SCTLR_EL2：从 0 重新构造 */
reg = 0U;
reg = (SCTLR_EL2_RES1 | SCTLR_I_BIT | SCTLR_SA_BIT);
write_sysreg(reg, sctlr_el2);

/* HCR_EL2：read-modify-write，只往上"或" ← 问题在这里 */
reg = read_sysreg(hcr_el2);
reg |= HCR_RW_BIT;
write_sysreg(reg, hcr_el2);

/* CPTR_EL2：从 0 重新构造 */
reg = 0U;
reg |= CPTR_EL2_RES1;
reg &= ~(CPTR_TFP_BIT | CPTR_TCPAC_BIT);
write_sysreg(reg, cptr_el2);
```

只有 `HCR_EL2` 把 bootloader 的残留状态原样继承进 OS。Linux 在等价
位置是覆盖写，不保留引导器状态。

## 后果：两个独立的启动失败

| 残留位 | 现象 | 何时暴露 |
|---|---|---|
| `TGE`（bit 27） | 架构规定 TGE=1 时禁止 `eret` 到 EL1，EL2→EL1 切换直接触发 Illegal Exception Return（ESR EC=0xE） | 极早，任何 C 代码之前 |
| `IMO`（bit 4）<br>`FMO`/`AMO`（bit 3/5） | 物理 IRQ/FIQ/SError 仍路由到 EL2，OS 打开中断后第一次中断跳进 bootloader 早已失效的处理路径 | 很晚，OS 完整初始化后、空闲循环放行第一个 timer tick 时 |

第二个尤其难查：内核堆、GIC、arch timer、`nx_bringup()` 全部正常返回，
进空闲循环打印出第一个字符才崩，且崩溃转储是 **bootloader 的格式**
而非 NuttX 的。

## 定位依据

`IMO` 那一例的判定不依赖猜测。进空闲循环前读三个寄存器：

| 寄存器 | 实测 | 判读 |
|---|---|---|
| `VBAR_EL1` | `0x404a7000` | 等于 `nm nuttx` 的 `_vector_table`，向量表正确 |
| `CurrentEL >> 2` | `1` | 确实在 EL1 |
| `DAIF` | `0x240` | D=1、F=1、A=0、**I=0（IRQ 已打开）** |

向量表正确、异常级正确、IRQ 已使能，异常却被 EL2 接住 —— 在 ARMv8
架构上只剩 `HCR_EL2.IMO=1` 一种解释。

## 修复

保持现有的 read-modify-write，只显式清掉四个不能带进 EL1 的位：

```c
reg &= ~(HCR_TGE_BIT | HCR_FMO_BIT | HCR_IMO_BIT | HCR_AMO_BIT);
```

`HCR_TGE_BIT` 此前未定义，补在 `arm64_arch.h` 里 `FMO/IMO/AMO` 旁边。

**为什么不会影响其它平台**：今天能正常启动的平台，这四位必然已经是 0，
因此这次清除在那些平台上是空操作。该论据不依赖任何测试覆盖率。

选择"显式清位"而非"像 Linux 那样整体覆盖写"，是因为前者改动面最小、
风险可证；如果 maintainer 更倾向后者，也可以改。

## 板上验证

在 KICKPI-K7（RK3576，U-Boot 2017.09 运行于 EL2）上：

1. **仅打通用层补丁、SoC 层零兜底** —— 把 `rk3576` 的
   `arm64_el_init()` 清空，只保留本补丁；
2. 启动到 NuttShell 并可交互：

```
- Boot from EL2
- Boot from EL1
- Boot to C runtime for OS Initialize

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 118f003d-dirty Aug 31 2026 arm64 kickpi-k7
```

复现分支：`verify-upstream-hcr`（= `rk3576-bsp` + 本补丁 − SoC 层兜底）。

## 提交方式

前置条件：已签署 CLA（`https://openvela.com/#/community/cla`，用报名的
GitHub 账号），否则 `cla/signature` 检查不通过、PR 无法合入。

```sh
# 1. fork open-vela/nuttx 后
git remote add fork git@github.com:<你的账号>/nuttx.git
git push fork upstream-hcr-el2

# 2. 在 GitHub 上发起 PR：
#    base:    open-vela/nuttx  dev-ai-contest-2026
#    compare: <你的账号>/nuttx  upstream-hcr-el2
```

PR 标题与正文直接用补丁里的 commit message（已按 NuttX 规范写好，
英文，含背景、根因、影响面与不会回归的论证）。

若 `cla/signature` 未过，签完 CLA 后在 PR 下评论 `/check-cla` 触发复检，
无需重建 PR。

## 与本队 BSP 的关系

`arch/arm64/src/rk3576/rk3576_boot.c` 里保留了同样的清理动作。上游补丁
合入后它即成冗余，但保留可使本 BSP 在未打补丁的上游分支上独立工作，
因此暂不移除，并在注释中注明了这层关系。
