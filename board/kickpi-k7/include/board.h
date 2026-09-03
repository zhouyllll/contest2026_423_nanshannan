/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* KICKPI-K7 (Rockchip RK3576)
 *
 * SoC     : RK3576, quad Cortex-A72 @2.2GHz + quad Cortex-A53 @2.0GHz
 * DRAM    : 4/8/16 GB, 物理基址 0x40000000
 * eMMC    : 16/32/64 GB
 * 网络    : 双千兆以太网
 * 按键    : RESET / POWER / RECOVERY / MASKROM
 * 扩展    : GPIO x22, UART x5, PWM x8, ADC x3, I2C x1, PDM x1, CAN x1, I3C x1
 *
 * 调试串口：★ 已确认为 UART0 @ 0x2ad40000，波特率 1500000，时钟 24 MHz
 * （依据 KICKPI 官方 Armbian 源码的 U-Boot defconfig
 *  kickpi-k7-rk3576_defconfig 中的 CONFIG_DEBUG_UART_* 与 CONFIG_BAUDRATE）。
 */

/* 板级 GPIO 分配
 *
 * 来源：KICKPI 官方 Armbian 源码的厂商内核设备树
 * patch/kernel/rk35xx-vendor-6.1/dt/rk3576-kickpi-k7.dts
 *
 * Rockchip 引脚编码：bank 内按组 A/B/C/D 各 8 根，组内 0-7。
 * 线号 = 组序号 * 8 + 组内序号，例如 RK_PB4 = 1 * 8 + 4 = 12。
 *
 * RK3576 的 GPIO 控制器基址见 hardware/rk3576_memorymap.h：
 *   GPIO0 0x27320000（与 GPIO1-4 不在同一地址段）
 *   GPIO1 0x2ae10000  GPIO2 0x2ae20000  GPIO3 0x2ae30000  GPIO4 0x2ae40000
 */

/* LED（gpio-leds） */

#define BOARD_LED_WORK_BANK      0            /* work，dts 默认 heartbeat 触发 */
#define BOARD_LED_WORK_PIN       12           /* RK_PB4 = 1*8+4 */

#define BOARD_FAN_PWR_BANK       2            /* 风扇电源，默认 on */
#define BOARD_FAN_PWR_PIN        11           /* RK_PB3 = 1*8+3 */

#define BOARD_4G_PWR_BANK        0            /* 4G 模组电源，默认 on */
#define BOARD_4G_PWR_PIN         8            /* RK_PB0 = 1*8+0 */

#define BOARD_SD_PWR_BANK        0            /* SD 卡电源，默认 on */
#define BOARD_SD_PWR_PIN         14           /* RK_PB6 = 1*8+6 */

/* 以太网 PHY 复位（RTL8211F，rgmii-rxid） */

#define BOARD_GMAC0_RST_BANK     2
#define BOARD_GMAC0_RST_PIN      13           /* RK_PB5 = 1*8+5，低有效 */

#define BOARD_GMAC1_RST_BANK     3
#define BOARD_GMAC1_RST_PIN      3            /* RK_PA3 = 0*8+3，低有效 */

/* 实时时钟 HYM8563
 *
 * 出处：原厂 dtb 的 /i2c@2ac50000/hym8563@51（compatible "haoyu,hym8563"）。
 * 该芯片同时输出 32.768kHz 给 SDIO WiFi 模块用。
 */

#define BOARD_RTC_I2C_BUS        2
#define BOARD_RTC_I2C_ADDR       0x51

/* 5 寸 MIPI 屏 + 电容触摸（F050008M01，720x1280）
 *
 * ★ 出处：原理图 K7_V1.1_20241211_SCH.pdf 第 27 页 "Single-MIPI LCM"，
 *   配合同一份 PDF 里的芯片引脚复用表。这是此前一直缺失的那份信息 ——
 *   厂商的 LCD overlay dtsi 拿不到，但原理图给出了同样的答案。
 *
 * 30pin FPC (J5100) 关键脚：
 *   Pin17 LCD_PWM_BL   <- LCD_BL_PWM1_CH1_M0  GPIO0_B5 (PWM1_CH1_M0)
 *   Pin18 LCD_TE       ── 原理图上打叉，未连线（见下方说明）
 *   Pin19 VCC3V3_LCD   <- VCC3V3_LCD_S0（受 LCD_PWREN 控制的电源轨）
 *   Pin20 LCD_RST      <- LCD_RESET_L3 = LCD_RESET_L 经电平转换
 *   Pin21 LCD_ID       -> SARADC_IN7（电阻分压识别屏型号，可选）
 *   Pin22 LCD_PWREN    <- LCD_PWREN_H         GPIO0_C6
 *   Pin23 TP_I2C_SCL   <- I2C0_SCL_M1_TP      GPIO0_C1 (func 9)
 *   Pin24 TP_I2C_SDA   <- I2C0_SDA_M1_TP      GPIO0_C2 (func 9)
 *   Pin25 TP_INT       <- TP_INT_L            GPIO0_C5
 *   Pin26 TP_RST       <- TP_RST_L            GPIO0_D0
 *   Pin28-30 5V0       <- VCC5V0_DEVICE_S0（常供，不受软件控制）
 *
 * ★ 电源轨：屏和触摸共用 VCC3V3_LCD_S0，由 LCD_PWREN_H 经
 *   Q5002(S8050 NPN) -> Q5100(WPM2341 P-MOS) 开关。LCD_PWREN_H 拉高
 *   才有 3.3V。此前扫遍 I2C 找不到触摸，根因就是这一条没拉高 ——
 *   芯片没电，任何总线上都不会应答。
 *
 * ★ LCD_RESET_L 是 1.8V 域，经 Q5101 电平转换成 3.3V 的 LCD_RESET_L3
 *   再送到 FPC。转换是非反相的，软件按低有效复位即可。
 *
 * ★ LCD_TE 未连线（原理图 Pin18 打叉）。没有 TE 信号，DSI 命令模式无法
 *   与屏刷新同步，因此这块屏必须走**视频模式**。
 */

#define BOARD_LCD_PWREN_BANK     0            /* GPIO0_C6，高有效，屏 3V3 使能 */
#define BOARD_LCD_PWREN_PIN      22           /* RK_PC6 = 2*8+6 */

#define BOARD_LCD_RST_BANK       0            /* GPIO0_A2，低有效 */
#define BOARD_LCD_RST_PIN        2            /* RK_PA2 = 0*8+2 */

#define BOARD_LCD_BL_BANK        0            /* GPIO0_B5，PWM1_CH1_M0 */
#define BOARD_LCD_BL_PIN         13           /* RK_PB5 = 1*8+5 */

#define BOARD_TP_I2C_BUS         0            /* I2C0，M1 复用（时钟在 PMU 域） */

#define BOARD_TP_INT_BANK        0            /* GPIO0_C5 */
#define BOARD_TP_INT_PIN         21           /* RK_PC5 = 2*8+5 */

#define BOARD_TP_RST_BANK        0            /* GPIO0_D0，低有效 */
#define BOARD_TP_RST_PIN         24           /* RK_PD0 = 3*8+0 */

/* MIPI CSI 摄像头（Sony IMX415，cam0）
 *
 * 出处：厂商 Armbian 源码
 *   patch/kernel/rk35xx-vendor-6.1/dt/rk3576-kickpi-k7-cam0.dtsi
 *
 *   imx415_0@37   I2C4（i2c4m3_xfer = GPIO3_B0/GPIO3_A7 功能 11）
 *   xvclk         板上 37.125MHz 固定晶振，非 SoC 提供
 *   avdd          vcc_mipidcphy0，GPIO0_D2 高有效
 *   data-lanes    4
 *   通路          imx415 -> csi2_dcphy0 -> mipi0_csi2 -> rkcif -> rkisp
 */

#define BOARD_CAM_PWR_BANK       0            /* GPIO0_D2，高有效 */
#define BOARD_CAM_PWR_PIN        26           /* RK_PD2 = 3*8+2 */

/* 存储配置（供 M4 存储适配参考）
 *
 *   sdhci  : eMMC，8 位总线，HS400 1.8V + enhanced strobe，non-removable
 *   sdmmc  : SD 卡，4 位总线，pinctrl 用 sdmmc0_clk/cmd/det/bus4
 *
 * 上电顺序注意：SD 卡供电由 BOARD_SD_PWR 控制，需先拉高再枚举。
 */

/* 其它板级信息
 *
 *   PMIC     : RK806（厂商 dts 引入 rk3576-rk806.dtsi）
 *   RTC      : HYM8563（同时为 SDIO WiFi 提供 32.768kHz 外部时钟）
 *   DDR blob : rk3576_ddr_lp4_1560MHz_lp5_2736MHz_v1.08.bin
 *
 * DRAM：★ 已实测为 4 GB 版本。板上出厂 Android 的 /proc/meminfo 报
 *   MemTotal: 3989804 kB（≈3.8 GiB，差额是固件保留区与内核自身占用）。
 *   物理范围 0x40000000 .. 0x140000000。
 *
 *   端口当前取 CONFIG_RAMBANK1_ADDR=0x42000000 + 512MB，即映射
 *   0x42000000..0x62000000，稳落在物理范围内。openvela 侧任务极少，
 *   512MB 绰绰有余；保守取值同时也为将来 AMP 划分共享内存留出空间。
 */

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H */
