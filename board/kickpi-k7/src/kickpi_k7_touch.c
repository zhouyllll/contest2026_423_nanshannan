/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_touch.c
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

/* 电容触摸：汇顶 GT9xx，接在 MIPI 屏（F050008M01，5 寸 720x1280）的排线上。
 *
 * 驱动本体是上游现成的 drivers/input/gt9xx.c，本文件只提供三个板级回调
 * （中断挂接、中断开关、上电）和注册调用。
 *
 * ★ 参数来源与不确定性
 *
 *   芯片家族与 I2C 地址取自 KICKPI 官方文档的 GT9XX 设备树示例：
 *       compatible = "goodix,gt9xx";  reg = <0x5d>;
 *   但该示例对应的是 1024x600 的屏，**总线号与 GPIO 未必与本板一致**。
 *   因此下面几个宏标了"待实测"：上板后用 i2c dev 扫到地址、
 *   并确认中断脚确实有跳变，再把它们固定下来。
 *
 *   GT9xx 的地址只有两个可能值：0x5d（默认）或 0x14（复位时 INT 拉高
 *   则选此值）。扫描时两个都要看。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/gt9xx.h>

#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "kickpi_k7.h"

#ifdef CONFIG_INPUT_GT9XX

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ★ 待实测：以下三项需上板确认后固定 */

#define TOUCH_I2C_BUS      2      /* 待实测：屏排线的触摸 I2C 接在哪条  */
#define TOUCH_I2C_ADDR     0x5d   /* GT9xx 默认；另一可能值为 0x14      */
#define TOUCH_IRQ_BANK     3      /* 待实测：文档示例为 gpio3 RK_PA3    */
#define TOUCH_IRQ_PIN      3
#define TOUCH_RST_BANK     0      /* 待实测：文档示例为 gpio0 RK_PB6    */
#define TOUCH_RST_PIN      14

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_touch_irq_attach(const struct gt9xx_board_s *state,
                                   xcpt_t isr, FAR void *arg)
{
  UNUSED(state);
  return rk3576_gpio_irq_attach(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, isr, arg);
}

static void kickpi_touch_irq_enable(const struct gt9xx_board_s *state,
                                    bool enable)
{
  UNUSED(state);
  rk3576_gpio_irq_enable(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, enable);
}

static int kickpi_touch_set_power(const struct gt9xx_board_s *state, bool on)
{
  UNUSED(state);

  /* 触摸控制器与屏共用供电，这里只操作复位脚。
   *
   * ★ 复位时序决定 I2C 地址：GT9xx 在复位释放的瞬间采样 INT 脚 ——
   *   低电平选 0x5d，高电平选 0x14。本实现让 INT 保持输入（外部下拉），
   *   因此期望地址是 0x5d；若上板扫到的是 0x14，说明该脚被外部拉高，
   *   把 TOUCH_I2C_ADDR 改掉即可，不必改时序。
   */

  rk3576_gpio_setdir(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, on);
  return OK;
}

static const struct gt9xx_board_s g_touch_board =
{
  .irq_attach = kickpi_touch_irq_attach,
  .irq_enable = kickpi_touch_irq_enable,
  .set_power  = kickpi_touch_set_power,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   注册电容触摸为 /dev/input0。
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;

  i2c = rk3576_i2cbus_initialize(TOUCH_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: 触摸所在 I2C%d 初始化失败\n", TOUCH_I2C_BUS);
      return -ENODEV;
    }

  ret = rk3576_gpio_irq_config(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN,
                               false, false);   /* 下降沿触发 */
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸中断脚配置失败: %d\n", ret);
      return ret;
    }

  ret = gt9xx_register("/dev/input0", i2c, TOUCH_I2C_ADDR, &g_touch_board);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/input0 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO,
         "触摸: /dev/input0 就绪（GT9xx @I2C%d:0x%02x, IRQ gpio%d-%d）\n",
         TOUCH_I2C_BUS, TOUCH_I2C_ADDR, TOUCH_IRQ_BANK, TOUCH_IRQ_PIN);
  return OK;
}

#endif /* CONFIG_INPUT_GT9XX */
