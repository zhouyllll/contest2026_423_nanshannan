/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_bt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AP6256 蓝牙（BCM4345C5）经 UART4 接入。
 *
 * ★ 硬件参数出自原厂 dtb，启动序列出自原厂 SDK：
 *
 *     /serial@2ad70000            okay，interrupts = SPI 80
 *     /pinctrl/uart4/uart4m1-*    xfer=GPIO1_C4/C5 cts=GPIO1_C3 rts=GPIO1_C2
 *                                 （功能 9；uart4 节点 pinctrl-0 指向 m1）
 *     /wireless-bluetooth         BT,reset_gpio=GPIO1_C7(23)
 *                                 BT,wake_gpio =GPIO1_D4(28)
 *                                 pinctrl-1 = "rts_gpio"  ← 见下
 *     external/rkwifibt/scripts/wifibt-init.sh:
 *       brcm_patchram_plus1 --baudrate 1500000 --tosleep 200000 ...
 *
 * ★ 挂在 16550 驱动的"UART1"槽位：drivers/serial/uart_16550.c 只实现到
 *   UART3。驱动的编号是它自己的，与 SoC 的 UART 编号无关。
 *
 * ★ 波特率 1500000 而非驱动默认的 2000000：UART 时钟 24MHz，16550 除数
 *   = 24e6/(16*波特率)。1500000 除数为 1（精确）；2000000 需要 0.75，
 *   表示不出来，会被取整成 1 —— 线上跑 1.5M 却已命令芯片切到 2M，两边
 *   对不上，结果是乱码，而且看起来像"固件加载失败"。原厂用的正是 1.5M。
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <debug.h>
#include <unistd.h>
#include <termios.h>

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

#define BT_UART_DEV     "/dev/ttyS1"     /* = SoC 的 UART4 */

#define UART4_PIN_BANK  1
#define UART4_PIN_RTS   18               /* GPIO1_C2 */
#define UART4_PIN_CTS   19               /* GPIO1_C3 */
#define UART4_PIN_TX    20               /* GPIO1_C4 */
#define UART4_PIN_RX    21               /* GPIO1_C5 */
#define UART4_PIN_FUNC  9

#define BT_RST_BANK     1
#define BT_RST_PIN      23               /* GPIO1_C7 */
#define BT_WAKE_PIN     28               /* GPIO1_D4 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bt_hw_reset
 *
 * Description:
 *   配好 UART4 引脚并复位蓝牙模组。probe 与 initialize 共用。
 *
 ****************************************************************************/

static void bt_hw_reset(void)
{
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_TX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_CTS, UART4_PIN_FUNC);

  /* ★ RTS 当 GPIO 常拉低，不交给 UART 自动流控。
   *
   *   依据是原厂 dtb：/wireless-bluetooth 除默认的 uart4m1-rtsn 之外，
   *   还准备了 pinctrl-1 = "rts_gpio"，把同一根 GPIO1_C2 复用成普通
   *   GPIO。原厂为这根脚特意准备两个状态，说明加载固件期间要手动摁住。
   *
   *   RTS 低 = "我可以收"。若由自动流控驱动而它停在高，模组一个字节
   *   都不会发 —— 表现就是第一条 HCI 命令超时，而这跟"引脚配错""模组
   *   没上电"在现象上完全一样，分不开。
   */

  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RTS, 0);
  rk3576_gpio_setdir(UART4_PIN_BANK, UART4_PIN_RTS, true);
  rk3576_gpio_write(UART4_PIN_BANK, UART4_PIN_RTS, false);

  /* 复位。原厂走 rfkill，底下就是这根脚。 */

  rk3576_pinmux_set(BT_RST_BANK, BT_RST_PIN, 0);
  rk3576_gpio_setdir(BT_RST_BANK, BT_RST_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, false);
  up_mdelay(20);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, true);

  /* 模组起振并让 HCI 就绪需要时间。原厂 --tosleep 200000 即 200ms。
   * 省掉它的表现是第一条命令无响应，跟引脚配错分不开。
   */

  up_mdelay(200);

  /* wake 脚拉高保持唤醒 */

  rk3576_pinmux_set(BT_RST_BANK, BT_WAKE_PIN, 0);
  rk3576_gpio_setdir(BT_RST_BANK, BT_WAKE_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_WAKE_PIN, true);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_bt_probe
 *
 * Description:
 *   最小握手：115200 下发一条 HCI Reset，等约 500ms 看有没有回应。
 *
 *   ★ 为什么必须先有这一步。
 *
 *     btuart_register() 一进去就是完整的固件加载：换波特率、进下载模式、
 *     把 70KB patchram 逐块灌进去。模组不应答时每条命令各等 100ms、
 *     逐块重试，过程很长；而它是**前台任务**，NSH 会一直等着，期间没有
 *     提示符、loader 也发不进去。我已经这样把板子弄到只能 MASKROM 恢复。
 *
 *     HCI Reset 是最干净的判据：任何 BCM 模组上电后都必须在 115200 下
 *     应答它（7 字节 command complete）。答了 -> 电源、复位、引脚、波特率
 *     全对；不答 -> 问题就在这四者之一，去调固件加载是白费。有界，
 *     失败立刻返回。
 *
 ****************************************************************************/

int kickpi_k7_bt_probe(void)
{
  static const uint8_t hci_reset[] =
    {
      0x01, 0x03, 0x0c, 0x00        /* HCI_COMMAND_PKT + OCF 0x0c03 + plen 0 */
    };

  uint8_t rsp[16];
  struct termios tio;
  int total = 0;
  int fd;
  int n;
  int i;

  bt_hw_reset();

  fd = open(BT_UART_DEV, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      printf("打不开 %s: %d\n", BT_UART_DEV, errno);
      return -errno;
    }

  if (tcgetattr(fd, &tio) == 0)
    {
      cfsetspeed(&tio, 115200);
      tio.c_cflag &= ~CRTSCTS;          /* RTS 已由 GPIO 摁住 */
      tcsetattr(fd, TCSANOW, &tio);
    }

  n = write(fd, hci_reset, sizeof(hci_reset));
  printf("已发 HCI Reset (%d 字节)，等应答…\n", n);

  for (i = 0; i < 50 && total < (int)sizeof(rsp); i++)
    {
      n = read(fd, rsp + total, sizeof(rsp) - total);
      if (n > 0)
        {
          total += n;
        }

      usleep(10000);
    }

  close(fd);

  if (total == 0)
    {
      printf("模组无应答 —— 问题在电源/复位/引脚/波特率，"
             "不要去调固件加载\n");
      return -ETIMEDOUT;
    }

  printf("收到 %d 字节:", total);
  for (i = 0; i < total; i++)
    {
      printf(" %02x", rsp[i]);
    }

  printf("\n");

  /* command complete 应为 04 0e 04 01 03 0c 00 */

  if (total >= 7 && rsp[0] == 0x04 && rsp[1] == 0x0e &&
      rsp[4] == 0x03 && rsp[5] == 0x0c)
    {
      printf("HCI Reset 应答正确，模组在线\n");
      return OK;
    }

  printf("有数据但不是 HCI Reset 的应答 —— 波特率或收发引脚可能反了\n");
  return -EIO;
}

/****************************************************************************
 * Name: kickpi_k7_bt_initialize
 *
 * Description:
 *   完整初始化：复位 + 交给 bt_uart_bcm4343x 驱动加载固件。
 *   ★ 先跑 kickpi_k7_bt_probe() 确认模组在线再用这个。
 *
 ****************************************************************************/

int kickpi_k7_bt_initialize(void)
{
  FAR struct btuart_lowerhalf_s *lower;
  int ret;

  bt_hw_reset();

  syslog(LOG_INFO, "蓝牙: 模组已复位（GPIO%d_%d），UART4 引脚就绪\n",
         BT_RST_BANK, BT_RST_PIN);

  lower = btuart_shim_getdevice(BT_UART_DEV);
  if (lower == NULL)
    {
      syslog(LOG_ERR, "ERROR: 蓝牙: 打不开 %s\n", BT_UART_DEV);
      return -ENODEV;
    }

  /* bt_uart_bcm4343x.c 的 btuart_create() 会在这里面完成固件加载；
   * 固件由 kickpi_k7_bt_firmware.c 以 g_bt_firmware_hcd[] 编入镜像。
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
