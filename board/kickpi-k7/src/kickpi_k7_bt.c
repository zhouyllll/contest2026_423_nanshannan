/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_bt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AP6256 蓝牙（BCM4345C5）经 UART4 接入。
 *
 * ★ 硬件参数出自原厂 dtb，不是推断的：
 *
 *     /serial@2ad70000            status = okay，interrupts = SPI 80
 *     /pinctrl/uart4/uart4m1-*    xfer=GPIO1_C4/C5 cts=GPIO1_C3 rts=GPIO1_C2
 *                                 （全部功能 9；uart4 节点 pinctrl-0 指向 m1）
 *     /wireless-bluetooth         BT,reset_gpio = GPIO1_C7 (pin 23)
 *                                 BT,wake_gpio  = GPIO1_D4 (pin 28)
 *
 * ★ 为什么挂在 16550 驱动的"UART1"槽位：drivers/serial/uart_16550.c 只实现
 *   到 UART3，没有 UART4。驱动的编号是**它自己的**，与 SoC 的 UART 编号
 *   无关 —— 我们把 UART4 的基址和中断填进它的 UART1 槽，得到 /dev/ttyS1。
 *
 * ★ 波特率必须是 1500000，不能用驱动默认的 2000000。
 *
 *     UART 时钟 24MHz，16550 的除数 = 24e6/(16*波特率)：
 *       1500000 -> 除数 1，精确
 *       2000000 -> 除数 0.75，**表示不出来**，会被取整成 1
 *     取整之后线上实际是 1.5M，而我们已经命令芯片切到 2M，两边对不上，
 *     结果是一片乱码 —— 而且看起来像"固件加载失败"，方向会查偏。
 *
 *     原厂脚本用的也正是这个值（external/rkwifibt/scripts/wifibt-init.sh）：
 *       brcm_patchram_plus1 --baudrate 1500000 --use_baudrate_for_download
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/wireless/bluetooth/bt_uart.h>
#include <nuttx/wireless/bluetooth/bt_uart_shim.h>

#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "kickpi_k7.h"

#if defined(CONFIG_BLUETOOTH_UART_SHIM) && defined(CONFIG_BLUETOOTH_BCM4343X)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BT_UART_DEV     "/dev/ttyS1"     /* = SoC 的 UART4，见文件头说明 */

#define UART4_PIN_BANK  1
#define UART4_PIN_RTS   18               /* GPIO1_C2 */
#define UART4_PIN_CTS   19               /* GPIO1_C3 */
#define UART4_PIN_TX    20               /* GPIO1_C4 */
#define UART4_PIN_RX    21               /* GPIO1_C5 */
#define UART4_PIN_FUNC  9

#define BT_RST_BANK     1
#define BT_RST_PIN      23               /* GPIO1_C7，BT,reset_gpio */
#define BT_WAKE_PIN     28               /* GPIO1_D4，BT,wake_gpio  */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_bt_initialize
 *
 * Description:
 *   复位蓝牙模组、配好 UART4 引脚，然后把 /dev/ttyS1 交给
 *   bt_uart_bcm4343x 驱动（它会在 btuart_register 内部完成
 *   换波特率 -> 下载 patchram -> 重启动 的三步）。
 *
 *   必须在串口驱动注册之后调用。
 *
 ****************************************************************************/

int kickpi_k7_bt_initialize(void)
{
  FAR struct btuart_lowerhalf_s *lower;
  int ret;

  /* 1) UART4 引脚（m1 组，功能 9）。
   *
   * ★ RTS/CTS 必须一起配上。BCM4345 在下载 patchram 时依赖硬件流控 ——
   *   70KB 固件在 1.5Mbaud 下连续灌入，没有流控必然溢出，表现为下载中途
   *   失败，而失败点每次都不一样，看起来像"固件文件坏了"。
   */

  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_TX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_CTS, UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RTS, UART4_PIN_FUNC);

  /* 2) 复位模组。原厂走 rfkill，底下就是这根脚。 */

  rk3576_pinmux_set(BT_RST_BANK, BT_RST_PIN, 0);      /* 功能 0 = GPIO */
  rk3576_gpio_setdir(BT_RST_BANK, BT_RST_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, false);
  up_mdelay(20);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, true);

  /* 模组内部起振并让 HCI 就绪需要时间。原厂 brcm_patchram_plus1 用的是
   * --tosleep 200000（200ms），这里取同一个量级。省掉它的表现是第一条
   * HCI 命令无响应，和"引脚配错"分不开。
   */

  up_mdelay(200);

  /* wake 脚拉高保持模组唤醒 */

  rk3576_pinmux_set(BT_RST_BANK, BT_WAKE_PIN, 0);
  rk3576_gpio_setdir(BT_RST_BANK, BT_WAKE_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_WAKE_PIN, true);

  syslog(LOG_INFO, "蓝牙: 模组已复位（GPIO%d_%d），UART4 引脚就绪\n",
         BT_RST_BANK, BT_RST_PIN);

  /* 3) 把串口包成 btuart 下半部 */

  lower = btuart_shim_getdevice(BT_UART_DEV);
  if (lower == NULL)
    {
      syslog(LOG_ERR, "ERROR: 蓝牙: 打不开 %s\n", BT_UART_DEV);
      return -ENODEV;
    }

  /* 4) 注册。bt_uart_bcm4343x.c 提供的 btuart_create() 会在这里面
   *    完成固件加载 —— 固件由 kickpi_k7_bt_firmware.c 以
   *    g_bt_firmware_hcd[] 提供，编译进镜像。
   */

  ret = btuart_register(lower);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 蓝牙: btuart_register 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "蓝牙: HCI 就绪（BCM4345C5，固件 %ld 字节）\n",
         g_bt_firmware_len);
  return OK;
}

#endif /* CONFIG_BLUETOOTH_UART_SHIM && CONFIG_BLUETOOTH_BCM4343X */
