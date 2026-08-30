# Amlogic Meson UART（`meson-s4-uart`）寄存器规格

面向 openvela / NuttX 驱动开发的硬件规格整理。**目标平台：LogicPi A1（四核 Cortex-A510）。**

## 来源与合规声明

本文档是**硬件寄存器规格的事实性整理**，用于在 openvela 上独立实现驱动。

| 事实 | 来源 | 许可证 |
|---|---|---|
| 控制器地址、中断号、时钟、`compatible` | Linux 主线 `amlogic-s6.dtsi` | `GPL-2.0+ OR MIT`（按 **MIT** 分支使用） |
| 寄存器偏移、位定义、波特率算法 | Linux 主线 `drivers/tty/serial/meson_uart.c` | GPL-2.0 |

**对第二项的处理方式**：仅阅读理解其描述的**硬件行为**（哪个偏移是什么寄存器、
哪一位代表什么状态、分频怎么算），据此用自己的语言写成本规格，
再按 openvela 驱动模型独立实现。**不复制任何源代码**。

寄存器偏移与位定义属于硬件接口事实，不属于该驱动的著作权表达。
本项目的实现不派生自其代码结构。该口径已在《项目描述》第五章第 5 条声明。

> ⚠️ 本文所有数值来自主线代码与设备树，**尚未在 LogicPi A1 实机验证**。
> 上板后须按 [`RECON.md`](../RECON.md) A5 用真实设备树与 `/proc/cmdline` 复核。

---

## 1. 控制器基本信息

| 项 | 值 | 出处 |
|---|---|---|
| 基址（参考） | `0xfe07a000` | `amlogic-s6.dtsi`：apb `0xfe000000` + `uart_b` `0x7a000` |
| 寄存器区长度 | `0x18`（6 个 32 位寄存器） | dtsi `reg` |
| 中断号 | **IRQ 201**（`GIC_SPI 169` + 32） | dtsi |
| **中断触发方式** | ★ **边沿触发**（`IRQ_TYPE_EDGE_RISING`） | dtsi |
| 时钟 | `xtal` = **24 MHz**（xtal / pclk / baud 三路同源） | dtsi |
| 别名 | `serial0` | 板级 dts |

> ★ **边沿触发要特别注意。** 多数平台的串口是电平触发，处理程序只要
> "有事就读干净"即可；边沿触发要求**在清空 FIFO 之前不能漏掉新到达的边沿**，
> 否则会丢中断导致接收假死。中断处理里必须循环读到 `RX_EMPTY` 置位为止。

---

## 2. 寄存器映射

| 偏移 | 名称 | 读写 | 用途 |
|---|---|---|---|
| `0x00` | **WFIFO** | W | 发送数据（写入即入 TX FIFO） |
| `0x04` | **RFIFO** | R | 接收数据（读出即从 RX FIFO 取一字节） |
| `0x08` | **CONTROL** | RW | 收发使能、帧格式、复位、中断使能 |
| `0x0c` | **STATUS** | R | 状态标志与错误标志 |
| `0x10` | **MISC** | RW | 收 / 发中断的 FIFO 阈值 |
| `0x14` | **REG5** | RW | 波特率设置 |

---

## 3. CONTROL（`0x08`）位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 12 | TX_EN | 发送使能 |
| 13 | RX_EN | 接收使能 |
| 15 | TWO_WIRE_EN | 两线模式（无流控），调试串口置 1 |
| 17:16 | STOP_BIT_LEN | `0`=1 位停止位，`1`=2 位停止位 |
| 18 | PARITY_TYPE | 校验类型（0/1 对应 偶/奇） |
| 19 | PARITY_EN | 校验使能 |
| 21:20 | DATA_LEN | `0`=**8 位**，`1`=7 位，`2`=6 位，`3`=5 位 |
| 22 | TX_RST | 发送通道复位（写 1 触发，需再清 0） |
| 23 | RX_RST | 接收通道复位（写 1 触发，需再清 0） |
| 24 | CLEAR_ERR | 清除错误标志 |
| 27 | RX_INT_EN | 接收中断使能 |
| 28 | TX_INT_EN | 发送中断使能 |

> 注意 DATA_LEN 的编码**不是**位宽数值本身：`0` 代表 8 位。写成 8 会得到 6 位。

---

## 4. STATUS（`0x0c`）位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 16 | PARITY_ERR | 校验错误 |
| 17 | FRAME_ERR | 帧错误 |
| 18 | TX_FIFO_WERR | 发送 FIFO 写错误（满时仍写入） |
| 20 | **RX_EMPTY** | 接收 FIFO 空 → **为 0 时才有数据可读** |
| 21 | **TX_FULL** | 发送 FIFO 满 → **为 0 时才可写入** |
| 22 | TX_EMPTY | 发送 FIFO 空 |
| 25 | XMIT_BUSY | 移位寄存器仍在发送中 |

错误位合集：`PARITY_ERR | FRAME_ERR | TX_FIFO_WERR`，检测到后经 CONTROL 的
CLEAR_ERR 清除。

**"发送真正完成"的判据是 `TX_EMPTY` 置位且 `XMIT_BUSY` 清零** ——
只看 `TX_EMPTY` 会在最后一个字节还在移位时就返回，
关电源或切波特率前必须用组合判据。

---

## 5. MISC（`0x10`）位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 7:0 | RECV_IRQ | **接收**中断的 FIFO 阈值（字节数） |
| 15:8 | XMIT_IRQ | **发送**中断的 FIFO 阈值（字节数） |

---

## 6. REG5（`0x14`）波特率

| 位 | 名称 | 含义 |
|---|---|---|
| 22:0 | 分频值 | 掩码 `0x7fffff` |
| 23 | **BAUD_USE** | 使用本寄存器的设置（**必须置 1**，否则设置不生效） |
| 24 | **BAUD_XTAL** | 以 xtal 为时钟源 |
| 27 | **BAUD_XTAL_DIV2** | xtal 二分频（★ 见下） |

### 分频算法

时钟为 24 MHz 时：

```
xtal_div = 2          ← 本平台取 2，并且必须置 BAUD_XTAL_DIV2(bit 27)
divisor  = round(24000000 / xtal_div / baud) - 1
REG5     = divisor | BAUD_USE(23) | BAUD_XTAL(24) | BAUD_XTAL_DIV2(27)
```

时钟非 24 MHz 时：`divisor = round(uartclk / 4 / baud) - 1`，且不置 24/27 位。

> ★ **`xtal_div` 取 2 还是 3 是分 SoC 的，取错波特率会差 50%（输出全是乱码）。**
> 早期 Meson（如 g12a）用 **3**；`meson-s4-uart` 一系用 **2** 并置 bit 27。
> 本平台 `amlogic-s6.dtsi` 的 `compatible` 为
> `"amlogic,s6-uart", "amlogic,meson-s4-uart"` —— **回落到 s4 条目，故取 2。**
> 这是本驱动最容易出错的一处。

### 常用波特率分频表（xtal 24 MHz，xtal_div = 2，基准 12 MHz）

| 波特率 | divisor | REG5 值 | 实际波特率 | 误差 |
|---|---|---|---|---|
| 9600 | 1249 | `0x098004e1` | 9600.0 | 0.000% |
| 57600 | 207 | `0x098000cf` | 57692.3 | +0.160% |
| **115200** | **103** | **`0x09800067`** | 115384.6 | +0.160% |
| 230400 | 51 | `0x09800033` | 230769.2 | +0.160% |
| 460800 | 25 | `0x09800019` | 461538.5 | +0.160% |
| 921600 | 12 | `0x0980000c` | 923076.9 | +0.160% |
| **1500000** | **7** | **`0x09800007`** | 1500000.0 | 0.000% |

+0.16% 的误差远小于 UART 通常可容忍的 ±2%，可正常通信。

> ⚠️ **LogicPi A1 的控制台波特率尚未确认。** Amlogic 平台惯例是 **115200**，
> 与 Rockchip 的 1500000 不同 —— 前身项目的 1500000 是 Rockchip 特有，**不要沿用**。
> 上板后从 `/proc/cmdline` 的 `console=` 参数读取确认。

---

## 7. 建议的初始化序列

```
1. 关中断：CONTROL 清 RX_INT_EN(27) / TX_INT_EN(28)
2. 复位收发：CONTROL 置 TX_RST(22) | RX_RST(23) | CLEAR_ERR(24)，随后清 0
3. 设帧格式：CONTROL 写 DATA_LEN=0(8位) | STOP_BIT_LEN=0(1位) |
             TWO_WIRE_EN(15)，PARITY_EN(19) 清 0
4. 设波特率：REG5 按第 6 节算法写入
5. 设阈值：  MISC 写 RECV_IRQ / XMIT_IRQ（早期可先用轮询，阈值随意）
6. 使能收发：CONTROL 置 TX_EN(12) | RX_EN(13)
7. （用到中断时）挂 IRQ 201，再置 RX_INT_EN
```

**early print（`arm64_lowputc`）只需要第 1 步之外的最小子集**：
实际上 U-Boot 已经把串口配好了，early print 阶段
**只要轮询 `TX_FULL` 再写 `WFIFO` 即可，不必重新初始化**。
这与前身项目 RK3568 上的做法一致，也是 M1 最先该跑通的一段。

## 8. 收发要点

**发送一字节**：轮询 STATUS 直到 `TX_FULL(21)` 为 0，然后写 WFIFO。

**接收一字节**：轮询 STATUS 直到 `RX_EMPTY(20)` 为 0，然后读 RFIFO。

**中断处理（边沿触发，务必循环）**：

```
do {
    while (STATUS 的 RX_EMPTY 为 0)  读 RFIFO 交给上层
    检查错误位，必要时 CLEAR_ERR
} while (STATUS 的 RX_EMPTY 仍为 0);   ← 处理期间可能又来了数据
```

---

## 9. 与 NuttX 驱动框架的对接

结构范式参照 `drivers/serial/uart_pl011.c`（同为简单 MMIO 串口）。
需要实现的 `uart_ops_s` 成员与本规格的对应关系：

| `uart_ops_s` 成员 | 本控制器上要做的事 |
|---|---|
| `setup` | 第 7 节初始化序列 |
| `shutdown` | 清 TX_EN / RX_EN 与两个中断使能位 |
| `attach` / `detach` | 挂接 / 摘除 IRQ 201 |
| `ioctl` | termios：改帧格式（CONTROL）与波特率（REG5） |
| `receive` | 读 RFIFO；状态由 STATUS 错误位换算 |
| `rxint` | 置 / 清 CONTROL 的 RX_INT_EN(27) |
| `rxavailable` | STATUS 的 `RX_EMPTY(20)` 为 0 |
| `send` | 写 WFIFO |
| `txint` | 置 / 清 CONTROL 的 TX_INT_EN(28) |
| `txready` | STATUS 的 `TX_FULL(21)` 为 0 |
| `txempty` | `TX_EMPTY(22)` 置位 **且** `XMIT_BUSY(25)` 清零 |

---

## 10. 上板必须复核的项

- [ ] 控制器基址是否为 `0xfe07a000`（真实设备树为准）
- [ ] 中断号是否为 201，触发方式是否仍为边沿
- [ ] **控制台波特率**（`/proc/cmdline` 的 `console=`）—— 115200 还是别的
- [ ] `xtal_div` 是否确为 2（波特率对不上时优先怀疑此处）
- [ ] LogicPi A1 引出的调试串口是否就是 `uart_b`
