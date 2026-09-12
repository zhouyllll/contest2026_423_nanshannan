/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c
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
#include <syslog.h>
#include <sys/boardctl.h>
#include <stdint.h>
#include <nuttx/board.h>
#include <errno.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_reboot.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_memory_initialize
 *
 * Description:
 *   All RK3576 boards must provide the following entry point.  It is called
 *   early in the initialization before memory has been configured.
 *
 *   Logic here must be careful to avoid using any global variables because
 *   those will be uninitialized at the time this function is called.
 *
 ****************************************************************************/

void rk3576_memory_initialize(void)
{
  /* DDR 已由启动链路上游完成初始化：
   *   BootROM -> TPL(ddr.bin, Rockchip 闭源) -> SPL -> U-Boot -> 本镜像
   *
   * OS 侧不做 DDR 初始化。RK3576 的 DRAM 物理基址为 0x40000000
   * (U-Boot CFG_SYS_SDRAM_BASE)。
   */
}

/****************************************************************************
 * Name: rk3576_board_initialize
 *
 * Description:
 *   All RK3576 boards must provide the following entry point.  It is called
 *   after rk3576_memory_initialize and after all memory has been configured
 *   and mapped, but before any devices have been initialized.
 *
 ****************************************************************************/

void rk3576_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* TODO(M1+)：板载 LED 初始化，需原理图确认 GPIO 编号。 */
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 ****************************************************************************/

#ifdef CONFIG_HAVE_CXXINITIALIZE
/****************************************************************************
 * Name: kickpi_k7_register_eh_frame
 *
 * Description:
 *   把 C++ 异常的栈展开表注册给 libgcc 的展开器。
 *
 *   ★ 为什么裸机上非做不可。
 *
 *     `throw` 时 _Unwind_RaiseException 要靠 _Unwind_Find_FDE 找到描述
 *     每个栈帧如何回退的 CFI 表（.eh_frame）。libgcc 找它有两条路：
 *
 *       a) 走 dl_iterate_phdr 查 PT_GNU_EH_FRAME 程序头 —— 需要动态加载器
 *       b) 查 __register_frame_info() 注册过的对象链表
 *
 *     裸机 NuttX 没有 (a)，而 (b) 平时由 crtbegin.o 的构造函数完成 ——
 *     可我们不链接 crtbegin/crtend。于是表在镜像里、展开器却找不到，
 *     现象是 vector/map/RTTI 全过、一 throw 就崩在展开器内部。
 *
 *     上游 NuttX 的每一块 arm64 板子都把 .eh_frame 丢进 /DISCARD/，
 *     所以这不是本移植特有的问题 —— 只是纯 C 的板子碰不到。
 *
 *     必须在任何 C++ 异常之前注册，因此放在 board_late_initialize()。
 *
 ****************************************************************************/

extern void __register_frame_info(FAR const void *begin, FAR void *ob);
extern uint8_t __EH_FRAME_BEGIN__[];

/* libgcc 的 struct object 是不透明的，这里给足空间让它存放注册信息。
 * 它必须在整个运行期存活，所以是 static。
 */

static uintptr_t g_eh_object[8];

static void kickpi_k7_register_eh_frame(void)
{
  __register_frame_info(__EH_FRAME_BEGIN__, g_eh_object);
  syslog(LOG_INFO, "C++: 栈展开表已注册 @%p\n", __EH_FRAME_BEGIN__);
}
#endif

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
#ifdef CONFIG_HAVE_CXXINITIALIZE
  kickpi_k7_register_eh_frame();
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */

#ifdef CONFIG_BOARDCTL_RESET_CAUSE
/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   nsh 的 `reboot` 命令走这里。
 *
 *   ★ 为什么要有它：在此之前板子上没有任何"重启"手段，每次都得走
 *     `loader` -> USB 枚举 -> rkdeveloptool rd 这条链。而那条链的 USB
 *     环节很不稳 —— 一场调试里已经三次卡在"板子进了 loader 但主机
 *     看不见它"，只能人工断电。加上这个命令就绕开了整条 USB 链路。
 *
 *   status 非 0 时进下载模式，便于烧写；0 是普通重启。
 *
 ****************************************************************************/

int board_reset(int status)
{
  if (status != 0)
    {
      rk3576_reboot_loader();
    }
  else
    {
      rk3576_reboot_normal();
    }

  return 0;
}

/****************************************************************************
 * Name: board_reset_cause
 *
 * Description:
 *   报告本次启动的复位原因。
 *
 *   依据 TRM Part1 的 CRU_GLBRST_ST（0x27200000 + 0x0C04）：
 *     bit15 PMU_WDT   bit14 NPU_WDT  bit13 WDT_S
 *     bit12 WDT_NS    bit11 BUS_WDT  bit10 DDR_WDT
 *     bit6  由看门狗复位（细分看 [15:11]）
 *     bit5  看门狗的第二级复位
 *   全 0 表示是上电复位（POR）。
 *
 *   ★ 读完要清，否则下一次启动仍会看到上一次的原因 —— xTS 的看门狗
 *     用例正是靠"上电时是 POR、看门狗触发后是 WDT"这个变化来判定的，
 *     不清的话第二次判断必然错。清除用 GLBRST_ST 写 1 清。
 *
 ****************************************************************************/

int board_reset_cause(FAR struct boardioc_reset_cause_s *cause)
{
  uint32_t st;

  if (cause == NULL)
    {
      return -EINVAL;
    }

  st = getreg32(RK3576_CRU_ADDR + 0x0c04);

  if (st & ((1u << 6) | (1u << 5) | (0x1fu << 11)))
    {
      cause->cause = BOARDIOC_RESETCAUSE_SYS_RWDT;
    }
  else if (st == 0)
    {
      cause->cause = BOARDIOC_RESETCAUSE_SYS_CHIPPOR;
    }
  else
    {
      cause->cause = BOARDIOC_RESETCAUSE_UNKOWN;
    }

  cause->flag = st;

  /* 写 1 清，供下次启动区分 */

  putreg32(st, RK3576_CRU_ADDR + 0x0c04);
  return OK;
}
#endif
