/****************************************************************************
 * chip/rk3576/include/amp.h
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

/* AMP 链路的**对外**接口，装成 <arch/chip/amp.h>。
 *
 * 这里只放排查工具（ampctl）要用的东西：地址常量和只读状态。数据通路走
 * 标准的 rpmsg API，不从这里出去。驱动内部的接口在 chip/rk3576/
 * rk3576_rptun.h 和 rk3576_mailbox.h，应用不要引。
 */

#ifndef __ARCH_ARM64_INCLUDE_RK3576_AMP_H
#define __ARCH_ARM64_INCLUDE_RK3576_AMP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_RPTUN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 共享内存窗口。这几个数必须和 Linux 侧 DTS 里 rockchip-rpmsg 的
 * reg（vring 区）与 memory-region（rpmsg-dma 池）逐字对上。
 *
 *   vring0   = SHM_BASE + 0x00000   openvela → Linux
 *   vring1   = SHM_BASE + 0x08000   Linux → openvela
 *   buf pool = SHM_BASE + 0x200000  2MB，由 Linux 分配、两边共用
 */

#define RK3576_RPTUN_SHM_BASE    CONFIG_RK3576_RPTUN_SHM_BASE
#define RK3576_RPTUN_SHM_SIZE    0x00400000

#define RK3576_RPTUN_VRING_SIZE  0x8000   /* Linux RPMSG_VRING_SIZE  */
#define RK3576_RPTUN_VRING_NUM   64       /* Linux RPMSG_BUF_COUNT   */
#define RK3576_RPTUN_VRING_ALIGN 0x1000   /* Linux RPMSG_VRING_ALIGN */
#define RK3576_RPTUN_BUF_SIZE    512      /* 496 payload + 16 hdr    */

#define RK3576_RPTUN_VRING0_DA \
  (RK3576_RPTUN_SHM_BASE)
#define RK3576_RPTUN_VRING1_DA \
  (RK3576_RPTUN_SHM_BASE + RK3576_RPTUN_VRING_SIZE)
#define RK3576_RPTUN_POOL_DA \
  (RK3576_RPTUN_SHM_BASE + 0x200000)
#define RK3576_RPTUN_POOL_LEN    0x200000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 只读状态快照，给排查工具用，不参与数据通路。 */

struct rk3576_rptun_stat_s
{
  bool     registered;   /* rptun_initialize() 已成功              */
  bool     driver_ok;    /* 已收到对端第一次 kick，握手完成        */
  uint32_t kicks_rx;     /* 收到的门铃次数                         */
  uint32_t kicks_tx;     /* 发出的门铃次数                         */
  uint32_t tx_busy;      /* 发门铃时对端还没清上一条，被 -EBUSY 挡 */
  uint32_t last_cmd;     /* 最后一次收到的 cmd                     */
  uint32_t last_data;    /* 最后一次收到的 data                    */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

int rk3576_rptun_getstat(struct rk3576_rptun_stat_s *stat);

/* 读回某个 mailbox group 的 8 个寄存器（A2B_INTEN 起，按偏移顺序）。 */

int rk3576_mailbox_dump(unsigned int group, uint32_t regs[8]);

/* 门铃自检：在一个**没被 rptun 占用**的 group 上自己给自己按门铃，
 * 回答"时钟、中断号、使能位、W1C 清除、回调派发这一整条路通不通"。
 * 不碰 rptun 的状态机。
 */

int rk3576_mailbox_selftest(unsigned int group, uint32_t cmd, uint32_t data,
                            uint32_t *rx_cmd, uint32_t *rx_data);

/* 同一条路径，但故意不触发：必须超时。用来证明上面那个自检会失败。 */

int rk3576_mailbox_selftest_notrigger(unsigned int group,
                                      uint32_t *rx_cmd, uint32_t *rx_data);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_RPTUN */
#endif /* __ARCH_ARM64_INCLUDE_RK3576_AMP_H */
