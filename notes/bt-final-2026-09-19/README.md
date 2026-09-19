# 蓝牙（SKW6621S / SDIO）整体排查结论 —— 截止前停止

日期：2026-09-19。结论：**软件侧可核对的项目全部与原厂一致，枚举仍失败；
剩余可能都在硬件层面，需要仪器测量。按用户决定，截止前不再继续。**

## 现象（未变）

`bt probe`：空闲时 CMD 高 64/64、DAT0 高 0/64；CMD52 读 / 复位、CMD5
均为 RINTSTS=0x2（RE，响应格式错）、RESP0=0；CMD0 正常完成。

RE 而不是 RTO：控制器在响应窗口看到了起始位，但后续位不对。说明 CMD 线
上有东西在动，但无法据此断定模组在正常应答。

## 本次核对（在 09-18 六轮单变量实验之后）

| 项目 | 依据 | 结论 |
|---|---|---|
| 引脚与复用 | SDK rk3576-pinctrl.dtsi sdmmc1m0：GPIO1_B4..B7 数据、C0 CMD、C1 CLK，功能 2，上拉 | 一致 |
| 卡时钟 | SDK clk-rk3576.c：CCLK_SRC_SDIO = CLKSEL_CON(104) mux@6(2) div@0(6)，{gpll,cpll,xin24m}；驱动选 xin24m、分频 60 → 400kHz，控制器内部 /2 | 一致 |
| 电源域 / 复位 | PD_SDGMAC 上电；WIFI_REG_ON GPIO1_C6、BT_REG_ON GPIO1_C7 先低后高，200ms | 与 mmc-pwrseq-simple 一致 |
| Linux 端干扰 | AMP 内核 CONFIG_RFKILL、CONFIG_WIRELESS 未开，无 MMC 驱动 | 不会碰模组的脚 |
| GMAC1 抢引脚 | GMAC1 数据脚在 GPIO2（eth1m0），与 GPIO1 的 SDIO 不重叠 | 无冲突 |
| GMAC1 25M 输出 | rk3576_gmac.c 同时从 GPIO2_D6 与 GPIO1_D5 输出 25MHz；GPIO1_D5 是模组 WIFI_HOST_WAKE（模组输出），原厂 K7 gmac1 pinctrl 并无 25M 输出 | 与原厂不一致。实验：probe 前把 GPIO1_D5 改回 GPIO 输入 → **结果不变**，排除 |
| IO 电压域 | SDK io-domain.c 无 RK3576 条目；TRM/数据手册未见电压选择寄存器 | 视为硬件自适应，软件无可修 |

## 下一步（需要仪器）

1. 量模组供电（VBAT、VDDIO）与 WIFI_REG_ON / BT_REG_ON 实际电平；
2. 量 DAT0 在模组复位前后的电平与被谁拉低；
3. 示波器看 CMD5 期间 CLK/CMD 波形与电平；
4. 以上都正常再对照原厂 Android 运行时的 SDIO 控制器寄存器。

枚举通过之后仍需：SKW SDIO 功能驱动、固件下载、HCI 通路、NuttX 蓝牙栈对接。

## 附带发现（未改）

rk3576_gmac.c 的 GMAC1 25M "两条都打开"会驱动 GPIO1_D5（模组 HOST_WAKE）。
与蓝牙失败无关（实验已排除），但与原厂不一致，后续整理网口时应去掉该输出。
