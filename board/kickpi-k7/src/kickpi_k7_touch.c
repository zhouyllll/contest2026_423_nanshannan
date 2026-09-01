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
#include <stdint.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/gt9xx.h>

#include "arm64_internal.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_i2c.h"
#include <arch/board/board.h>
#include "kickpi_k7.h"

#ifdef CONFIG_INPUT_GT9XX

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 引脚与总线取自原理图 K7_V1.1 第 27 页 "Single-MIPI LCM"，
 * 定义在 board.h（含出处与电源轨说明）。
 *
 * 此前扫遍 I2C1~I2C9 找不到触摸，有两个叠加的原因，原理图把两个都
 * 解释清楚了：
 *   1) 触摸挂在 I2C0 上，而 I2C0 因时钟在 PMU 域被跳过了，从未扫过；
 *   2) 触摸由 VCC3V3_LCD_S0 供电，该轨受 LCD_PWREN_H (GPIO0_C6)
 *      控制，不拉高则芯片无电 —— 扫任何总线都不会有应答。
 */

#define TOUCH_I2C_BUS      BOARD_TP_I2C_BUS
#define TOUCH_I2C_ADDR     0x5d   /* GT9xx 默认；复位时 INT 为高则是 0x14 */

#define TOUCH_IRQ_BANK     BOARD_TP_INT_BANK
#define TOUCH_IRQ_PIN      BOARD_TP_INT_PIN
#define TOUCH_RST_BANK     BOARD_TP_RST_BANK
#define TOUCH_RST_PIN      BOARD_TP_RST_PIN

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_touch_irq_attach(const struct gt9xx_board_s *state,
                                   xcpt_t isr, FAR void *arg)
{
  UNUSED(state);

  if (TOUCH_IRQ_BANK < 0)
    {
      return OK;      /* 中断脚未知，上层退化为轮询 */
    }

  return rk3576_gpio_irq_attach(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, isr, arg);
}

static void kickpi_touch_irq_enable(const struct gt9xx_board_s *state,
                                    bool enable)
{
  UNUSED(state);

  if (TOUCH_IRQ_BANK >= 0)
    {
      rk3576_gpio_irq_enable(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, enable);
    }
}

static int kickpi_touch_set_power(const struct gt9xx_board_s *state, bool on)
{
  UNUSED(state);

  /* 触摸与屏共用 VCC3V3_LCD_S0，该轨由 kickpi_k7_lcd_power() 统一
   * 管理（屏和触摸不能各自开关同一条轨）。这里只做复位脚。
   *
   * ★ 复位时序决定 I2C 地址：GT9xx 在复位释放的瞬间采样 INT 脚 ——
   *   低电平选 0x5d，高电平选 0x14。原理图上 TP_INT_L 有 10K 上拉到
   *   VCC_3V3_S3 (R5106)，若复位期间不主动拉低，采到的就是高电平，
   *   地址会是 0x14。下面在释放复位前把 INT 驱动为低，锁定 0x5d。
   */

  if (!on)
    {
      rk3576_gpio_setdir(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
      rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, false);
      return OK;
    }

  /* 复位保持低，同时把 INT 驱动为低选地址 0x5d */

  rk3576_pinmux_set(TOUCH_RST_BANK, TOUCH_RST_PIN, RK3576_PINMUX_GPIO);
  rk3576_pinmux_set(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, RK3576_PINMUX_GPIO);

  rk3576_gpio_setdir(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, false);
  rk3576_gpio_setdir(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, true);
  rk3576_gpio_write(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, false);
  up_mdelay(10);

  /* 释放复位，INT 继续保持低 >50us 让芯片采样完成 */

  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  up_udelay(200);

  /* INT 交回输入，之后作为中断脚使用 */

  rk3576_gpio_setdir(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, false);
  up_mdelay(50);          /* GT9xx 上电到可通信约需 50ms */
  return OK;
}

/****************************************************************************
 * Name: kickpi_touch_probe
 *
 * Description:
 *   读 GT9xx 的产品 ID 寄存器确认芯片确实在总线上应答。
 *
 *   ★ gt9xx_register() 只做注册，不访问器件 —— 注册成功不等于芯片存在。
 *     这里补一次真实读取：0x8140 起 4 字节是 ASCII 产品号（如 "911"、
 *     "1158"），0x8144 起 2 字节是固件版本。读不到就说明供电、地址或
 *     总线仍有问题，此时注册出来的 /dev/input0 是个空壳。
 *
 *   GT9xx 用 16 位寄存器地址，高字节在前，因此要写 2 字节再重启读。
 *
 ****************************************************************************/

static int kickpi_touch_probe(struct i2c_master_s *i2c, uint8_t addr)
{
  struct i2c_msg_s msg[2];
  uint8_t regaddr[2];
  uint8_t buf[6];
  int ret;

  regaddr[0] = 0x81;          /* 0x8140 高字节 */
  regaddr[1] = 0x40;

  msg[0].frequency = 400000;
  msg[0].addr      = addr;
  msg[0].flags     = 0;
  msg[0].buffer    = regaddr;
  msg[0].length    = 2;

  msg[1].frequency = 400000;
  msg[1].addr      = addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = sizeof(buf);

  ret = I2C_TRANSFER(i2c, msg, 2);
  if (ret < 0)
    {
      return ret;
    }

  /* 产品号是不带结尾 0 的 ASCII，可能只有 3 位；末字节补 0 再打印。 */

  buf[4] = '\0';
  syslog(LOG_INFO,
         "触摸: GT9xx 应答 产品号=\"%s\" 版本=0x%02x%02x "
         "(原始 %02x %02x %02x %02x)\n",
         (char *)buf, buf[5], buf[4],
         buf[0], buf[1], buf[2], buf[3]);
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

  /* 先跑一遍复位时序把芯片带出复位（顺带锁定 0x5d 地址），再探测。
   * 电源轨已由 kickpi_k7_lcd_power() 在此之前打开。
   */

  kickpi_touch_set_power(&g_touch_board, true);

  ret = kickpi_touch_probe(i2c, TOUCH_I2C_ADDR);
  if (ret < 0)
    {
      /* 换另一个可能的地址再试一次 —— INT 采样时序若与预期不符，
       * 芯片会落在 0x14 上。
       */

      syslog(LOG_WARNING, "触摸: 0x%02x 无应答(%d)，改试 0x14\n",
             TOUCH_I2C_ADDR, ret);
      ret = kickpi_touch_probe(i2c, 0x14);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: 触摸 0x5d 与 0x14 均无应答，不注册 /dev/input0\n");
          return -ENODEV;
        }
    }

  if (TOUCH_IRQ_BANK >= 0)
    {
      ret = rk3576_gpio_irq_config(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN,
                                   false, false);   /* 下降沿触发 */
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: 触摸中断脚配置失败: %d\n", ret);
          return ret;
        }
    }

  ret = gt9xx_register("/dev/input0", i2c, TOUCH_I2C_ADDR, &g_touch_board);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/input0 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "触摸: /dev/input0 就绪（GT9xx @I2C%d:0x%02x, %s）\n",
         TOUCH_I2C_BUS, TOUCH_I2C_ADDR,
         TOUCH_IRQ_BANK < 0 ? "中断脚未知，轮询模式" : "中断模式");
  return OK;
}

#endif /* CONFIG_INPUT_GT9XX */
