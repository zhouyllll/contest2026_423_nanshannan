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
#include <sys/boardctl.h>
#include <stdint.h>
#include <nuttx/board.h>
#include <errno.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
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

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  /* Perform board initialization */
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */

#ifdef CONFIG_BOARDCTL_RESET_CAUSE
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
