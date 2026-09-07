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
#include "arm64_internal.h"
#include <nuttx/wireless/bluetooth/bt_uart.h>
#include <nuttx/wireless/bluetooth/bt_uart_shim.h>

#include "rk3576_cru.h"
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

/* UART4 时钟与复位（出处见 bt_hw_reset 的说明） */

#define UART4_PCLK_CON      13
#define UART4_PCLK_BIT      13
#define UART4_SCLK_CON      14
#define UART4_SCLK_BIT      12
#define UART4_SEL_CON       63
#define UART4_SEL_MUX_SHIFT 8
#define UART4_SEL_DIV_SHIFT 0
#define UART4_SRST_P        221
#define UART4_SRST_S        236

#define BT_RST_BANK     1
#define BT_RST_PIN      23               /* GPIO1_C7 */
#define BT_WAKE_PIN     28               /* GPIO1_D4 */
#define BT_WAKEHOST_BANK 0
#define BT_WAKEHOST_PIN  9               /* GPIO0_B1，BT,wake_host_irq */

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
  /* ★ 本函数逐条照抄原厂内核的上电时序：
   *     kernel-6.1/net/rfkill/rfkill-bt.c : rfkill_rk_set_power()
   *
   *   我第一版是"拉低复位 -> 拉高 -> 等 200ms"，看起来合理，但漏掉了两件
   *   原厂做了的事，而且把第三件做反了：
   *
   *     1) wake_host 这根**本该是输入**的脚，上电期间要当输出拉高 20ms，
   *        之后再改回输入。
   *     2) RTS 不是一直摁着，而是**脉冲**：切成 GPIO -> 拉到使能(低)
   *        -> 100ms -> 拉到非使能(高) -> 交还给 UART 功能。
   *        我原来一直摁低，模组始终认为主机"随时能收"，但它自己的
   *        握手时序没走完。
   *     3) 复位只在"当前处于未使能"时才翻转，且低电平只有 20ms ——
   *        我改成 200ms 反而偏离了原厂。
   *
   *   这些都不是能从数据手册推出来的，只能读原厂代码。
   */

  /* ★ 0) 先开 UART4 的时钟并解复位。
   *
   *   这一步原来漏了 —— 我以为 /dev/ttyS1 注册成功就说明 UART 可用，
   *   但注册只证明驱动挂上了，不证明这个 UART 有时钟。没有时钟时寄存器
   *   读回全 0、写进去没有任何效果，而且**不报错**：表现是 HCI Reset
   *   超时，与"模组没上电"完全一样，我为此查了半天电源和复位时序。
   *
   *   16550 的内部回环自测（uart4_loopback_test）能一句话分开这两者。
   *
   *   出处：kernel-6.1/drivers/clk/rockchip/clk-rk3576.c
   *     PCLK_UART4  CLKGATE_CON(13) bit 13
   *     SCLK_UART4  CLKSEL_CON(63) mux@8(3位) div@0(8位)
   *                 CLKGATE_CON(14) bit 12
   *     clk_uart_p = {gpll, cpll, aupll, xin24m, ...} -> 取 3 = 24MHz，
   *     与 CONFIG_16550_UART1_CLOCK=24000000 一致
   *   复位：SRST_P_UART4=221  SRST_S_UART4=236
   */

  rk3576_clk_gate(UART4_PCLK_CON, UART4_PCLK_BIT, true);
  rk3576_clk_setmux(UART4_SEL_CON, UART4_SEL_MUX_SHIFT, 3, 3);   /* xin24m */
  rk3576_clk_setmux(UART4_SEL_CON, UART4_SEL_DIV_SHIFT, 8, 0);   /* 1 分频 */
  rk3576_clk_gate(UART4_SCLK_CON, UART4_SCLK_BIT, true);

  rk3576_reset(UART4_SRST_P, true);
  rk3576_reset(UART4_SRST_S, true);
  up_udelay(20);
  rk3576_reset(UART4_SRST_P, false);
  rk3576_reset(UART4_SRST_S, false);
  up_udelay(100);

  /* UART4 引脚（收发 + CTS 走 UART 功能；RTS 下面单独处理） */

  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_TX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RX,  UART4_PIN_FUNC);
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_CTS, UART4_PIN_FUNC);

  /* 1) BT wake 拉高（保持模组唤醒） */

  rk3576_pinmux_set(BT_RST_BANK, BT_WAKE_PIN, 0);
  rk3576_gpio_setdir(BT_RST_BANK, BT_WAKE_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_WAKE_PIN, true);

  /* 2) wake_host 暂当输出拉高 20ms */

  rk3576_pinmux_set(BT_WAKEHOST_BANK, BT_WAKEHOST_PIN, 0);
  rk3576_gpio_setdir(BT_WAKEHOST_BANK, BT_WAKEHOST_PIN, true);
  rk3576_gpio_write(BT_WAKEHOST_BANK, BT_WAKEHOST_PIN, true);
  up_mdelay(20);

  /* 3) 复位：低 20ms 再拉高（reset_gpio 是 ACTIVE_HIGH，高=使能） */

  rk3576_pinmux_set(BT_RST_BANK, BT_RST_PIN, 0);
  rk3576_gpio_setdir(BT_RST_BANK, BT_RST_PIN, true);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, false);
  up_mdelay(20);
  rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, true);

  /* 4) wake_host 改回输入 */

  rk3576_gpio_setdir(BT_WAKEHOST_BANK, BT_WAKEHOST_PIN, false);

  /* 5) RTS 脉冲：GPIO 拉低 100ms 再拉高，然后交还 UART 功能。
   *    uart_rts_gpios 在 dtb 里是 ACTIVE_LOW，所以"使能"= 低。
   */

  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RTS, 0);
  rk3576_gpio_setdir(UART4_PIN_BANK, UART4_PIN_RTS, true);
  rk3576_gpio_write(UART4_PIN_BANK, UART4_PIN_RTS, false);   /* 使能 */
  up_mdelay(100);
  rk3576_gpio_write(UART4_PIN_BANK, UART4_PIN_RTS, true);    /* 解除 */
  rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_RTS, UART4_PIN_FUNC);
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

/****************************************************************************
 * Name: uart4_loopback_test
 *
 * Description:
 *   用 16550 的内部回环（MCR bit4）自测 UART4。
 *
 *   ★ 为什么必须做这一步。
 *
 *     在此之前我一直假设 UART4 本身没问题，理由只是"/dev/ttyS1 注册成功
 *     了" —— 那只证明驱动**注册**成功，不证明这个 UART 的时钟、基址、
 *     引脚复用是对的。而"我们发不出去"和"模组不应答"在现象上完全一样：
 *     都是 HCI Reset 超时。
 *
 *     回环把这两者分开：MCR 的 LOOP 位一置，TX 在芯片内部直接回到 RX，
 *     完全不经过引脚和模组。写进去能读回来 -> UART 块、时钟、寄存器
 *     访问都正常，问题在外面；读不回来 -> 问题就在我们这边，去查模组
 *     是白费。
 *
 ****************************************************************************/

#define UART4_BASE   0x2ad70000
#define U16550_RBR   0x00
#define U16550_THR   0x00
#define U16550_LSR   0x14        /* REGWIDTH=32，寄存器间隔 4 字节 */
#define U16550_MCR   0x10
#define U16550_MCR_LOOP  (1 << 4)
#define U16550_LSR_THRE  (1 << 5)
#define U16550_LSR_DR    (1 << 0)

static int uart4_loopback_test(void)
{
  uint32_t mcr;
  uint32_t lsr;
  uint8_t  got = 0;
  int      i;

  /* 先把几个只读寄存器打出来：全 0 = 外设没响应（时钟/基址问题）；
   * 有合理值 = 外设活着，那问题就在回环逻辑或别处。
   * UART0（控制台，已知可用）同址偏移作对照。
   */

  printf("UART4@%08x: LSR=%02x MCR=%02x USR=%02x | UART0 对照: LSR=%02x\n",
         UART4_BASE,
         (unsigned)getreg32(UART4_BASE + U16550_LSR),
         (unsigned)getreg32(UART4_BASE + U16550_MCR),
         (unsigned)getreg32(UART4_BASE + 0x7c),
         (unsigned)getreg32(0x2ad40000 + U16550_LSR));

  mcr = getreg32(UART4_BASE + U16550_MCR);
  putreg32(mcr | U16550_MCR_LOOP, UART4_BASE + U16550_MCR);

  /* 清掉可能残留的一个字节 */

  if (getreg32(UART4_BASE + U16550_LSR) & U16550_LSR_DR)
    {
      (void)getreg32(UART4_BASE + U16550_RBR);
    }

  for (i = 0; i < 1000; i++)
    {
      if (getreg32(UART4_BASE + U16550_LSR) & U16550_LSR_THRE)
        {
          break;
        }

      up_udelay(10);
    }

  putreg32(0x5a, UART4_BASE + U16550_THR);

  for (i = 0; i < 1000; i++)
    {
      lsr = getreg32(UART4_BASE + U16550_LSR);
      if (lsr & U16550_LSR_DR)
        {
          got = (uint8_t)getreg32(UART4_BASE + U16550_RBR);
          break;
        }

      up_udelay(10);
    }

  putreg32(mcr, UART4_BASE + U16550_MCR);      /* 恢复 */

  /* ★ 这个回环结果**不可信**，只作参考，不要据此下结论。
   *
   *   /dev/ttyS1 已经注册，NuttX 串口驱动的接收中断是开着的。回环把字节
   *   送回 RBR 的瞬间，驱动的 ISR 会先把它取走，我们的轮询自然读到 0 ——
   *   于是一个完全正常的 UART 也会"回环失败"。我据此断言过"UART 自己
   *   就不通"，是假阳性。
   *
   *   真正的判据是上面那行寄存器对照：LSR/USR 与已知可用的 UART0 一致，
   *   就说明这个 UART 有时钟、能访问。要做真回环，得先把设备从驱动手里
   *   拿开（或在注册之前做），否则就是在和驱动抢寄存器。
   */

  printf("UART4 回环: 写 0x5a 读回 0x%02x（仅参考 —— 驱动 ISR 会抢先取走，"
         "此测试不足以判定 UART 好坏）\n", got);
  return OK;
}

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

  (void)uart4_loopback_test();

  /* ★ 先看模组自己驱动的那几根线，这能把"模组没活"和"UART 配错"分开。
   *
   *   CTS 由模组驱动：它就绪时会把 CTS 拉低（"你可以发"）。读到低说明
   *   模组已上电并在工作，问题就在 UART 这边；读到高（或浮空）说明模组
   *   根本没起来，去调波特率和收发引脚是白费。
   *
   *   这两种情况的最终现象都是"HCI Reset 无应答"，事后分不开 —— 所以
   *   要在发命令之前先把它们分开。
   */

  {
    int cts;
    int wake_host;

    int k;
    int lowseen = -1;

    rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_CTS, 0);   /* 暂当 GPIO */
    rk3576_gpio_setdir(UART4_PIN_BANK, UART4_PIN_CTS, false);

    /* ★ 连续采样 3 秒，而不是读一次。
     *
     *   模组从释放复位到把 CTS 拉低要多久，我们并不知道 —— 读一次读到高，
     *   分不清"永远不会来"和"还没到"。采样到第一次变低的时刻，这两者
     *   就分开了，而且顺带量出了真实的就绪时间。
     */

    for (k = 0; k < 60; k++)
      {
        if (rk3576_gpio_read(UART4_PIN_BANK, UART4_PIN_CTS) == 0)
          {
            lowseen = k * 50;
            break;
          }

        up_mdelay(50);
      }

    cts = (lowseen >= 0) ? 0 : 1;
    if (lowseen >= 0)
      {
        printf("CTS 在释放复位后约 %d ms 变低\n", lowseen);
      }
    else
      {
        printf("CTS 3 秒内始终为高\n");
      }

    /* BT,wake_host_irq = GPIO0_B1(9)，也是模组驱动的 */

    rk3576_pinmux_set(0, 9, 0);
    rk3576_gpio_setdir(0, 9, false);
    up_udelay(100);
    wake_host = rk3576_gpio_read(0, 9);

    /* ★ 把我们自己驱动的脚回读一遍。
     *
     *   写下去不等于生效：复用没切成 GPIO、方向没设成输出，写操作都不会
     *   报错，但电平根本没变 —— 现象与"模组坏了"完全一样。这个项目里
     *   已经栽过一次（把命名当判据而不是回读），这里不重犯。
     */

    printf("线状态: CTS(GPIO1_C3)=%d  wake_host(GPIO0_B1)=%d\n",
           cts, wake_host);
    printf("自驱回读: BT_RST(GPIO1_C7)=%d 期望1  "
           "BT_WAKE(GPIO1_D4)=%d 期望1  WIFI_EN(GPIO1_C6)=%d 期望1\n",
           rk3576_gpio_read(BT_RST_BANK, BT_RST_PIN),
           rk3576_gpio_read(BT_RST_BANK, BT_WAKE_PIN),
           rk3576_gpio_read(1, 22));
    printf("        CTS 为 0 说明模组已就绪；为 1 说明模组没起来\n");

    rk3576_pinmux_set(UART4_PIN_BANK, UART4_PIN_CTS, UART4_PIN_FUNC);
  }

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
