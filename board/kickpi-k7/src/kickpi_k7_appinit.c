/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_appinit.c
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
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <stdint.h>
#include <nuttx/board.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/drivers/drivers.h>

#include "rk3576_power.h"
#include "rk3576_sdhci.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  This function is never
 *   called directly from application code, but only indirectly via the
 *   (non-standard) boardctl() interface using the command BOARDIOC_INIT.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#if defined(CONFIG_FS_PROCFS) || defined(CONFIG_DEV_GPIO) || \
    defined(CONFIG_RK3576_I2C)
  int ret;
#endif

#ifdef CONFIG_FS_PROCFS
  /* 挂载 procfs。
   *
   * CONFIG_NSH_ARCHINIT=y 时 NSH 不会自己挂载 /proc，改由本函数负责
   * （NSH 通过 boardctl(BOARDIOC_INIT) 调到这里）。没有它，free、ps、
   * uptime 等命令都会报 "Could not open /proc/... (is procfs mounted?)"。
   *
   * 挂载失败不作为致命错误：procfs 只是观察窗口，缺了它系统其余部分
   * 照常工作，因此仅记录日志、继续初始化后面的外设。
   */

  ret = mount(NULL, CONFIG_NSH_PROC_MOUNTPOINT, "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 procfs 到 %s 失败: %d\n",
             CONFIG_NSH_PROC_MOUNTPOINT, ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  /* 注册板上 GPIO 为 /dev/gpoutN。同样不作为致命错误 —— GPIO 不可用时
   * 串口控制台仍应能进入，便于继续排查。
   */

  ret = kickpi_k7_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GPIO 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_I2C
  ret = kickpi_k7_i2c_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: I2C 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_SDHCI
  /* eMMC：拿到 sdio_dev_s 句柄，交给 mmcsd 上层注册 /dev/mmcsd0。
   * 失败不阻断其余初始化 —— 没有存储时控制台仍应可用，便于继续排查。
   */

    {
      struct sdio_dev_s *sdio = rk3576_sdhci_initialize(0);

      if (sdio == NULL)
        {
          syslog(LOG_ERR, "ERROR: eMMC 控制器初始化失败\n");
        }
      else
        {
          ret = mmcsd_slotinitialize(0, sdio);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize 失败: %d\n", ret);
            }
          else
            {
              /* ★ 不能只看返回值。mmcsd_slotinitialize() 在卡未识别时
               * 也返回 OK —— 它只把 -ENODEV 之外的错误当作失败，
               * 而块设备注册发生在更里层的 mmcsd_probe()，条件是
               * 分区块数非零。所以这里实际 stat 一下设备节点，
               * 存在才算就绪。
               */

              struct stat st;

              if (stat("/dev/mmcsd0", &st) == 0)
                {
                  syslog(LOG_INFO, "eMMC: /dev/mmcsd0 就绪\n");

#ifdef CONFIG_BCH
                  /* NuttX 的块设备不能直接被 open() 当文件读 ——
                   * dd / hexdump 这类工具走的是字符设备接口。
                   * BCH 把块设备包成字符设备，便于裸读验证与取分区表。
                   * 只读注册，避免误写坏 eMMC 上原有的分区。
                   */

                  ret = bchdev_register("/dev/mmcsd0", "/dev/mmcsd0c", true);
                  if (ret < 0)
                    {
                      syslog(LOG_ERR, "ERROR: 注册 /dev/mmcsd0c 失败: %d\n",
                             ret);
                    }
                  else
                    {
                      syslog(LOG_INFO,
                             "eMMC: /dev/mmcsd0c 就绪（只读字符视图）\n");
                    }
#endif
                }
              else
                {
                  syslog(LOG_ERR,
                         "ERROR: mmcsd_slotinitialize 返回 OK 但 "
                         "/dev/mmcsd0 不存在 —— 卡未被识别\n");
                }
            }
        }
    }
#endif

  /* ★ 显示链路第一步：打开 VOP 与 DSI 所在的电源域。
   *
   * 与 eMMC 不同，这两个域 U-Boot 不会留给我们 —— 原厂 dtb 里
   * dsi@27d80000 是 disabled，引导阶段根本没用过 MIPI。
   *
   * 验证方式：上电后读 VOP 的寄存器。读到 0 或 0xffffffff 说明域没开
   * 或基址不对；读到像样的值说明域确实活了。这是先前 GPIO(VER_ID)、
   * eMMC(CAP0) 用过的同一套自检思路 —— 找一个能一次性证伪多个前提的读数。
   */

    {
      int pd;

      pd = rk3576_power_on(RK3576_PD_VOP);
      if (pd == OK)
        {
          pd = rk3576_power_on(RK3576_PD_VO0);
        }

      if (pd < 0)
        {
          syslog(LOG_ERR, "ERROR: 显示电源域上电失败: %d\n", pd);
        }
      else
        {
          volatile uint32_t *vop = (volatile uint32_t *)0x27d00000ul;

          syslog(LOG_INFO,
                 "VOP 探测: [0x00]=0x%08" PRIx32 " [0x04]=0x%08" PRIx32
                 " [0x08]=0x%08" PRIx32 " [0x0c]=0x%08" PRIx32 "%s\n",
                 vop[0], vop[1], vop[2], vop[3],
                 (vop[0] == 0 || vop[0] == 0xffffffff)
                   ? "  ← 全 0/全 F，域未开或基址不对" : "  ← 域已就绪");
        }
    }

#ifdef CONFIG_INPUT_GT9XX
  ret = kickpi_k7_touch_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸初始化失败: %d\n", ret);
    }
#endif

  /* TODO(M3+)：网络等外设的注册点。 */

  return OK;
}
