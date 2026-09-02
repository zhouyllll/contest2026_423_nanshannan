/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_touch.c
 *
 * 5 寸 MIPI 屏（F050008M01）转接板上的电容触摸。
 *
 * ★ 芯片是 FocalTech FT8756 @ I2C0:0x38，不是 Goodix GT9xx。
 *
 *   出处是原厂 Android 的启动日志：
 *     input: fts_ts as /devices/platform/27300000.i2c/i2c-0/0-0038/input/input1
 *     [FTS_TS/I]fts_get_chip_types:verify id:0x8756
 *     [FTS_TS/I]fts_parse_dt:max touch number:10, irq gpio:21, reset gpio:24
 *
 *   之前按 GT9xx 扫 0x5d / 0x14 一直无应答，原因就是型号猜错了。
 *   总线（0x27300000 = I2C0）和两个引脚（IRQ=GPIO0_C5=21、
 *   RST=GPIO0_D0=24）此前从原理图推出的结论则与日志完全一致。
 *
 *   FT8756 的多点触摸寄存器布局与 FT5x06 系列兼容，直接用 NuttX 自带的
 *   ft5x06 驱动。
 *
 * ★ 触摸的复位脚（TP_RST）与面板的复位脚（LCD_RESET）是两根不同的线，
 *   动 TP_RST 不会影响 U-Boot 已经初始化好的面板。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/ft5x06.h>

#include "arm64_internal.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_i2c.h"
#include <arch/board/board.h>
#include "kickpi_k7.h"

#ifdef CONFIG_INPUT_FT5X06

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TOUCH_I2C_BUS      BOARD_TP_I2C_BUS
#define TOUCH_I2C_ADDR     0x38          /* 原厂日志的 0-0038 */

#define TOUCH_IRQ_BANK     BOARD_TP_INT_BANK
#define TOUCH_IRQ_PIN      BOARD_TP_INT_PIN
#define TOUCH_RST_BANK     BOARD_TP_RST_BANK
#define TOUCH_RST_PIN      BOARD_TP_RST_PIN

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifndef CONFIG_FT5X06_POLLMODE
static int kickpi_touch_attach(const struct ft5x06_config_s *config,
                               xcpt_t isr, void *arg)
{
  UNUSED(config);
  return rk3576_gpio_irq_attach(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, isr, arg);
}

static void kickpi_touch_enable(const struct ft5x06_config_s *config,
                                bool enable)
{
  UNUSED(config);
  rk3576_gpio_irq_enable(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, enable);
}

static void kickpi_touch_clear(const struct ft5x06_config_s *config)
{
  UNUSED(config);

  /* 本端口的 GPIO bank ISR 在派发给回调之后才写 EOI 清除挂起位
   * （见 rk3576_gpio.c 里对电平触发的说明），因此这里无需再清一次。
   */
}
#endif

static void kickpi_touch_wakeup(const struct ft5x06_config_s *config)
{
  UNUSED(config);

  /* 本板没有单独的 WAKE 脚，靠复位脚唤醒。 */
}

static void kickpi_touch_nreset(const struct ft5x06_config_s *config,
                                bool state)
{
  UNUSED(config);

  /* 低有效：state=false 进复位，state=true 出复位。 */

  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, state);
}

static const struct ft5x06_config_s g_touch_config =
{
  .address   = TOUCH_I2C_ADDR,
  .frequency = 400000,
#ifndef CONFIG_FT5X06_POLLMODE
  .attach    = kickpi_touch_attach,
  .enable    = kickpi_touch_enable,
  .clear     = kickpi_touch_clear,
#endif
  .wakeup    = kickpi_touch_wakeup,
  .nreset    = kickpi_touch_nreset,
};

/****************************************************************************
 * Name: kickpi_touch_probe
 *
 * Description:
 *   读 FT8756 的芯片 ID 确认器件在总线上应答。
 *
 *   ★ 注册函数不访问器件，注册成功不等于芯片存在 —— 这个坑在 GT9xx
 *     那版踩过一次。这里读 0xA3（chip id 高字节）与 0x9F（低字节），
 *     原厂日志报的是 0x8756。
 *
 ****************************************************************************/

static int kickpi_touch_probe(struct i2c_master_s *i2c)
{
  struct i2c_msg_s msg[2];
  uint8_t reg;
  uint8_t val;
  uint8_t idh;
  uint8_t idl;
  int ret;
  int i;

  static const uint8_t regs[2] =
  {
    0xa3,    /* chip id 高字节 */
    0x9f     /* chip id 低字节 */
  };

  uint8_t got[2] =
  {
    0, 0
  };

  for (i = 0; i < 2; i++)
    {
      reg = regs[i];

      msg[0].frequency = 400000;
      msg[0].addr      = TOUCH_I2C_ADDR;
      msg[0].flags     = 0;
      msg[0].buffer    = &reg;
      msg[0].length    = 1;

      msg[1].frequency = 400000;
      msg[1].addr      = TOUCH_I2C_ADDR;
      msg[1].flags     = I2C_M_READ;
      msg[1].buffer    = &val;
      msg[1].length    = 1;

      ret = I2C_TRANSFER(i2c, msg, 2);
      if (ret < 0)
        {
          return ret;
        }

      got[i] = val;
    }

  idh = got[0];
  idl = got[1];

  syslog(LOG_INFO,
         "触摸: FT 系列应答 芯片ID=0x%02x%02x（原厂日志为 0x8756）\n",
         idh, idl);
  return OK;
}

/****************************************************************************
 * Public Functions
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

  /* 复位时序。TP_RST 与面板复位无关，可以放心操作。 */

  rk3576_pinmux_set(TOUCH_RST_BANK, TOUCH_RST_PIN, RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, false);
  up_mdelay(10);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  up_mdelay(200);              /* FT8756 上电到可通信约需 200ms */

  ret = kickpi_touch_probe(i2c);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: 触摸 0x%02x 无应答(%d)，不注册输入设备\n",
             TOUCH_I2C_ADDR, ret);
      return -ENODEV;
    }

#ifndef CONFIG_FT5X06_POLLMODE
  rk3576_pinmux_set(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, false);

  ret = rk3576_gpio_irq_config(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN,
                               false, false);   /* 下降沿触发 */
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸中断脚配置失败: %d\n", ret);
      return ret;
    }
#endif

  ret = ft5x06_register(i2c, &g_touch_config, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/input0 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO,
         "触摸: /dev/input0 就绪（FT8756 @I2C%d:0x%02x, %s）\n",
         TOUCH_I2C_BUS, TOUCH_I2C_ADDR,
#ifdef CONFIG_FT5X06_POLLMODE
         "轮询模式"
#else
         "中断模式"
#endif
        );
  return OK;
}

#endif /* CONFIG_INPUT_FT5X06 */
