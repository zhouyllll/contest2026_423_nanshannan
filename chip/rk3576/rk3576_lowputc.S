/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_lowputc.S
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm64_macro.inc"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

    .file    "rk3576_lowputc.S"

/* RK3576 调试串口基址（来源：rk3576.dtsi serial@2ad40000，UART0）。
 *
 * 汇编阶段取不到 hardware/rk3576_memorymap.h 的 C 宏，只能写字面量。
 * ★ 必须和 defconfig 的 CONFIG_16550_UART0_BASE 保持一致。
 *
 * ★ 这是全项目第一个会向外吐字符的地方。M1 调试时，如果这里都出不了
 *   字符，说明问题在加载地址、异常向量或 MMU，而不在串口。
 *
 * ★ M1 阶段不需要配时钟、引脚复用和波特率 —— U-Boot 已经把这一路串口
 *   初始化好了（它自己就在往这个口打印启动日志），这里只是往已经能用的
 *   FIFO 里写字节。
 *
 * ★ 已由厂商资料确认（KICKPI 官方 Armbian 源码的 U-Boot defconfig
 *   patch/u-boot/legacy/u-boot-radxa-rk35xx/defconfig/kickpi-k7-rk3576_defconfig）：
 *     CONFIG_DEBUG_UART_BASE=0x2ad40000   -> UART0
 *     CONFIG_DEBUG_UART_CLOCK=24000000    -> 24 MHz
 *     CONFIG_DEBUG_UART_SHIFT=2           -> reg-shift=2, 即 REGINCR=4
 *     CONFIG_BAUDRATE=1500000
 */

#define UART_DBG_BASE_ADDRESS 0x2ad40000    /* RK3576 UART0 */

/****************************************************************************
 * Assembly Macros
 ****************************************************************************/

/* Wait for the DW 8250 UART to be ready to transmit.
 * 寄存器按 4 字节步进：LSR 是第 5 个寄存器，偏移 5 * 4 = 0x14。
 */

.macro early_uart_ready xb, wt
1:
    ldrh  \wt, [\xb, #0x14]      /* UART_LSR (Line Status Register) */
    tst   \wt, #0x20             /* Check THRE (TX Holding Register Empty) */
    b.eq  1b                     /* Wait for the UART to be ready (THRE=1) */
.endm

/* Transmit a character. THR 是第 0 个寄存器，偏移 0。 */

.macro early_uart_transmit xb, wt
    strb  \wt, [\xb]             /* UART_THR (Transmit Holding Register) */
.endm

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Print a character on the UART - this function is called by C */

GTEXT(arm64_lowputc)
SECTION_FUNC(text, arm64_lowputc)
    ldr   x15, =UART_DBG_BASE_ADDRESS
    early_uart_ready x15, w2
    early_uart_transmit x15, w0
    ret
