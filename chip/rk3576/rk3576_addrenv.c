/****************************************************************************
 * chip/rk3576/rk3576_addrenv.c
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
 * 物理地址 ←→ 虚拟地址。
 *
 * ★ 为什么这两个函数非有不可
 *
 *   OpenAMP 和 rpmsg 的共享内存是**跨 OS**的：resource table 里记的
 *   vring / 缓冲池地址必须是物理地址，因为对端（Linux）只认物理地址；
 *   而本端访问它们要用虚拟地址。libmetal 的 io.c 和
 *   drivers/rpmsg/rpmsg_virtio.c 因此直接调用这两个 up_ 接口。
 *   不提供就是链接期 undefined reference —— 这也是接 AMP 时第一个
 *   会撞上的错误。
 *
 * ★ 本端口为什么可以写成恒等
 *
 *   arch/arm64 的 MMU 表在 rk3576_boot.c 里全部用 MMU_REGION_FLAT_ENTRY
 *   建，也就是**平坦恒等映射**：DRAM、外设、AMP 共享窗口，每一段的 VA
 *   都等于 PA。配置上也是 CONFIG_BUILD_FLAT，没开 CONFIG_ARCH_ADDRENV，
 *   没有第二套地址空间存在。所以恒等不是"偷懒的近似"，它就是事实。
 *
 *   一旦以后改成 CONFIG_BUILD_KERNEL（用户态有独立地址空间），这里就
 *   必须换成真的走页表。所以下面用 #error 把这个前提钉死 —— 与其将来
 *   让 DMA 拿着一个错误地址去搬数据（表现是随机内存被改写，极难定位），
 *   不如现在就编译失败。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/types.h>

#include <nuttx/arch.h>

#ifdef CONFIG_BUILD_KERNEL
#  error "rk3576_addrenv.c 的恒等实现只对 CONFIG_BUILD_FLAT 成立；" \
         "改成 BUILD_KERNEL 时必须改成走页表的实现"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_addrenv_pa_to_va
 ****************************************************************************/

FAR void *up_addrenv_pa_to_va(uintptr_t pa)
{
  return (FAR void *)pa;
}

/****************************************************************************
 * Name: up_addrenv_va_to_pa
 ****************************************************************************/

uintptr_t up_addrenv_va_to_pa(FAR void *va)
{
  return (uintptr_t)va;
}
