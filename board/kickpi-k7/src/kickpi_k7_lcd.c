/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_lcd.c
 *
 * 5 寸 MIPI 屏（F050008M01，720x1280）的板级电源与复位。
 *
 * 屏和触摸共用 VCC3V3_LCD_S0 这一条电源轨，因此开关必须集中在这里。
 * 引脚出处见 include/board.h 顶部对原理图第 27 页的摘录。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <syslog.h>

#include <arch/board/board.h>

#include "arm64_internal.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_lcd_powered;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_lcd_power(bool on)
{
  if (on == g_lcd_powered)
    {
      return OK;
    }

  /* 两个脚都可能被引导器留成别的复用，先明确切回 GPIO。 */

  rk3576_pinmux_set(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN,
                    RK3576_PINMUX_GPIO);
  rk3576_pinmux_set(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN,
                    RK3576_PINMUX_GPIO);

  rk3576_gpio_setdir(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, true);
  rk3576_gpio_setdir(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN, true);

  if (!on)
    {
      /* 断电前先把屏按在复位态，避免悬空脚倒灌。 */

      rk3576_gpio_write(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN, false);
      rk3576_gpio_write(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, false);
      g_lcd_powered = false;
      return OK;
    }

  /* 上电时序：复位保持低 -> 开电源轨 -> 等稳 -> 释放复位。
   *
   * LCD_PWREN_H 走的是 S8050 + P-MOS 组合开关，导通有几百微秒量级的
   * 延迟，加上面板自身的上电要求，这里取 20ms 余量。
   */

  rk3576_gpio_write(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN, false);
  rk3576_gpio_write(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, true);
  up_mdelay(20);

  rk3576_gpio_write(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN, true);
  up_mdelay(20);

  g_lcd_powered = true;

  /* 电源轨在位判据 —— 不靠"写进去了"，靠一个能被证伪的观测。
   *
   * 转接板（RK_IF_RK3568_FPC）的 1.8V 由板上 U1(WL2801E18-5) 从
   * VCC3V3_LCD 生成，触摸 I2C 主板侧的两个 10K 上拉(R1/R3)接的正是
   * 这个 1.8V。因此：
   *   PWREN=0 -> 无 3V3_LCD -> 无 1V8 -> 上拉消失，SCL/SDA 应读到 0
   *   PWREN=1 -> 上拉恢复，SCL/SDA 应读到 1
   * 两次读数不同，就证明这条轨确实受我们控制且真的通了；
   * 若两次都是 1，说明高电平另有来源（如 SoC 内部上拉），
   * 电源轨是否真的起来了并未被证明。
   */

  {
    /* 触摸 I2C 的两根线，功能号 9（见 rk3576_i2c.c 的 I2C0 条目）。 */

    const int scl = 17;                     /* GPIO0_C1 */
    const int sda = 18;                     /* GPIO0_C2 */
    int lo_scl;
    int lo_sda;
    int hi_scl;
    int hi_sda;

    /* ★ 必须先关内部上拉。上一版没关，两次都读到 1，判据毫无区分力 ——
     * 高电平来自 SoC 自己，外部上拉在不在根本看不出来。
     */

    rk3576_pinmux_set(0, scl, RK3576_PINMUX_GPIO);
    rk3576_pinmux_set(0, sda, RK3576_PINMUX_GPIO);
    rk3576_pinmux_setpull(0, scl, RK3576_PULL_NONE);
    rk3576_pinmux_setpull(0, sda, RK3576_PULL_NONE);
    rk3576_gpio_setdir(0, scl, false);
    rk3576_gpio_setdir(0, sda, false);

    rk3576_gpio_write(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, false);
    up_mdelay(100);
    lo_scl = rk3576_gpio_read(0, scl);
    lo_sda = rk3576_gpio_read(0, sda);

    rk3576_gpio_write(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, true);
    up_mdelay(100);
    hi_scl = rk3576_gpio_read(0, scl);
    hi_sda = rk3576_gpio_read(0, sda);

    syslog(LOG_INFO,
           "LCD: 电源轨判据（内部拉已关）PWREN=0 -> SCL/SDA=%d/%d，"
           "PWREN=1 -> %d/%d —— %s\n",
           lo_scl, lo_sda, hi_scl, hi_sda,
           (lo_scl == hi_scl && lo_sda == hi_sda) ?
           (hi_scl ? "恒为高，上拉与 PWREN 无关" : "恒为低，1.8V 轨没起来") :
           "随 PWREN 变化，VCC3V3_LCD -> VCC_1V8 链路正常");

    /* ★ 判据做完必须把复用还给 I2C0，否则控制器与引脚断开，
     * 后续传输全部超时（上一版就是这样把 -ENXIO 变成了 -ETIMEDOUT）。
     */

    rk3576_pinmux_set(0, scl, 9);
    rk3576_pinmux_set(0, sda, 9);
  }

  /* 读回确认电平真的变了 —— 复用配错时写入不报错但引脚不动，
   * 这个坑在本端口的 I2C2 上踩过一次。
   */

  syslog(LOG_INFO,
         "LCD: 电源轨已开 PWREN(gpio%d-%d)=%d RST(gpio%d-%d)=%d\n",
         BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN,
         rk3576_gpio_read(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN),
         BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN,
         rk3576_gpio_read(BOARD_LCD_RST_BANK, BOARD_LCD_RST_PIN));

  return OK;
}
