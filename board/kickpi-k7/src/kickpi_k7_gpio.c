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
#include "rk3576_pinmux.h"
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

/****************************************************************************
 * 输入引脚
 *
 * ★ cmocka_driver_gpio 需要一个输入设备（默认 /dev/gpio2），此前板上
 *   只注册了两个输出脚，用例的中断子项因 fd_in < 0 失败。
 *
 *   选 TP_INT_L（GPIO0_C5）作输入：它是触摸中断脚，板上有 10K 上拉到
 *   3V3（原理图 R5106），读它不会干扰任何东西；而且触摸驱动此时尚未
 *   接管中断，两者不冲突。
 */

struct kickpi_gpin_s
{
  struct gpio_dev_s gpio;
  uint8_t           bank;
  uint8_t           pin;
  uint8_t           minor;
  const char       *name;
  pin_interrupt_t   callback;
};

static int kickpi_gpin_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  if (value == NULL)
    {
      return -EINVAL;
    }

  *value = rk3576_gpio_read(p->bank, p->pin) != 0;
  return OK;
}

/****************************************************************************
 * ★ 输入脚不能只实现 go_read。
 *
 *   cmocka_driver_gpio 的中断子项会通过 GPIOC_SETPINTYPE 把引脚切成
 *   中断类型，上层在调用前用 DEBUGASSERT 检查回调非空 —— 缺哪个就直接
 *   断言失败并打印栈回溯，而不是返回错误码。所以 setpintype / attach /
 *   enable 三个必须一起给出。
 *
 *   底层中断能力本来就有（触摸用的就是它），这里只是把它接到 gpio
 *   上层的接口上。
 ****************************************************************************/

static int kickpi_gpin_attach(FAR struct gpio_dev_s *dev,
                              pin_interrupt_t callback)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  p->callback = callback;
  return OK;
}

static int kickpi_gpin_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)arg;

  if (p->callback != NULL)
    {
      p->callback(&p->gpio, p->minor);
    }

  return OK;
}

static int kickpi_gpin_enable(FAR struct gpio_dev_s *dev, bool enable)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  if (enable)
    {
      /* 双边沿：用例会主动拉动引脚验证两个方向都能触发。 */

      rk3576_gpio_irq_config(p->bank, p->pin, false, false);
      rk3576_gpio_irq_attach(p->bank, p->pin, kickpi_gpin_isr, p);
    }

  return rk3576_gpio_irq_enable(p->bank, p->pin, enable);
}

static int kickpi_gpin_setpintype(FAR struct gpio_dev_s *dev,
                                  enum gpio_pintype_e pintype)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  switch (pintype)
    {
      case GPIO_INPUT_PIN:
      case GPIO_INPUT_PIN_PULLUP:
      case GPIO_INPUT_PIN_PULLDOWN:
        rk3576_gpio_irq_enable(p->bank, p->pin, false);
        rk3576_gpio_setdir(p->bank, p->pin, false);
        break;

      case GPIO_INTERRUPT_PIN:
      case GPIO_INTERRUPT_HIGH_PIN:
      case GPIO_INTERRUPT_LOW_PIN:
      case GPIO_INTERRUPT_RISING_PIN:
      case GPIO_INTERRUPT_FALLING_PIN:
      case GPIO_INTERRUPT_BOTH_PIN:
        rk3576_gpio_setdir(p->bank, p->pin, false);
        rk3576_gpio_irq_config(p->bank, p->pin,
                               pintype == GPIO_INTERRUPT_RISING_PIN,
                               pintype == GPIO_INTERRUPT_HIGH_PIN ||
                               pintype == GPIO_INTERRUPT_LOW_PIN);
        break;

      default:
        return -EINVAL;
    }

  dev->gp_pintype = pintype;
  return OK;
}

static const struct gpio_operations_s g_gpin_ops =
{
  .go_read       = kickpi_gpin_read,
  .go_attach     = kickpi_gpin_attach,
  .go_enable     = kickpi_gpin_enable,
  .go_setpintype = kickpi_gpin_setpintype,
};

static struct kickpi_gpin_s g_gpins[] =
{
  {
    .bank = BOARD_TESTPIN_BANK,       /* GPIO4_B3，40 针扩展口，未接器件 */
    .pin  = BOARD_TESTPIN_PIN,
    .name = "testpin",
  },
};

#define KICKPI_NGPIN (sizeof(g_gpins) / sizeof(g_gpins[0]))

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

  /* 输入脚接在输出脚之后，编号顺延（当前为 /dev/gpio2）。 */

  for (i = 0; i < KICKPI_NGPIN; i++)
    {
      FAR struct kickpi_gpin_s *p = &g_gpins[i];
      int minor = KICKPI_NGPOUT + i;

      p->gpio.gp_pintype = GPIO_INPUT_PIN;
      p->gpio.gp_ops     = &g_gpin_ops;
      p->minor           = minor;

      rk3576_pinmux_set(p->bank, p->pin, RK3576_PINMUX_GPIO);

      /* ★ 必须给内部上拉。
       *
       *   这一脚在 40 针扩展口上、没有接任何器件，也就没有外部上下拉 ——
       *   悬空输入会随环境噪声抖动。测试用例把它配成双边沿中断后，
       *   抖动就变成持续的中断风暴，把 CPU 占满，现象是整块板子失去
       *   响应（既不报错也不复位），很容易误判成驱动挂死。
       *
       *   "选一个没接器件的引脚"解决了占用冲突，却引入了悬空问题 ——
       *   空闲和悬空是两回事。
       */

      rk3576_pinmux_setpull(p->bank, p->pin, RK3576_PULL_UP);

      ret = rk3576_gpio_setdir(p->bank, p->pin, false);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s (gpio%u-%u) 设为输入失败: %d\n",
                 p->name, p->bank, p->pin, ret);
          continue;
        }

      ret = gpio_pin_register(&p->gpio, minor);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s 注册 /dev/gpio%d 失败: %d\n",
                 p->name, minor, ret);
          continue;
        }

      syslog(LOG_INFO, "GPIO: /dev/gpio%d = %s (gpio%u-%u, 输入=%d)\n",
             minor, p->name, p->bank, p->pin,
             rk3576_gpio_read(p->bank, p->pin));
    }

  return OK;
}

/* TODO(M3)：GPIO 中断。需要在 SoC 层补 INT_EN/INT_TYPE/INT_POLARITY 的
 * 配置和 GIC 中断挂接，五个 bank 各占一个 SPI。届时可支持 GPIO_INTERRUPT_PIN。
 */

#endif /* CONFIG_DEV_GPIO */
