/****************************************************************************
 * chip/rk3576/rk3576_pl330.h
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

#ifndef __CHIP_RK3576_RK3576_PL330_H
#define __CHIP_RK3576_RK3576_PL330_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/dma/dma.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ★ 突发长度走 dma_config_s 的 option 字段。
 *
 *   通用的 struct dma_config_s 有位宽（*_width）和请求号（*_drq），却没有
 *   "一个突发里做几次传输"。这个值对 PL330 是必需的：DMAWFP 等到外设的
 *   突发请求后，DMALDP 会一口气搬 len x size 个字节 —— 如果 len 超过外设
 *   FIFO 的水位阈值，就会读到 FIFO 里没有的东西。
 *
 *   通用结构里 option 的定义就是"本控制器可选的配置"，所以放在这里，并在
 *   此处写明约定，而不是自己另开一个私有的配置结构。
 *
 *   不填（0）时按 1 处理，即每个请求搬一次。
 */

#define RK3576_DMA_OPT_BURST(n)     ((n) & 0x1f)
#define RK3576_DMA_OPT_GET_BURST(o) (((o) & 0x1f) ? ((o) & 0x1f) : 1)

/* SAI1 的外设请求号（rk3576.dtsi: dmas = <&dmac0 2>, <&dmac0 3>） */

#define RK3576_DMA_REQ_SAI1_TX      2
#define RK3576_DMA_REQ_SAI1_RX      3

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pl330_initialize
 *
 * Description:
 *   初始化一个 PL330 控制器并返回它的 DMA 设备对象。可重复调用，同一个
 *   控制器只会真正初始化一次。
 *
 * Input Parameters:
 *   ctrl - 控制器序号，0..2（对应 dtsi 的 dmac0..dmac2）。目前只实现 0。
 *
 * Returned Value:
 *   成功返回设备对象，失败返回 NULL。
 *
 ****************************************************************************/

struct dma_dev_s *rk3576_pl330_initialize(int ctrl);

#endif /* __CHIP_RK3576_RK3576_PL330_H */
