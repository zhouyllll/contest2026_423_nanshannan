/****************************************************************************
 * chip/rk3576/hardware/rk3576_mailbox.h
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

#ifndef __CHIP_RK3576_HARDWARE_RK3576_MAILBOX_H
#define __CHIP_RK3576_HARDWARE_RK3576_MAILBOX_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 Mailbox。出处：TRM Part1 V1.2 第 17 章（p.851~）。
 *
 * 14 个独立的 group，每个占 4KB，寄存器只有 8 个：
 *
 *   MAILBOX_CH0..CH13 = 0x2AE50000 + n * 0x1000
 *
 * 每个 group 是一对单向门铃：A2B（AP 写、BB 收）和 B2A（BB 写、AP 收）。
 * "AP" / "BB" 只是 TRM 对两端的叫法，哪一端是谁由软件约定 —— 见 17.3
 * 「You can also regard CPU as the "BB" side」。
 *
 * 中断号（TRM Part1 中断表，列出的已经是 GIC INTID，即 NuttX 的 irq 号）：
 *
 *   irq_mailbox_ap0..ap13 = 157..170   ← B2A 方向，收 "BB" 端写的消息
 *   irq_mailbox_bb0..bb13 = 171..184   ← A2B 方向，收 "AP" 端写的消息
 *
 * 内核 dtsi 里 mailbox0..13 的 interrupts = GIC_SPI 125..138，+32 正好
 * 是 157..170，两处对得上，说明 Linux 侧固定站 "AP"。所以 openvela 站
 * "BB"：发消息写 B2A_CMD/DATA，收消息用 A2B_* 和 irq_mailbox_bbN。
 */

#define RK3576_MAILBOX_BASE    0x2ae50000
#define RK3576_MAILBOX_STRIDE  0x00001000
#define RK3576_MAILBOX_COUNT   14

#define RK3576_MBOX_A2B_INTEN  0x0000
#define RK3576_MBOX_A2B_STATUS 0x0004
#define RK3576_MBOX_A2B_CMD    0x0008
#define RK3576_MBOX_A2B_DATA   0x000c
#define RK3576_MBOX_B2A_INTEN  0x0010
#define RK3576_MBOX_B2A_STATUS 0x0014
#define RK3576_MBOX_B2A_CMD    0x0018
#define RK3576_MBOX_B2A_DATA   0x001c

/* INTEN 是 hiword-update 寄存器：写低 16 位要同时把 bit(16+n) 置 1。
 *
 *   bit 0 = inten     中断使能
 *   bit 8 = trig_mode 1 = 先写 CMD 再写 DATA 才触发（复位值就是 1）
 *
 * STATUS 的 bit 0 是 W1C。
 */

#define RK3576_MBOX_INT_MASK       (1u << 0)
#define RK3576_MBOX_TRIGGER_MASK   (1u << 8)
#define RK3576_MBOX_HIWORD(bit)    (1u << ((bit) + 16))
#define RK3576_MBOX_INT_UPDATE     RK3576_MBOX_HIWORD(0)
#define RK3576_MBOX_TRIGGER_UPDATE RK3576_MBOX_HIWORD(8)

/* pclk_mailbox0 门控：CRU_GATE_CON17 bit13（TRM p.123，1 = 关）。
 * 一个门控管全部 14 个 group。
 */

#define RK3576_CRU_MAILBOX_GATE_CON 17
#define RK3576_CRU_MAILBOX_GATE_BIT 13

#endif /* __CHIP_RK3576_HARDWARE_RK3576_MAILBOX_H */
