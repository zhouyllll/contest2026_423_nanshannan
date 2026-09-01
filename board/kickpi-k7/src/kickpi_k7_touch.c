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

/* ★★ 板上实测结论（屏已物理接上）：I2C1~I2C9 全部扫描，
 *    未在任何总线上发现 GT9xx（0x5d 或 0x14）。
 *
 *    已扫到的器件：I2C1 的 0x23(PMIC)、I2C2 的 0x4e(PD)+0x51(RTC)、
 *    I2C3 的 0x10(ES8388)；I2C4/5/7/8/9 为空；I2C6 起始条件即失败
 *    （fcnt=0，引脚复用存疑）。未扫 I2C0 —— 它在 PMU 域，时钟基址不同。
 *
 *    最可能的原因是**触摸未供电**：厂商面板节点有
 *    power-supply = <&vcc3v3_lcd_n>，而该 regulator 定义在 LCD overlay
 *    dtsi 里，基础 dtb 中并不存在（dsi 节点本身也是 disabled）。
 *    没供电时扫遍所有总线也找不到，这一点无法用软件区分。
 *
 *    四个缺失信息都在同一个文件里，需向 KICKPI 技术支持索取：
 *      rk3576-kickpi-k7-android-mipi-5-720-1280-F050008M01.dtsi
 *        触摸的 I2C 总线号、供电轨 GPIO、面板初始化序列、背光控制
 *
 *    下面的总线号保持 2 仅为占位，未经证实。
 */

#define TOUCH_I2C_BUS      2      /* 未证实：全总线扫描未发现触摸  */
#define TOUCH_I2C_ADDR     0x5d   /* GT9xx 默认；另一可能值为 0x14      */
/* ★ 中断脚原取自 KICKPI 文档中另一块屏（1024x600）的示例 gpio3-3，
 *   现已证明该值错误：board.h 记载 gpio3-3 是 GMAC1 的 PHY 复位脚
 *   （出处为本板 dts）。两者冲突，会互相干扰。
 *
 *   本板 F050008M01 的触摸中断/复位脚尚无可靠出处，暂填 -1 表示未知：
 *   不配置中断，改由上层轮询。这样触摸仍可用，且不会误动别的引脚。
 *
 *   落实办法：向 KICKPI 索取
 *   rk3576-kickpi-k7-android-mipi-5-720-1280-F050008M01.dtsi，
 *   其中的 goodix_irq_gpio / goodix_rst_gpio 即为确定值。
 */

#define TOUCH_IRQ_BANK     (-1)
#define TOUCH_IRQ_PIN      (-1)
#define TOUCH_RST_BANK     (-1)
#define TOUCH_RST_PIN      (-1)

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

  /* 触摸控制器与屏共用供电，这里只操作复位脚。
   *
   * ★ 复位时序决定 I2C 地址：GT9xx 在复位释放的瞬间采样 INT 脚 ——
   *   低电平选 0x5d，高电平选 0x14。本实现让 INT 保持输入（外部下拉），
   *   因此期望地址是 0x5d；若上板扫到的是 0x14，说明该脚被外部拉高，
   *   把 TOUCH_I2C_ADDR 改掉即可，不必改时序。
   */

  if (TOUCH_RST_BANK < 0)
    {
      return OK;      /* 复位脚未知，不去误动别的引脚 */
    }

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
