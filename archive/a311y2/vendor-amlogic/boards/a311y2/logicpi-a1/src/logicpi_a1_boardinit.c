/****************************************************************************
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

#include <nuttx/config.h>
#include <stdint.h>
#include <nuttx/board.h>
#include "logicpi_a1.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: a311y2_board_initialize
 *
 * Description:
 *   在内存映射建立之后、设备初始化之前调用。
 *
 ****************************************************************************/

void a311y2_board_initialize(void)
{
  /* DDR 已由启动链路上游完成初始化：
   *   BootROM -> BL2 -> ARM Trusted Firmware -> U-Boot -> 本镜像
   * 主线 dts 中为 ATF 预留了独立内存区（secmon_reserved），佐证该分工。
   *
   * TODO(M1+)：LED / 按键等板级资源初始化，需原理图确认 GPIO 编号。
   */
}

/****************************************************************************
 * Name: board_late_initialize
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
}
#endif
