/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_gpio.c
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

/* 把板上引出的 GPIO 注册成 /dev/gpioN，可用 nsh 的 gpio 命令读写。
 *
 * 只注册 U-Boot 已经复用成 GPIO 的引脚 —— SoC 层的 rk3576_gpio.c 不配
 * pinmux，原因见该文件顶部说明。板子 dts 里声明为 gpio-leds / gpio-fan
 * 的引脚满足这个前提。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include <nuttx/ioexpander/gpio.h>
#include <arch/board/board.h>

#include "rk3576_gpio.h"
#include "hardware/rk3576_gpio.h"
#include "kickpi_k7.h"

#ifdef CONFIG_DEV_GPIO

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kickpi_gpout_s
{
  struct gpio_dev_s   gpio;      /* 必须是第一个成员 */
  uint8_t             bank;
  uint8_t             pin;
  const char         *name;      /* 仅用于日志 */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int kickpi_gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value);
static int kickpi_gpout_write(FAR struct gpio_dev_s *dev, bool value);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gpio_operations_s g_gpout_ops =
{
  .go_read       = kickpi_gpout_read,
  .go_write      = kickpi_gpout_write,
  .go_attach     = NULL,        /* 中断暂不支持，见文件末尾 TODO */
  .go_enable     = NULL,
  .go_setpintype = NULL,
  .go_setdebounce = NULL,
  .go_setmask    = NULL,
};

/* 板上可用的输出引脚。引脚号来自 include/board.h，出处是厂商 dts。 */

static struct kickpi_gpout_s g_gpouts[] =
{
  {
    .bank = BOARD_LED_WORK_BANK,      /* GPIO0_B4，dts 默认 heartbeat */
    .pin  = BOARD_LED_WORK_PIN,
    .name = "work-led",
  },
  {
    .bank = BOARD_FAN_PWR_BANK,       /* GPIO2_B3，风扇电源，默认 on */
    .pin  = BOARD_FAN_PWR_PIN,
    .name = "fan-pwr",
  },
};

#define KICKPI_NGPOUT (sizeof(g_gpouts) / sizeof(g_gpouts[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct kickpi_gpout_s *priv = (FAR struct kickpi_gpout_s *)dev;
  int ret;

  DEBUGASSERT(priv != NULL && value != NULL);

  /* 读的是 EXT_PORT，即引脚上的实际电平，而不是输出寄存器的值。
   * 两者不一致时说明引脚被外部驱动着（短路、上下拉过强等）。
   */

  ret = rk3576_gpio_read(priv->bank, priv->pin);
  if (ret < 0)
    {
      return ret;
    }

  *value = (ret != 0);
  return OK;
}

static int kickpi_gpout_write(FAR struct gpio_dev_s *dev, bool value)
{
  FAR struct kickpi_gpout_s *priv = (FAR struct kickpi_gpout_s *)dev;

  DEBUGASSERT(priv != NULL);
  return rk3576_gpio_write(priv->bank, priv->pin, value);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_gpio_initialize
 *
 * Description:
 *   注册板上 GPIO 输出引脚为 /dev/gpioN。
 *
 * Returned Value:
 *   成功返回 OK。单个引脚注册失败只记录日志、继续注册其余引脚，
 *   因为一个引脚不可用不应拖垮整个板级初始化。
 *
 ****************************************************************************/

int kickpi_k7_gpio_initialize(void)
{
  uint32_t verid;
  int ret;
  int i;

  /* 自检：读 GPIO0 的 VER_ID。这一个读操作同时验证基址、PCLK 时钟、
   * MMU 映射三件事 —— 三者任一不对，总线都会返回 0 或 0xffffffff，
   * 而不会是一个像模像样的版本号。
   *
   * ★ 只排除这两个无效值，不做版本白名单匹配。
   *   最初按 Linux gpio-rockchip 里的 V2/V2_1 两个常量做精确匹配，
   *   而本板实测是 0x010219c8（第三个取值），结果把一块完全正常的
   *   控制器判成了故障。版本号会随修订递增，白名单是错的判据。
   */

  verid = rk3576_gpio_verid(0);
  if (verid == RK3576_GPIO_VER_INVALID_0 ||
      verid == RK3576_GPIO_VER_INVALID_1)
    {
      syslog(LOG_ERR,
             "ERROR: GPIO0 VER_ID = 0x%08" PRIx32 " —— 读不到控制器，"
             "基址、PCLK 或 MMU 映射有问题\n", verid);
      return -ENODEV;
    }

  syslog(LOG_INFO, "GPIO: 控制器版本 0x%08" PRIx32 "%s\n", verid,
         verid == RK3576_GPIO_VER_K7_OBSERVED ? "（与实测一致）" : "");

  for (i = 0; i < KICKPI_NGPOUT; i++)
    {
      FAR struct kickpi_gpout_s *p = &g_gpouts[i];

      p->gpio.gp_pintype = GPIO_OUTPUT_PIN;
      p->gpio.gp_ops     = &g_gpout_ops;

      ret = rk3576_gpio_setdir(p->bank, p->pin, true);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s (gpio%u-%u) 设方向失败: %d\n",
                 p->name, p->bank, p->pin, ret);
          continue;
        }

      ret = gpio_pin_register(&p->gpio, i);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s 注册 /dev/gpio%d 失败: %d\n",
                 p->name, i, ret);
          continue;
        }

      syslog(LOG_INFO, "GPIO: /dev/gpio%d = %s (gpio%u-%u)\n",
             i, p->name, p->bank, p->pin);
    }

  return OK;
}

/* TODO(M3)：GPIO 中断。需要在 SoC 层补 INT_EN/INT_TYPE/INT_POLARITY 的
 * 配置和 GIC 中断挂接，五个 bank 各占一个 SPI。届时可支持 GPIO_INTERRUPT_PIN。
 */

#endif /* CONFIG_DEV_GPIO */
