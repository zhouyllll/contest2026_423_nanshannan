/****************************************************************************
 * chip/rk3576/rk3576_mailbox.h
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

#ifndef __CHIP_RK3576_RK3576_MAILBOX_H
#define __CHIP_RK3576_RK3576_MAILBOX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 在中断上下文里被调用；cmd/data 就是对端写进门铃的两个字，本层不解释。 */

typedef void (*rk3576_mailbox_callback_t)(void *arg, uint32_t cmd,
                                          uint32_t data);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 打开 pclk 门控、清掉残留状态、挂上 group 的 A2B 中断。
 * 只是 irq_attach，不 enable —— enable 留到注册回调时，避免回调还没装好
 * 就被对端的第一次 kick 打中。
 */

int rk3576_mailbox_initialize(unsigned int rx_group);

/* 向某个 group 的 B2A 门铃写一条 {cmd, data}。先写 CMD 后写 DATA 才触发。 */

int rk3576_mailbox_send(unsigned int group, uint32_t cmd, uint32_t data);

/* 注册（callback != NULL）或注销（NULL）接收回调，并相应开关中断。 */

void rk3576_mailbox_register_callback(rk3576_mailbox_callback_t callback,
                                      void *arg);

/* 调试用：读回 group 的 8 个寄存器，主要给 ampctl status 用。 */

int rk3576_mailbox_dump(unsigned int group, uint32_t regs[8]);

/* 自检：见 rk3576_mailbox.c 里同名函数的说明。 */

int rk3576_mailbox_selftest(unsigned int group, uint32_t cmd, uint32_t data,
                            uint32_t *rx_cmd, uint32_t *rx_data);

/* 同一条路径，但故意不触发：必须超时。用来证明上面那个自检会失败。 */

int rk3576_mailbox_selftest_notrigger(unsigned int group,
                                      uint32_t *rx_cmd, uint32_t *rx_data);

#endif /* __CHIP_RK3576_RK3576_MAILBOX_H */
