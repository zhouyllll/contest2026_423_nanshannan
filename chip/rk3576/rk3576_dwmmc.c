/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_dwmmc.c
 *
 * RK3576 的 SD 卡控制器（Synopsys DesignWare MSHC，mmc@2a310000）。
 *
 * ★ 与 eMMC 不是同一种控制器，rk3576_sdhci.c 的代码不能复用。
 *   两者的区别见 hardware/rk3576_dwmmc.h 顶部。
 *
 * 板级依据（全部取自原厂 dtb，非推测）：
 *   基址        0x2a310000，长度 16KB
 *   中断        GIC_SPI 251
 *   电源域      PD_SDGMAC（与 GMAC 共用，已有 rk3576_power_on）
 *   总线宽度    4
 *   FIFO 深度   256
 *   引脚(func1) bus4 = GPIO2_A0..A3   cmd = GPIO2_A4   clk = GPIO2_A5
 *               det  = GPIO0_A7       pwren = GPIO0_B6
 *   时钟        clk-rk3576.c
 *     COMPOSITE(CCLK_SRC_SDMMC0, ... gpll_cpll_24m_p,
 *               RK3576_CLKSEL_CON(105), 13, 2, MFLAGS, 7, 6, DFLAGS,
 *               RK3576_CLKGATE_CON(43), 1, GFLAGS)
 *     GATE(HCLK_SDMMC0, ... RK3576_CLKGATE_CON(43), 2, GFLAGS)
 *
 *   ★ 时钟按**名字**从 clk-rk3576.c 取，不按 dtb 里的时钟编号推算：
 *     厂商内核的编号与主线不是固定偏移（I2C0 差 8，SDMMC0 差 7），
 *     按偏移换算会算到别的时钟上。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_power.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_dwmmc.h"
#include "hardware/rk3576_dwmmc.h"

#ifdef CONFIG_RK3576_DWMMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DWMMC_GATE_CON      43
#define DWMMC_GATE_CCLK     1        /* CCLK_SRC_SDMMC0 */
#define DWMMC_GATE_HCLK     2        /* HCLK_SDMMC0     */
#define DWMMC_SEL_CON       105
#define DWMMC_SEL_MUX_SHIFT 13       /* 2 位：0=gpll 1=cpll 2=24M */
#define DWMMC_SEL_MUX_GPLL  0
#define DWMMC_SEL_MUX_24M   2
#define DWMMC_GPLL_HZ       1188000000u

/* ★ 控制器内部在 CIU 时钟上还有一级固定 2 分频（CLKGEN）。
 * 出处 dw_mmc-rockchip.c 的 RK3288_CLKGEN_DIV。算卡时钟必须算上它。
 */

#define DWMMC_CLKGEN_DIV    2
#define DWMMC_SEL_DIV_SHIFT 7        /* 6 位分频            */

#define DWMMC_SRST_H        269      /* SRST_H_SDMMC0 */

/* 引脚 */

#define PIN_FUNC            1
#define PIN_BUS_BANK        2
#define PIN_BUS_FIRST       0        /* GPIO2_A0..A3 */
#define PIN_CMD_BANK        2
#define PIN_CMD             4
#define PIN_CLK_BANK        2
#define PIN_CLK             5
#define PIN_DET_BANK        0
#define PIN_DET             7
#define PIN_PWR_BANK        0
#define PIN_PWR             14

/* SDIO 实例（WiFi，AP6256）。出处：原厂 dtb 与 SDK 的 clk-rk3576.c
 *
 *   /mmc@2a320000  dw-mshc 4bit + cap-sdio-irq + non-removable
 *   CCLK_SRC_SDIO  CLKSEL_CON(104) mux@6(2位) div@0(6位)，
 *                  gate CLKGATE_CON(42) bit 11
 *   HCLK_SDIO      gate CLKGATE_CON(42) bit 12
 *   SRST_H_SDIO    684
 *   引脚（sdmmc1m0，全部功能 2）：
 *     bus4 = GPIO1_B4..B7(12..15)  cmd = GPIO1_C0(16)  clk = GPIO1_C1(17)
 */

#define SDIO_GATE_CON       42
#define SDIO_GATE_CCLK      11
#define SDIO_GATE_HCLK      12
#define SDIO_SEL_CON        104
#define SDIO_SEL_MUX_SHIFT  6
#define SDIO_SEL_DIV_SHIFT  0
#define SDIO_SRST_H         684
#define SDIO_PIN_FUNC       2
#define SDIO_PIN_BANK       1
#define SDIO_PIN_BUS_FIRST  12
#define SDIO_PIN_CMD        16
#define SDIO_PIN_CLK        17

/* WiFi 模组的上电时序（/sdio-pwrseq）：
 *   reset-gpios = GPIO1 pin22，**低有效**；post-power-on-delay-ms = 200
 */

#define WIFI_RST_BANK       1
#define WIFI_RST_PIN        22

/* ★ 模组有**两路独立使能**，此前只拉起了 WiFi 那一路。
 *
 *   原理图：WIFI_REG_ON_H = GPIO1_C6，BT_REG_ON_H = GPIO1_C7，两个独立信号。
 *   原厂 dtb 也是分开声明的：
 *     /sdio-pwrseq      reset-gpios  = <&gpio1 22 GPIO_ACTIVE_LOW>
 *     /wireless-bluetooth BT,reset_gpio = <&gpio1 23 GPIO_ACTIVE_HIGH>
 *
 *   SKW6621S 是单 die 组合芯片，WiFi 与蓝牙共用同一条 SDIO。只拉 WiFi
 *   一路时，芯片内部的启动可能停在半路 —— 而它正是应答 CMD5 的那一方。
 *   现象与实测吻合：RESP_ERR、无 RTO、无 RCRC，即"有东西在拉命令线，
 *   但回的帧不合法"。
 */

#define BT_RST_BANK         1
#define BT_RST_PIN          23
#define WIFI_PWRON_DELAY_MS 200

/* 一个控制器实例的硬件参数。
 *
 * ★ 板上两个 dw-mshc 的时钟、复位、引脚**完全不同**，只有寄存器布局
 *   一样。把这些做成表而不是写死，是因为写死的那份在加第二个实例时
 *   不会编译报错 —— 它会用 SD 的时钟去开 SDIO，表现为寄存器读得到、
 *   卡不动，和"没插卡"分不开。
 */

struct dwmmc_hw_s
{
  uint32_t base;
  uint8_t  gate_con;
  uint8_t  gate_cclk;
  uint8_t  gate_hclk;
  uint8_t  sel_con;
  uint8_t  sel_mux_shift;
  uint8_t  sel_div_shift;
  uint16_t srst_h;
  uint8_t  pin_func;
  uint8_t  bus_bank;
  uint8_t  bus_first;
  uint8_t  cmd_bank;
  uint8_t  cmd_pin;
  uint8_t  clk_bank;
  uint8_t  clk_pin;
  int8_t   det_bank;          /* <0：不可插拔，无插卡检测 */
  int8_t   det_pin;
  int8_t   pwr_bank;          /* <0：无控制器 PWREN 引脚   */
  int8_t   pwr_pin;
  bool     is_sdio;           /* 判据用 CMD5 而不是 CMD8   */
};

static const struct dwmmc_hw_s g_dwmmc_hw[] =
{
  {
    .base = RK3576_DWMMC_SD_BASE,
    .gate_con = DWMMC_GATE_CON, .gate_cclk = DWMMC_GATE_CCLK,
    .gate_hclk = DWMMC_GATE_HCLK,
    .sel_con = DWMMC_SEL_CON, .sel_mux_shift = DWMMC_SEL_MUX_SHIFT,
    .sel_div_shift = DWMMC_SEL_DIV_SHIFT, .srst_h = DWMMC_SRST_H,
    .pin_func = PIN_FUNC,
    .bus_bank = PIN_BUS_BANK, .bus_first = PIN_BUS_FIRST,
    .cmd_bank = PIN_CMD_BANK, .cmd_pin = PIN_CMD,
    .clk_bank = PIN_CLK_BANK, .clk_pin = PIN_CLK,
    .det_bank = PIN_DET_BANK, .det_pin = PIN_DET,
    .pwr_bank = PIN_PWR_BANK, .pwr_pin = PIN_PWR,
    .is_sdio = false,
  },
  {
    .base = RK3576_DWMMC_SDIO_BASE,
    .gate_con = SDIO_GATE_CON, .gate_cclk = SDIO_GATE_CCLK,
    .gate_hclk = SDIO_GATE_HCLK,
    .sel_con = SDIO_SEL_CON, .sel_mux_shift = SDIO_SEL_MUX_SHIFT,
    .sel_div_shift = SDIO_SEL_DIV_SHIFT, .srst_h = SDIO_SRST_H,
    .pin_func = SDIO_PIN_FUNC,
    .bus_bank = SDIO_PIN_BANK, .bus_first = SDIO_PIN_BUS_FIRST,
    .cmd_bank = SDIO_PIN_BANK, .cmd_pin = SDIO_PIN_CMD,
    .clk_bank = SDIO_PIN_BANK, .clk_pin = SDIO_PIN_CLK,
    .det_bank = -1, .det_pin = -1,     /* non-removable */
    .pwr_bank = -1, .pwr_pin = -1,     /* 由 sdio-pwrseq 的 GPIO 管 */
    .is_sdio = true,
  },
};

static FAR const struct dwmmc_hw_s *dwmmc_hw(uint32_t base)
{
  size_t i;

  for (i = 0; i < sizeof(g_dwmmc_hw) / sizeof(g_dwmmc_hw[0]); i++)
    {
      if (g_dwmmc_hw[i].base == base)
        {
          return &g_dwmmc_hw[i];
        }
    }

  return NULL;
}

#define RESET_TIMEOUT_US    500000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* ★ 基址必须按实例取，不能写死。
 *
 *   板上有两个 dw-mshc：0x2a310000 是 TF 卡，0x2a320000 是 WiFi（SDIO）。
 *   两者**同时在用**，所以也不能用一个文件级的"当前基址"变量去切 ——
 *   那种写法在单实例时看不出问题，等第二个实例接进来才会以数据错乱的
 *   形式暴露，而且错得很隐蔽。
 */

static inline uint32_t dw_getreg(uint32_t base, uint32_t off)
{
  return getreg32(base + off);
}

static inline void dw_putreg(uint32_t base, uint32_t off, uint32_t val)
{
  putreg32(val, base + off);
}

static int dw_update_clk(uint32_t base);

/****************************************************************************
 * Name: dw_reset_ctrl
 *
 * Description:
 *   控制器软复位。CTRL 的三个复位位由硬件在完成后自动清零，
 *   清不掉就说明时钟没到位 —— 这与 GMAC 的 SWR 是同一类判据。
 *
 ****************************************************************************/

static int dw_reset_ctrl(uint32_t base)
{
  uint32_t val;
  int us;

  dw_putreg(base, DWMMC_CTRL, DWMMC_CTRL_ALL_RESET);

  for (us = 0; us < RESET_TIMEOUT_US; us++)
    {
      val = dw_getreg(base, DWMMC_CTRL);
      if ((val & DWMMC_CTRL_ALL_RESET) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR,
         "ERROR: DWMMC 软复位超时 CTRL=0x%08" PRIx32
         " —— 复位位不自清，通常是 CCLK 没供上\n", val);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dwmmc_probe
 *
 * Description:
 *   SD 卡控制器前置链路：电源域 + 两路时钟 + 复位 + 引脚复用，
 *   然后取三个互相独立的读数作为判据。
 *
 ****************************************************************************/

int rk3576_dwmmc_probe(uint32_t base)
{
  FAR const struct dwmmc_hw_s *hw = dwmmc_hw(base);
  uint32_t verid;
  uint32_t hcon;
  uint32_t cdetect;
  uint32_t fifo_depth;
  bool     sdio_ready = false;
  int      ret;
  int      i;

  if (hw == NULL)
    {
      syslog(LOG_ERR, "ERROR: 0x%08" PRIx32 " 不是已知的 dw-mshc 实例\n",
             base);
      return -EINVAL;
    }

  /* 1) 电源域。两个实例与 GMAC 同属 PD_SDGMAC。 */

  ret = rk3576_power_on(RK3576_PD_SDGMAC);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: PD_SDGMAC 上电失败: %d\n", ret);
      return ret;
    }

  /* 2) 两路时钟。biu(hclk) 供寄存器访问，ciu(cclk) 驱动卡时钟 ——
   *    只开 hclk 的话寄存器读写正常但卡不动，这个坑在 I2C 上踩过。
   *
   *    选源取 2 = xin24m：初始化阶段本来就要 400kHz 以下，用 24M
   *    分频最省事，也避开了 gpll/cpll 实际频率的不确定性。
   *    分频 6 位，写 (div/2 - 1)？——不，dw_mmc 的 CLKDIV 才是卡时钟
   *    分频；这里的 CLKSEL 分频是给控制器源时钟的，取 1 分频。
   */

  rk3576_clk_setmux(hw->sel_con, hw->sel_mux_shift, 2, 2);

  /* ★ 分频位置：SDIO 在 CRU 里分到 400kHz，控制器的 CLKDIV 走旁路。
   *
   *   原厂 Linux 在这个主机上打印的是
   *     mmc0: Bus speed = 400000Hz (req 400000Hz, actual 400000Hz, div = 0)
   *   div=0 说明它把源时钟直接整到了 400kHz，而不是靠控制器内部分频。
   *
   *   两种分法的**频率**等效，但 Rockchip dw_mmc 的采样/驱动相位是按
   *   源时钟整定的，分频放在控制器里会移动采样点 —— 表现正是"卡在拉
   *   命令线但响应帧格式不对"（RESP_ERR，无超时无 CRC 错）。
   *
   *   24MHz / 60 = 400kHz，CRU 分频器写 (n-1)。
   */

  if (hw->is_sdio)
    {
      rk3576_clk_setmux(hw->sel_con, hw->sel_div_shift, 6, 59);
    }
  else
    {
      rk3576_clk_setmux(hw->sel_con, hw->sel_div_shift, 6, 0);
    }
  rk3576_clk_gate(hw->gate_con, hw->gate_cclk, true);
  rk3576_clk_gate(hw->gate_con, hw->gate_hclk, true);

  /* 3) 解除模块复位 */

  rk3576_reset(hw->srst_h, true);
  up_udelay(20);
  rk3576_reset(hw->srst_h, false);
  up_udelay(100);

  /* 4) 引脚复用。全部功能号 1。 */

  for (i = 0; i < 4; i++)
    {
      rk3576_pinmux_set(hw->bus_bank, hw->bus_first + i, hw->pin_func);
    }

  rk3576_pinmux_set(hw->cmd_bank, hw->cmd_pin, hw->pin_func);
  rk3576_pinmux_set(hw->clk_bank, hw->clk_pin, hw->pin_func);

  if (hw->det_pin >= 0)
    {
      rk3576_pinmux_set(hw->det_bank, hw->det_pin, hw->pin_func);
    }

  /* 卡供电。
   *
   * ★ 厂商 dtb 的 sdmmc0-pwren 把 GPIO0_B6 复用成功能 1，也就是控制器
   *   自己的 SDMMC0_PWREN 输出，由 PWREN 寄存器(0x004)驱动 —— 不是当作
   *   普通 GPIO 拉高。两种做法的电平可能相同，但只有前者与控制器的
   *   上电时序联动。这里按厂商的来。
   */

  if (hw->pwr_pin >= 0)
    {
      rk3576_pinmux_set(hw->pwr_bank, hw->pwr_pin, hw->pin_func);
      dw_putreg(base, DWMMC_PWREN, 1);
      up_mdelay(20);
    }

  /* 4b) WiFi 模组上电（仅 SDIO 实例）。
   *
   * ★ /sdio-pwrseq：reset-gpios = GPIO1_22，**低有效**，
   *   post-power-on-delay-ms = 200。
   *
   *   必须在发任何命令之前完成，而且那 200ms 不能省 —— 模组内部要
   *   等晶振起振并把 SDIO 从机准备好。省掉它的表现是 CMD5 无应答，
   *   和"引脚配错"完全一样。
   */

  /* ★ 只把模组按在复位里，**释放要等时钟起来之后**。
   *
   *   Linux 的顺序是 mmc_power_up（先供电、先起总线时钟）-> 再执行
   *   pwrseq 的 post_power_on（释放复位）-> 才发命令。也就是说模组是
   *   **在有时钟的情况下**出复位的。
   *
   *   我们原来反了：无时钟时就释放复位，等 200ms 才开时钟。SDIO 卡在
   *   复位释放的那一刻会按引脚状态锁存工作配置，没有时钟就可能锁进错的
   *   状态 —— 之后它照样会拉命令线应答，但帧的形状不对，正是我们看到的
   *   "RESP_ERR 置位、无超时、无 CRC 错、RESP0 全 0"。
   */

  if (hw->is_sdio)
    {
      rk3576_pinmux_set(WIFI_RST_BANK, WIFI_RST_PIN, 0);   /* 功能 0 = GPIO */
      rk3576_pinmux_set(BT_RST_BANK, BT_RST_PIN, 0);
      rk3576_gpio_setdir(BT_RST_BANK, BT_RST_PIN, true);
      rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, false);   /* 先拉低 */
      rk3576_gpio_setdir(WIFI_RST_BANK, WIFI_RST_PIN, true);
      rk3576_gpio_write(WIFI_RST_BANK, WIFI_RST_PIN, false);  /* 保持复位 */
      up_mdelay(20);
    }

  /* 5) 判据一：版本与硬件配置寄存器。
   *
   *    VERID 是只读的 IP 版本号，读回全 0 或全 f 说明基址错、时钟没开
   *    或电源域没上电 —— 三者之一，但至少能立刻排除"代码逻辑问题"。
   */

  verid = dw_getreg(base, DWMMC_VERID);
  hcon  = dw_getreg(base, DWMMC_HCON);

  if (verid == 0 || verid == 0xffffffff)
    {
      syslog(LOG_ERR,
             "ERROR: DWMMC VERID=0x%08" PRIx32 " —— 寄存器块无响应\n",
             verid);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "DWMMC: VERID=0x%08" PRIx32 " HCON=0x%08" PRIx32
         " 数据总线宽度=%" PRIu32 " 位\n",
         verid, hcon,
         DWMMC_GET_HDATA_WIDTH(hcon) == 0 ? 16 :
         DWMMC_GET_HDATA_WIDTH(hcon) == 1 ? 32 : 64);

  /* 6) 软复位。复位位自清是"时钟真的在跑"的证明，比读寄存器更强。 */

  ret = dw_reset_ctrl(base);
  if (ret < 0)
    {
      return ret;
    }

  dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);   /* 清残留状态 */
  dw_putreg(base, DWMMC_INTMASK, 0);               /* 轮询式，先不开中断 */
  dw_putreg(base, DWMMC_TMOUT, 0xffffffff);

  /* 判据二：FIFO 深度。dtb 说 256，从 FIFOTH 复位值反推一次，
   * 对得上就说明读到的确实是这个控制器，而不是别的地址。
   */

  fifo_depth = ((dw_getreg(base, DWMMC_FIFOTH) >> 16) & 0xfff) + 1;

  /* 判据三：卡在位检测。CDETECT 位 0 低有效。
   *
   * 这一条能把"控制器没配好"和"卡没插"分开 —— 两者的后续现象
   * （命令超时）完全一样，事后无法区分。
   */

  cdetect = dw_getreg(base, DWMMC_CDETECT);

  /* ★ 不可插拔的实例（SDIO/WiFi）没有插卡检测线，CDETECT 恒读"没卡"。
   *   对它套用这一判据会在一切正常时也直接退出。
   */

  if (hw->det_pin >= 0)
    {
      syslog(LOG_INFO,
             "DWMMC: FIFO 深度≈%" PRIu32 "（dtb 记载 256）"
             " CDETECT=0x%08" PRIx32 " —— %s\n",
             fifo_depth, cdetect,
             DWMMC_CDETECT_PRESENT(cdetect) ? "检测到卡在位" : "未检测到卡");

      if (!DWMMC_CDETECT_PRESENT(cdetect))
        {
          return -ENODEV;
        }
    }
  else
    {
      syslog(LOG_INFO,
             "DWMMC: FIFO 深度≈%" PRIu32 " 不可插拔实例，跳过插卡检测\n",
             fifo_depth);
    }

  /* 7) 判据四：手工发 CMD0 + CMD8，直接看命令通路。
   *
   *    ★ 为什么单独做这一步：交给 mmcsd 之后，失败只会得到一句
   *      "卡未被识别" —— 它内部会依次试 SD、SDIO、MMC 的多种命令，
   *      不认得的必然超时，日志里分不出哪条是真失败。
   *
   *      CMD8 (SEND_IF_COND, arg=0x1aa) 是最干净的一条：任何 SD 2.0
   *      及以上的卡都必须原样回显 0x1aa。回显对上 -> 时钟、命令线、
   *      响应通路全部正常；超时 -> 卡没被时钟驱动或命令线不通；
   *      回显不对 -> 采样时序有问题。三种结果互不相同。
   */

  {
    uint32_t sts;
    uint32_t resp;
    int      us;

    /* ★ SDIO 侧不走 HOLD 寄存器。
     *
     *   USE_HOLD_REG 决定命令/响应是否经过 HOLD 寄存器再采样。它影响的
     *   正是采样时刻 —— 而我们的症状（RESP_ERR 置位、无超时、无 CRC 错、
     *   RESP0 全 0）恰恰是"帧收到了但采错了位"的样子。SD 卡在 400kHz
     *   下对此不敏感，SDIO 卡（SDR104）可能敏感。
     */

    uint32_t holdbit = hw->is_sdio ? 0 : DWMMC_CMD_USE_HOLD_REG;

    /* 先给足初始化时钟：400kHz 下 80 个时钟约 200us。 */

    /* SDIO 侧源时钟已在 CRU 分到 400kHz，控制器内部走旁路（与原厂一致）；
     * SD 侧维持原来的做法（已验证可用，不动它）。
     */

    /* ★ 严格按原厂 dw_mmc.c 的顺序设时钟，每一步都要"通知 CIU"。
     *
     *   出处：kernel-6.1/drivers/mmc/host/dw_mmc.c 的 dw_mci_setup_bus()
     *     CLKENA = 0        -> 通知 CIU     （先停，避免改分频时出毛刺）
     *     CLKSRC = 0        -> 通知 CIU     （选分频器 0）
     *     CLKDIV = div      -> 通知 CIU
     *     CLKENA = ENABLE   -> 通知 CIU
     *
     *   我们原来少了前两步。CLKSRC 若残留非零值，控制器会去用另一个
     *   未配置的分频器；而"通知 CIU"是 DW MMC 的硬性要求 —— 不发这条
     *   命令，写进 CLKDIV/CLKENA 的值不会真正生效。
     *
     *   ★ CLKENA 只置 bit0(ENABLE)，**绝不能置 bit16(LOW_PWR)**：那会让
     *     控制器在总线空闲时停掉时钟，而 SDIO 卡靠持续时钟应答与报中断。
     *     原厂对 SDIO 也是特意不置这一位的。
     */

    dw_putreg(base, DWMMC_CLKENA, 0);
    dw_update_clk(base);
    dw_putreg(base, DWMMC_CLKSRC, 0);
    dw_update_clk(base);
    dw_putreg(base, DWMMC_CLKDIV, hw->is_sdio ? 0 : 30);
    dw_update_clk(base);
    dw_putreg(base, DWMMC_CLKENA, DWMMC_CLKENA_ENABLE);
    dw_update_clk(base);
    up_mdelay(2);

    /* 时钟已经在跑，现在才放模组出复位（见上面的说明） */

    if (hw->is_sdio)
      {
        rk3576_gpio_write(WIFI_RST_BANK, WIFI_RST_PIN, true);
        rk3576_gpio_write(BT_RST_BANK, BT_RST_PIN, true);
        up_mdelay(WIFI_PWRON_DELAY_MS);
        syslog(LOG_INFO,
               "DWMMC: 模组释放复位 WIFI_REG_ON(GPIO%d_%d)=1 "
               "BT_REG_ON(GPIO%d_%d)=1，等 %d ms\n",
               WIFI_RST_BANK, WIFI_RST_PIN, BT_RST_BANK, BT_RST_PIN,
               WIFI_PWRON_DELAY_MS);
      }

    /* CMD0 GO_IDLE_STATE，无响应，带初始化序列 */

    dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);
    dw_putreg(base, DWMMC_CMDARG, 0);
    dw_putreg(base, DWMMC_CMD, DWMMC_CMD_START | holdbit |
                         DWMMC_CMD_INIT | DWMMC_CMD_INDX(0));

    for (us = 0; us < 200000; us++)
      {
        if ((dw_getreg(base, DWMMC_CMD) & DWMMC_CMD_START) == 0)
          {
            break;
          }

        up_udelay(1);
      }

    for (us = 0; us < 200000; us++)
      {
        if (dw_getreg(base, DWMMC_RINTSTS) & DWMMC_INT_CMD_DONE)
          {
            break;
          }

        up_udelay(1);
      }

    syslog(LOG_INFO, "DWMMC: CMD0 后 RINTSTS=0x%08" PRIx32 "\n",
           dw_getreg(base, DWMMC_RINTSTS));

    up_mdelay(2);

    /* CMD8 SEND_IF_COND，参数 0x1aa（2.7-3.6V + 校验图案 0xaa），
     * 短响应带 CRC。
     */

    dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);

    /* ★ SD 用 CMD8，SDIO 用 CMD5。
     *
     *   SDIO 卡不认 CMD8，对它发 CMD8 必然超时 —— 而超时同时也是
     *   "线没通"的表现，两者分不开，判据就失效了。CMD5
     *   (IO_SEND_OP_COND, arg=0) 才是 SDIO 的对应命令：SDIO 设备回
     *   OCR（R4 短响应，**无 CRC**），非 SDIO 设备完全不应答。
     */

    if (hw->is_sdio)
      {
        /* SDIO enumeration repeats CMD5 until the card reports ready.
         *
         *   第一次 arg=0 是询问，卡回它支持的 OCR 电压窗口；之后带着
         *   OCR 重发，直到卡把 bit31(C, 上电完成) 置起来。发一次就判定
         *   等于要求卡在第一条命令上就完成上电，多数卡做不到。
         *
         * R4 has no valid CRC or normal command-index field. Disable response
         * CRC checking; an RE bit still means the response is invalid.
         */

        int try;
        uint32_t ocr = 0;

        for (try = 0; try < 20; try++)
          {
            dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);
            dw_putreg(base, DWMMC_CMDARG, ocr);
            dw_putreg(base, DWMMC_CMD,
                      DWMMC_CMD_START | holdbit |
                      DWMMC_CMD_RESP_EXP | DWMMC_CMD_INDX(5));

            /* ★ 先看控制器有没有**接受**这条命令。
             *
             *   CMD 寄存器的 START 位是自清的：控制器把命令收进命令队列
             *   后才清零。若它一直不清，说明控制器压根没受理 —— 那么
             *   RINTSTS 里的任何位都不是这条命令的结果，据此判断响应
             *   就完全跑偏了。
             *
             *   SD 卡成功时 RINTSTS=0x04(CMD_DONE)，而 SDIO 这边是
             *   0x02(RESP_ERR) 且**没有 CMD_DONE** —— 正是这个反常促使
             *   加上这一项。
             */

            {
              int started = 0;

              for (us = 0; us < 200000; us++)
                {
                  if ((dw_getreg(base, DWMMC_CMD) & DWMMC_CMD_START) == 0)
                    {
                      started = 1;
                      break;
                    }

                  up_udelay(1);
                }

              if (!started && try == 0)
                {
                  syslog(LOG_ERR,
                         "DWMMC: CMD5 的 START 位不自清 —— 控制器未受理命令，"
                         "STATUS=0x%08" PRIx32 " CTRL=0x%08" PRIx32 "\n",
                         dw_getreg(base, DWMMC_STATUS),
                         dw_getreg(base, DWMMC_CTRL));
                }
            }

            for (us = 0; us < 200000; us++)
              {
                sts = dw_getreg(base, DWMMC_RINTSTS);
                if (sts & (DWMMC_INT_CMD_DONE | DWMMC_INT_CMD_ERROR))
                  {
                    break;
                  }

                up_udelay(1);
              }

            resp = dw_getreg(base, DWMMC_RESP0);

            if (try < 3 || (resp & 0x80000000) != 0)
              {
                syslog(LOG_INFO,
                       "DWMMC: CMD5 第%d次 RINTSTS=0x%08" PRIx32
                       " RESP0=0x%08" PRIx32 "\n", try, sts, resp);
              }

            if (sts & DWMMC_INT_CMD_ERROR)
              {
                syslog(LOG_ERR,
                       "DWMMC: CMD5 response invalid RINTSTS=0x%08" PRIx32
                       " RESP0=0x%08" PRIx32 "\n", sts, resp);
                break;
              }

            if ((sts & DWMMC_INT_CMD_DONE) == 0 ||
                ((resp >> 28) & 0x7) == 0 ||
                (resp & 0x00ffffff) == 0)
              {
                syslog(LOG_ERR,
                       "DWMMC: CMD5 invalid R4 RINTSTS=0x%08" PRIx32
                       " RESP0=0x%08" PRIx32 "\n", sts, resp);
                break;
              }

            if (resp & 0x80000000)
              {
                sdio_ready = true;
                break;                 /* C 位置起，上电完成 */
              }

            ocr = resp & 0x00ffffff;

            up_mdelay(10);
          }

        /* CCCR CMD52 access follows RCA assignment and card selection in
         * sdio_probe(); it is not valid as a host-only pre-selection test.
         */

        dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);
        return sdio_ready ? OK : -EIO;
      }
    else
      {
        dw_putreg(base, DWMMC_CMDARG, 0x1aa);
        dw_putreg(base, DWMMC_CMD, DWMMC_CMD_START | DWMMC_CMD_USE_HOLD_REG |
                             DWMMC_CMD_RESP_EXP | DWMMC_CMD_RESP_CRC |
                             DWMMC_CMD_INDX(8));
      }

    for (us = 0; us < 200000; us++)
      {
        if ((dw_getreg(base, DWMMC_CMD) & DWMMC_CMD_START) == 0)
          {
            break;
          }

        up_udelay(1);
      }

    sts = 0;
    for (us = 0; us < 500000; us++)
      {
        sts = dw_getreg(base, DWMMC_RINTSTS);
        if (sts & (DWMMC_INT_CMD_DONE | DWMMC_INT_CMD_ERROR))
          {
            break;
          }

        up_udelay(1);
      }

    resp = dw_getreg(base, DWMMC_RESP0);

    if (hw->is_sdio)
      {
        /* R4 的 bit31 是 C（初始化完成），bit[30:28] 是功能单元个数，
         * bit[23:0] 是 OCR 电压窗口。功能单元 >0 就证明对面是 SDIO。
         */

        syslog(LOG_INFO,
               "DWMMC: CMD5 RINTSTS=0x%08" PRIx32 " RESP0=0x%08" PRIx32
               " 功能单元=%" PRIu32 " OCR=0x%06" PRIx32 " —— %s\n",
               sts, resp, (resp >> 28) & 0x7, resp & 0xffffff,
               (sts & DWMMC_INT_RTO)      ? "无应答：模组未上电、时钟或命令线不通" :
               (((resp >> 28) & 0x7) != 0) ? "检测到 SDIO 设备" :
                                             "有响应但功能单元为 0，不像 SDIO");
      }
    else
      {
        syslog(LOG_INFO,
               "DWMMC: CMD8 RINTSTS=0x%08" PRIx32 " RESP0=0x%08" PRIx32
               " —— %s\n",
               sts, resp,
               (sts & DWMMC_INT_RTO)      ? "响应超时：卡没被时钟驱动或命令线不通" :
               (sts & DWMMC_INT_RCRC)     ? "CRC 错：采样时序有问题" :
               ((resp & 0xfff) == 0x1aa)  ? "回显正确，命令通路正常" :
                                            "回显不符，采样时序有问题");
      }

    dw_putreg(base, DWMMC_RINTSTS, DWMMC_INT_ALL);
  }

  return OK;
}


/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_dwmmc_dev_s
{
  struct sdio_dev_s dev;              /* 必须是第一个成员 */
  uint32_t          base;             /* 控制器基址（TF 卡 / WiFi 各一份） */
  uint8_t          *buffer;           /* 当前事务的数据缓冲区 */
  size_t            remaining;        /* 待传字节数           */
  bool              is_write;
  uint16_t          blocksize;
  uint16_t          nblocks;
  sdio_eventset_t   waitevents;
  uint32_t          waittimeout_ms;
  bool              widebus;
  uint32_t          fifo_off;         /* FIFO 数据窗口偏移     */
  uint32_t          card_hz;          /* 实际卡时钟            */

  /* ★ 上一条命令的结果。recv_r* 必须如实返回它。
   *
   *   mmcsd 判定卡类型时取的是 SDIO_RECVR3 的返回值，而不是
   *   sendcmd/waitresponse 的：
   *     mmcsd_sendcmdpoll(priv, MMC_CMD1, ...);
   *     ret = SDIO_RECVR3(priv->dev, MMC_CMD1, &response);
   *     if (ret != OK) { 不是 MMC，继续试 CMD8 }
   *
   *   若 recv_r* 无条件返回 OK，CMD1 超时也会被当成成功 ——
   *   mmcsd 就认定这是张 MMC 卡，从此走 MMC 分支，CMD8 永远发不出去。
   *   现象是"一直在发 CMD1"，与根因隔着一整个分支判断。
   */

  int               last_result;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_dwmmc_dev_s g_dwmmc;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: dw_update_clk
 *
 * Description:
 *   把 CLKDIV/CLKENA 的改动送进卡时钟域。
 *
 *   ★ dw_mmc 与 SDHCI 在这里有本质区别：CLKDIV 和 CLKENA 写完**不会
 *     立即生效**，必须发一条带 UPD_CLK 位的空命令，由控制器在内部同步。
 *     漏掉这一步的表现是时钟始终停在上一个值，而寄存器读回却是新值 ——
 *     读回一致会让人误以为已经生效。
 *
 ****************************************************************************/

static int dw_update_clk(uint32_t base)
{
  int us;

  dw_putreg(base, DWMMC_CMD, DWMMC_CMD_START | DWMMC_CMD_UPD_CLK |
                       DWMMC_CMD_PRV_DAT_WAIT);

  for (us = 0; us < 100000; us++)
    {
      if ((dw_getreg(base, DWMMC_CMD) & DWMMC_CMD_START) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "ERROR: DWMMC 时钟更新超时 CMD=0x%08" PRIx32 "\n",
         dw_getreg(base, DWMMC_CMD));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: dw_set_clock_hz
 ****************************************************************************/

static void dw_set_clock_hz(struct rk3576_dwmmc_dev_s *priv, uint32_t hz)
{
  uint32_t cclkin;
  uint32_t div;
  uint32_t parent;
  uint32_t mux;

  /* 先停时钟再改 —— 运行中改分频会产生毛刺。 */

  dw_putreg(priv->base, DWMMC_CLKENA, 0);
  dw_update_clk(priv->base);

  if (hz == 0)
    {
      priv->card_hz = 0;
      return;
    }

  /* ★ Rockchip 的 dw_mmc 不是靠 CLKDIV 调卡时钟的，而是靠调 CIU 源时钟。
   *
   *   厂商驱动 dw_mmc-rockchip.c 的 dw_mci_rk3288_set_ios() 注释：
   *
   *     bus_hz = cclkin / RK3288_CLKGEN_DIV        // CLKGEN 固定 2 分频
   *     ios->clock = (div == 0) ? bus_hz : bus_hz / (2 * div)
   *     Note: div can only be 0 or 1
   *     cclkin = ios->clock * RK3288_CLKGEN_DIV    // clk_set_rate 调源时钟
   *
   *   也就是说控制器内部先固定 2 分频，CLKDIV 只允许 0 或 1。
   *
   *   此前本端口把源时钟固定在 24MHz、用 CLKDIV 做大分频（400kHz 算出
   *   div=30）—— 那是**超出规格的取值**。控制器由此进入未定义行为，
   *   表现为对代码布局极度敏感：命令路径上多一条 syslog 就能工作，
   *   换成等效延时、编译屏障或让出 CPU 都不行。这类"看起来像时序"的
   *   现象，根子往往是某个字段被写了非法值。
   *
   *   现在改为：CLKDIV 固定 0，用 CRU 调 CCLK_SRC_SDMMC0 到 hz*2。
   *
   *     COMPOSITE(CCLK_SRC_SDMMC0, gpll_cpll_24m_p,
   *               CLKSEL_CON(105), 13, 2, MFLAGS,   选源
   *                                 7, 6, DFLAGS)   6 位分频
   */

  /* ★ 这里不乘 CLKGEN_DIV。
   *
   *   厂商 RK3288 的驱动里 cclkin = ios->clock * RK3288_CLKGEN_DIV，
   *   前提是控制器内部确有那一级 2 分频。RK3576 是否相同**未经证实** ——
   *   若没有，乘 2 会让识别阶段的卡时钟变成 800kHz，超过规范的 400kHz
   *   上限，表现为部分命令响应超时（实测 CMD55 RTO）。
   *
   *   按 hz 直接设源时钟，两种情况下都合法：
   *     有 CLKGEN/2 -> 实际 hz/2，偏慢但合规
   *     无 CLKGEN/2 -> 实际 hz，正好
   *   先保证能工作，分频器是否存在等能通信后再用实测频率确认。
   */

  cclkin = hz;

  /* 选源：低速用 24M 分频，高速用 gpll。分频器只有 6 位（1..64），
   * 所以源频率不能离目标太远。
   */

  if (cclkin <= 24000000)
    {
      parent = 24000000;
      mux    = DWMMC_SEL_MUX_24M;
    }
  else
    {
      parent = DWMMC_GPLL_HZ;
      mux    = DWMMC_SEL_MUX_GPLL;
    }

  div = (parent + cclkin - 1) / cclkin;
  if (div < 1)
    {
      div = 1;
    }
  else if (div > 64)
    {
      div = 64;
    }

  rk3576_clk_setmux(DWMMC_SEL_CON, DWMMC_SEL_MUX_SHIFT, 2, mux);
  rk3576_clk_setmux(DWMMC_SEL_CON, DWMMC_SEL_DIV_SHIFT, 6, div - 1);

  /* CLKDIV 固定 0 = 直通（规格只允许 0 或 1） */

  dw_putreg(priv->base, DWMMC_CLKDIV, 0);
  dw_update_clk(priv->base);

  dw_putreg(priv->base, DWMMC_CLKENA, DWMMC_CLKENA_ENABLE);
  dw_update_clk(priv->base);

  priv->card_hz = parent / div;

  syslog(LOG_INFO,
         "DWMMC: 卡时钟 目标 %" PRIu32 "Hz 源=%" PRIu32 "Hz/%" PRIu32
         " = %" PRIu32 "Hz（若含 CLKGEN/2 则再半）\n",
         hz, parent, div, priv->card_hz);
}

/****************************************************************************
 * Name: dw_fifo_reset
 ****************************************************************************/

static void dw_fifo_reset(uint32_t base)
{
  uint32_t val = dw_getreg(base, DWMMC_CTRL);
  int us;

  dw_putreg(base, DWMMC_CTRL, val | DWMMC_CTRL_FIFO_RESET);

  for (us = 0; us < 100000; us++)
    {
      if ((dw_getreg(base, DWMMC_CTRL) & DWMMC_CTRL_FIFO_RESET) == 0)
        {
          return;
        }

      up_udelay(1);
    }

  syslog(LOG_WARNING, "DWMMC: FIFO 复位未自清\n");
}

/****************************************************************************
 * Name: dw_wait_while_busy
 *
 * Description:
 *   发带数据的命令之前，先用软件等卡的忙信号清掉。
 *
 *   ★ 数据类命令都会带 PRV_DAT_WAIT，让硬件自己等上一次数据结束。但
 *     Databook 要求软件**另外**再确认一次卡不忙 —— 否则命令会悬在
 *     控制器里不被受理（CMD 的 START 位一直不清），表现为"命令未被
 *     受理"，而不是超时或错误。ACMD51（读 SCR，第一条数据命令）正是
 *     这样卡住的。
 *
 *     出处 dw_mci_wait_while_busy()：超时 500ms，仍忙就打日志硬发 ——
 *     照抄这个行为，宁可发出去看错误码，也不要静默悬住。
 *
 ****************************************************************************/

static void dw_wait_while_busy(uint32_t base)
{
  int us;

  /* ★ 两个"忙"要分清：
   *     bit9  data_busy    —— 卡把 DAT0 拉低表示自己忙
   *     bit10 mc_busy      —— 控制器的数据状态机在忙
   *   参考驱动只等前者，但后者卡住时命令同样发不出去（本端口就撞上过：
   *   一条不该带数据的命令被加了 DAT_EXP，数据状态机就此悬住）。
   *   两个都等，才不会把"控制器卡住"误判成"卡忙"。
   */

  for (us = 0; us < 500000; us++)
    {
      uint32_t sts = dw_getreg(base, DWMMC_STATUS);

      if ((sts & (DWMMC_STATUS_BUSY | DWMMC_STATUS_MC_BUSY)) == 0)
        {
          return;
        }

      up_udelay(1);
    }

  syslog(LOG_WARNING,
         "DWMMC: 卡持续忙 STATUS=0x%08" PRIx32 "，仍然发出命令\n",
         dw_getreg(base, DWMMC_STATUS));
}

/****************************************************************************
 * SDIO 接口实现
 ****************************************************************************/

static void rk3576_dwmmc_reset(struct sdio_dev_s *dev)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

#ifdef CONFIG_RK3576_DWMMC_TRACE
  syslog(LOG_INFO, "DW reset()\n");
#endif

  dw_reset_ctrl(priv->base);
  dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_ALL);
  dw_putreg(priv->base, DWMMC_INTMASK, 0);
  dw_putreg(priv->base, DWMMC_TMOUT, 0xffffffff);
  dw_putreg(priv->base, DWMMC_CTYPE, DWMMC_CTYPE_1BIT);

  priv->buffer     = NULL;
  priv->remaining  = 0;
  priv->widebus    = false;
  priv->waitevents = 0;
}

static sdio_capset_t rk3576_dwmmc_capabilities(struct sdio_dev_s *dev)
{
  UNUSED(dev);

  /* ★ 必须报 4BIT_ONLY。
   *
   *   NuttX 的 mmcsd 层在这里不对称：CMD6 切卡的总线宽度要求
   *   priv->buswidth 带 4BIT 标志，而该标志只在 caps 含
   *   SDIO_CAPS_4BIT_ONLY 时才置位；但 SDIO_WIDEBUS(true) 只看
   *   IS_MMC()。报 0 的话会出现"主机 4 位、卡 1 位"的错配 ——
   *   这个坑在 eMMC 那边踩过一次。
   */

  /* ★ SDIO_CAPS_DMABEFOREWRITE 决定的是**调用顺序**，与用不用 DMA 无关。
   *
   *   NuttX 写单块的默认顺序是：先发 CMD24，再 BLOCKSETUP/SENDSETUP。
   *   DW 控制器要求发命令之前 BLKSIZ/BYTCNT 已经写好 —— 按默认顺序，
   *   sendcmd 执行时缓冲区还没挂上，长度寄存器是 0，命令就不带数据阶段，
   *   随后数据永远传不出去（现象是等 DATA_OVER 超时，而主机侧已传
   *   字节数为 0）。
   *
   *   报上这个标志后，mmcsd 改成先 setup 再发命令，正合 DW 的要求。
   */

  return SDIO_CAPS_4BIT_ONLY | SDIO_CAPS_DMABEFOREWRITE;
}

static sdio_statset_t rk3576_dwmmc_status(struct sdio_dev_s *dev)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  UNUSED(dev);

  /* 用硬件的卡检测脚，而不是恒返回 PRESENT。SD 卡是可插拔的，
   * 这里说实话才有意义。
   */

  return DWMMC_CDETECT_PRESENT(dw_getreg(priv->base, DWMMC_CDETECT)) ?
         SDIO_STATUS_PRESENT : 0;
}

static void rk3576_dwmmc_widebus(struct sdio_dev_s *dev, bool wide)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  priv->widebus = wide;
  dw_putreg(priv->base, DWMMC_CTYPE, wide ? DWMMC_CTYPE_4BIT : DWMMC_CTYPE_1BIT);
}

static void rk3576_dwmmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  switch (rate)
    {
      case CLOCK_SDIO_DISABLED:
        dw_set_clock_hz(priv, 0);
        break;

      case CLOCK_IDMODE:
        dw_set_clock_hz(priv, 400000);
        break;

      case CLOCK_MMC_TRANSFER:
      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
      default:
        dw_set_clock_hz(priv, 25000000);
        break;
    }
}

static int rk3576_dwmmc_attach(struct sdio_dev_s *dev)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  UNUSED(dev);

  /* 轮询式实现，不接中断。中断版本待后续补。 */

  dw_putreg(priv->base, DWMMC_INTMASK, 0);
  return OK;
}

static int rk3576_dwmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t arg)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;
  uint32_t regval = DWMMC_CMD_START | DWMMC_CMD_USE_HOLD_REG;
  uint32_t datalen;
  int us;

  regval |= DWMMC_CMD_INDX(cmd & MMCSD_CMDIDX_MASK);

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        break;

      case MMCSD_R2_RESPONSE:
        regval |= DWMMC_CMD_RESP_EXP | DWMMC_CMD_RESP_LONG |
                  DWMMC_CMD_RESP_CRC;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        regval |= DWMMC_CMD_RESP_EXP;    /* 这两种响应没有 CRC */
        break;

      default:
        regval |= DWMMC_CMD_RESP_EXP | DWMMC_CMD_RESP_CRC;
        break;
    }

  /* ★ 是否带数据阶段，必须看**命令字自己的标志**，不能看缓冲区挂没挂。
   *
   *   mmcsd 读 SCR 的顺序是：
   *     SDIO_BLOCKSETUP -> SDIO_RECVSETUP -> CMD55(APP_CMD) -> ACMD51
   *   缓冲区在 CMD55 之前就挂上了，而数据属于 ACMD51。若按
   *   "buffer != NULL" 判断，CMD55 也会被加上 DAT_EXP 并写入
   *   BLKSIZ/BYTCNT —— 控制器于是等一个永远不会到来的数据阶段，
   *   数据状态机就此卡住（STATUS 的 MC_BUSY 置位且不再清），随后
   *   真正带数据的 ACMD51 一直无法被受理。
   *
   *   现象是 "CMD51 未被受理"，而根因在上一条命令，中间隔了一步。
   */

  if ((cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR &&
      priv->buffer != NULL && priv->remaining > 0)
    {
      regval |= DWMMC_CMD_DAT_EXP;
      if ((cmd & MMCSD_WRXFR) != 0)
        {
          regval |= DWMMC_CMD_DAT_WR;
        }

      datalen = priv->remaining;
      dw_putreg(priv->base, DWMMC_BLKSIZ, priv->blocksize);
      dw_putreg(priv->base, DWMMC_BYTCNT, datalen);
    }
  else
    {
      /* 不带数据的命令要把长度清零，否则控制器会沿用上一条的值。 */

      dw_putreg(priv->base, DWMMC_BLKSIZ, 0);
      dw_putreg(priv->base, DWMMC_BYTCNT, 0);
    }

  /* CMD0 需要带初始化序列（80 个时钟） */

  if ((cmd & MMCSD_CMDIDX_MASK) == 0)
    {
      regval |= DWMMC_CMD_INIT;
    }

  /* ★ PRV_DAT_WAIT 要对**每一条**命令都设，不只是带数据的。
   *
   *   出处是厂商 SDK 里 U-Boot 的轮询式实现（与本驱动同构）
   *   u-boot/drivers/mmc/dw_mmc.c dwmci_send_cmd()：
   *
   *     if (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION)
   *             flags |= DWMCI_CMD_ABORT_STOP;
   *     else
   *             flags |= DWMCI_CMD_PRV_DAT_WAIT;
   *
   *   不设它时，命令可能在控制器尚未收尾时发出而丢失 —— 表现为
   *   waitresponse 等不到 CMD_DONE，且现象随代码布局漂移。
   */

  if ((cmd & MMCSD_CMDIDX_MASK) == 12)
    {
      regval |= DWMMC_CMD_STOP;
    }
  else
    {
      regval |= DWMMC_CMD_PRV_DAT_WAIT;
    }

  /* 每条命令开始时先置为未完成，避免沿用上一条的结果。 */

  priv->last_result = -EBUSY;

  /* 顺序与 U-Boot 一致：先等卡不忙，再清中断状态，最后写参数与命令。 */

  dw_wait_while_busy(priv->base);
  dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_ALL);
  dw_putreg(priv->base, DWMMC_CMDARG, arg);
  dw_putreg(priv->base, DWMMC_CMD, regval);

  /* 等待命令被控制器接受（START 位自清）。这一步不等于命令完成，
   * 只是控制器收下了 —— 两者分开，超时时能说清卡在哪一步。
   */

  for (us = 0; us < 100000; us++)
    {
      if ((dw_getreg(priv->base, DWMMC_CMD) & DWMMC_CMD_START) == 0)
        {
#ifdef CONFIG_RK3576_DWMMC_TRACE
          syslog(LOG_INFO, "DW >CMD%" PRIu32 " 已受理\n",
                 cmd & MMCSD_CMDIDX_MASK);
#endif
          return OK;
        }

      up_udelay(1);
    }

  /* ★ 这条出口此前没有 trace，正是盲区所在：命令在这里失败时，
   * mmcsd_sendcmdpoll() 直接返回错误、不会调 waitresponse，
   * 于是整次尝试在日志里完全不可见。而 mmcsd 会据此改走别的分支
   * （例如 CMD8 失败就认定不是 SD 2.0 卡，转去试 MMC 的 CMD1）——
   * 最终现象是"一直在发 CMD1"，与真正的失败点隔了好几步。
   */

  syslog(LOG_ERR,
         "DW >CMD%" PRIu32 " 未被受理 CMD=0x%08" PRIx32
         " STATUS=0x%08" PRIx32 " RINTSTS=0x%08" PRIx32 "\n",
         cmd & MMCSD_CMDIDX_MASK, dw_getreg(priv->base, DWMMC_CMD),
         dw_getreg(priv->base, DWMMC_STATUS), dw_getreg(priv->base, DWMMC_RINTSTS));

  /* 命令根本没发出去，同样要记 —— 否则 recv_* 会返回上一条命令
   * 遗留的 OK，把"没发成功"报成"成功"。
   */

  priv->last_result = -ETIMEDOUT;
  return priv->last_result;
}

static int rk3576_dwmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;
  uint32_t sts;
  int us;

  for (us = 0; us < 500000; us++)
    {
      sts = dw_getreg(priv->base, DWMMC_RINTSTS);

      if (sts & DWMMC_INT_CMD_ERROR)
        {
          /* RTO（响应超时）在探测流程里是正常现象：mmcsd 会先试
           * SD 的命令再试 MMC 的，不认得的那条必然超时。所以这里
           * 只在非 RTO 的错误上打日志，避免刷屏掩盖真正的问题。
           */

#ifdef CONFIG_RK3576_DWMMC_TRACE
          syslog(LOG_INFO, "DW CMD%" PRIu32 " err sts=0x%08" PRIx32 "\n",
                 cmd & MMCSD_CMDIDX_MASK, sts);
#else
          /* ★ RTO 也要记。曾经为避免刷屏而只记非 RTO 的错误，结果把
           * "CMD55 响应超时"这条最关键的信息屏蔽掉了，排查时看到的是
           * "驱动没报任何错误"，反而误导。刷屏可以靠限流解决，信息
           * 缺失无法补救。
           */

          {
            static int rto_reported;

            if ((sts & DWMMC_INT_RTO) == 0 || rto_reported < 8)
              {
                if (sts & DWMMC_INT_RTO)
                  {
                    rto_reported++;
                  }

                syslog(LOG_ERR,
                       "DWMMC CMD%" PRIu32 " %s RINTSTS=0x%08" PRIx32 "\n",
                       cmd & MMCSD_CMDIDX_MASK,
                       (sts & DWMMC_INT_RTO) ? "响应超时" : "响应错误", sts);
              }
          }
#endif

          dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_CMD_ERROR);
          priv->last_result = (sts & DWMMC_INT_RTO) ? -ETIMEDOUT : -EIO;
          return priv->last_result;
        }

      if (sts & DWMMC_INT_CMD_DONE)
        {
          dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_CMD_DONE);

          /* ★ 这一句必须在 #ifdef 外面。
           *
           *   它曾经写在下面那个 CONFIG_RK3576_DWMMC_TRACE 块里 —— 一句
           *   功能代码混进了调试块。而所有**失败**路径的赋值都在块外，
           *   于是关掉 trace 之后 last_result 永远停在命令发出前置的
           *   -EBUSY，recvshort/recvlong 开头那句
           *   `if (priv->last_result != OK) return ...` 就让每条命令的
           *   响应读取都失败。
           *
           *   现象是「SD 卡只有打开 trace 编译选项才能工作」，而这类
           *   现象最容易被当成竞态 —— 以为是日志的额外延时掩盖了时序
           *   问题，于是往延时、水位、时钟上找。实际上与时序毫无关系，
           *   条件编译把一句赋值圈进去了而已，编译器也不会有任何提示。
           *
           *   教训：#ifdef 块里只放"打印/统计"，任何改变状态的语句都要
           *   放在外面。开关一个调试选项不应该改变功能。
           */

          priv->last_result = OK;

#ifdef CONFIG_RK3576_DWMMC_TRACE
          /* 无响应的命令（如 CMD0）读 RESP0 得到的是上一条命令的残留，
           * 打出来会误导，所以按响应类型区分。
           */

          if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE)
            {
              syslog(LOG_INFO, "DW  CMD%" PRIu32 " 完成（无响应）\n",
                     cmd & MMCSD_CMDIDX_MASK);
            }
          else
            {
              syslog(LOG_INFO, "DW  CMD%" PRIu32 " 完成 resp=0x%08" PRIx32
                     "\n", cmd & MMCSD_CMDIDX_MASK,
                     dw_getreg(priv->base, DWMMC_RESP0));
            }
#endif
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR,
         "DW CMD%" PRIu32 " 无响应 RINTSTS=0x%08" PRIx32
         " STATUS=0x%08" PRIx32 "\n",
         cmd & MMCSD_CMDIDX_MASK, dw_getreg(priv->base, DWMMC_RINTSTS),
         dw_getreg(priv->base, DWMMC_STATUS));
  /* 命令卡在控制器里，后续命令多半也会被拖住。按参考驱动的做法复位
   * FIFO 与 DMA，把状态机拉回可用。
   */

  dw_fifo_reset(priv->base);

  priv->last_result = -ETIMEDOUT;
  return priv->last_result;
}

static int rk3576_dwmmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                  uint32_t *rshort)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  UNUSED(cmd);

  /* ★ 必须如实返回上一条命令的结果，不能无条件 OK。原因见
   * struct rk3576_dwmmc_dev_s 里 last_result 的说明。
   */

  if (priv->last_result != OK)
    {
      return priv->last_result;
    }

  if (rshort != NULL)
    {
      *rshort = dw_getreg(priv->base, DWMMC_RESP0);
    }

  return OK;
}

static int rk3576_dwmmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t rlong[4])
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  UNUSED(cmd);

  if (rlong == NULL)
    {
      return -EINVAL;
    }

  if (priv->last_result != OK)
    {
      return priv->last_result;
    }

  /* ★ dw_mmc 的 R2 与 SDHCI 不同，这里不需要移位。
   *
   *   SDHCI 把 R[127:8] 放在寄存器里，取值时要整体左移 8 位；
   *   dw_mmc 的 RESP0..3 已经是对齐的，RESP3 是最高 32 位。
   *   照搬 SDHCI 的移位会让 CID/CSD 整体错一个字节，表现为
   *   厂商号读成 0、容量算错 —— eMMC 那边正是这么发现的。
   */

  rlong[0] = dw_getreg(priv->base, DWMMC_RESP3);
  rlong[1] = dw_getreg(priv->base, DWMMC_RESP2);
  rlong[2] = dw_getreg(priv->base, DWMMC_RESP1);
  rlong[3] = dw_getreg(priv->base, DWMMC_RESP0);
  return OK;
}

#ifdef CONFIG_SDIO_BLOCKSETUP
static void rk3576_dwmmc_blocksetup(struct sdio_dev_s *dev,
                                    unsigned int blocklen,
                                    unsigned int nblocks)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  priv->blocksize = blocklen;
  priv->nblocks   = nblocks;
}
#endif

static int rk3576_dwmmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                  size_t nbytes)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  dw_fifo_reset(priv->base);
  priv->buffer    = buffer;
  priv->remaining = nbytes;
  priv->is_write  = false;
  return OK;
}

static int rk3576_dwmmc_sendsetup(struct sdio_dev_s *dev,
                                  const uint8_t *buffer, size_t nbytes)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  dw_fifo_reset(priv->base);
  priv->buffer    = (uint8_t *)buffer;
  priv->remaining = nbytes;
  priv->is_write  = true;
  return OK;
}

static int rk3576_dwmmc_cancel(struct sdio_dev_s *dev)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  priv->buffer    = NULL;
  priv->remaining = 0;
  dw_fifo_reset(priv->base);
  return OK;
}

/****************************************************************************
 * Name: dw_pio_transfer
 *
 * Description:
 *   PIO 方式搬数据。RK3576 的 dw_mmc 支持内部 DMA（IDMAC），但先用
 *   PIO 打通功能，DMA 是提速，不是必需 —— 一次只引入一处不确定性。
 *
 ****************************************************************************/

static int dw_pio_transfer(struct rk3576_dwmmc_dev_s *priv)
{
  uint32_t *p = (uint32_t *)priv->buffer;
  size_t words = priv->remaining / 4;
  size_t done = 0;
  uint32_t sts;
  int us;

#ifdef CONFIG_RK3576_DWMMC_TRACE
  syslog(LOG_INFO,
         "DW xfer %s remaining=%zu words=%zu blksz=%u BLKSIZ=%" PRIu32
         " BYTCNT=%" PRIu32 "\n",
         priv->is_write ? "写" : "读", priv->remaining, words,
         priv->blocksize, dw_getreg(priv->base, DWMMC_BLKSIZ),
         dw_getreg(priv->base, DWMMC_BYTCNT));
#endif

  for (us = 0; us < 2000000 && done < words; us++)
    {
      sts = dw_getreg(priv->base, DWMMC_RINTSTS);

      if (sts & DWMMC_INT_DATA_ERROR)
        {
          syslog(LOG_ERR,
                 "ERROR: DWMMC 数据错误 RINTSTS=0x%08" PRIx32
                 " 已传 %zu/%zu 字\n", sts, done, words);
          dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_DATA_ERROR);
          return -EIO;
        }

      if (priv->is_write)
        {
          /* FIFO 未满就继续灌 */

          while (done < words &&
                 (dw_getreg(priv->base, DWMMC_STATUS) & DWMMC_STATUS_FIFO_FULL) == 0)
            {
              dw_putreg(priv->base, priv->fifo_off, p[done++]);
            }
        }
      else
        {
          uint32_t fcnt = DWMMC_GET_FCNT(dw_getreg(priv->base, DWMMC_STATUS));

          while (done < words && fcnt-- > 0)
            {
              p[done++] = dw_getreg(priv->base, priv->fifo_off);
            }
        }

      if (done < words)
        {
          up_udelay(1);
        }
    }

  if (done < words)
    {
      syslog(LOG_ERR,
             "ERROR: DWMMC 数据传输超时 已传 %zu/%zu 字 STATUS=0x%08"
             PRIx32 "\n", done, words, dw_getreg(priv->base, DWMMC_STATUS));
      return -ETIMEDOUT;
    }

  /* 等 DATA_OVER —— 数据搬完不等于卡那边结束。 */

  for (us = 0; us < 500000; us++)
    {
      if (dw_getreg(priv->base, DWMMC_RINTSTS) & DWMMC_INT_DATA_OVER)
        {
          dw_putreg(priv->base, DWMMC_RINTSTS, DWMMC_INT_DATA_OVER);
          return OK;
        }

      up_udelay(1);
    }

  /* ★ TBBCNT 与 TCBCNT 是两个独立的计数器，正好把"数据卡在哪一段"
   * 分开：
   *     TBBCNT  主机 <-> FIFO 已传字节数
   *     TCBCNT  FIFO <-> 卡   已传字节数
   *
   *   TBBCNT 够而 TCBCNT 为 0  -> FIFO 没有往卡里排，问题在卡时钟或
   *                               FIFO 水位配置
   *   两者都够但没有 DATA_OVER -> 数据已送达，卡还没给出结束信号
   *   TBBCNT 不够              -> 我们自己没把数据推完
   */

  syslog(LOG_ERR,
         "ERROR: DWMMC 等 DATA_OVER 超时 RINTSTS=0x%08" PRIx32
         " STATUS=0x%08" PRIx32 " 主机侧=%" PRIu32 " 卡侧=%" PRIu32
         " 字节（应为 %zu）\n",
         dw_getreg(priv->base, DWMMC_RINTSTS), dw_getreg(priv->base, DWMMC_STATUS),
         dw_getreg(priv->base, DWMMC_TBBCNT), dw_getreg(priv->base, DWMMC_TCBCNT),
         words * 4);
  return -ETIMEDOUT;
}

static void rk3576_dwmmc_waitenable(struct sdio_dev_s *dev,
                                    sdio_eventset_t eventset,
                                    uint32_t timeout)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;

  priv->waitevents     = eventset;
  priv->waittimeout_ms = timeout;
}

static sdio_eventset_t rk3576_dwmmc_eventwait(struct sdio_dev_s *dev)
{
  struct rk3576_dwmmc_dev_s *priv = (struct rk3576_dwmmc_dev_s *)dev;
  int ret;

  if (priv->buffer == NULL || priv->remaining == 0)
    {
      priv->waitevents = 0;
      return SDIOWAIT_TRANSFERDONE;
    }

  ret = dw_pio_transfer(priv);

  priv->buffer     = NULL;
  priv->remaining  = 0;
  priv->waitevents = 0;

  return (ret == OK) ? SDIOWAIT_TRANSFERDONE : SDIOWAIT_ERROR;
}

static void rk3576_dwmmc_callbackenable(struct sdio_dev_s *dev,
                                        sdio_eventset_t eventset)
{
  UNUSED(dev);
  UNUSED(eventset);
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rk3576_dwmmc_registercallback(struct sdio_dev_s *dev,
                                         worker_t callback, void *arg)
{
  UNUSED(dev);
  UNUSED(callback);
  UNUSED(arg);
  return OK;
}
#endif

#ifdef CONFIG_SDIO_MUXBUS
static int rk3576_dwmmc_lock(struct sdio_dev_s *dev, bool lock)
{
  UNUSED(dev);
  UNUSED(lock);
  return OK;
}
#endif

static const struct sdio_dev_s g_dwmmc_ops =
{
#ifdef CONFIG_SDIO_MUXBUS
  .lock             = rk3576_dwmmc_lock,
#endif
  .reset            = rk3576_dwmmc_reset,
  .capabilities     = rk3576_dwmmc_capabilities,
  .status           = rk3576_dwmmc_status,
  .widebus          = rk3576_dwmmc_widebus,
  .clock            = rk3576_dwmmc_clock,
  .attach           = rk3576_dwmmc_attach,
  .sendcmd          = rk3576_dwmmc_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
  .blocksetup       = rk3576_dwmmc_blocksetup,
#endif
  .recvsetup        = rk3576_dwmmc_recvsetup,
  .sendsetup        = rk3576_dwmmc_sendsetup,
  .cancel           = rk3576_dwmmc_cancel,
  .waitresponse     = rk3576_dwmmc_waitresponse,
  .recv_r1          = rk3576_dwmmc_recvshort,
  .recv_r2          = rk3576_dwmmc_recvlong,
  .recv_r3          = rk3576_dwmmc_recvshort,
  .recv_r4          = rk3576_dwmmc_recvshort,
  .recv_r5          = rk3576_dwmmc_recvshort,
  .recv_r6          = rk3576_dwmmc_recvshort,
  .recv_r7          = rk3576_dwmmc_recvshort,
  .waitenable       = rk3576_dwmmc_waitenable,
  .eventwait        = rk3576_dwmmc_eventwait,
  .callbackenable   = rk3576_dwmmc_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  .registercallback = rk3576_dwmmc_registercallback,
#endif
};

/****************************************************************************
 * Name: rk3576_dwmmc_initialize
 *
 * Description:
 *   返回可交给 mmcsd_slotinitialize() 的 sdio_dev_s。
 *   必须在 rk3576_dwmmc_probe() 成功之后调用。
 *
 ****************************************************************************/

struct sdio_dev_s *rk3576_dwmmc_initialize(uint32_t base)
{
  struct rk3576_dwmmc_dev_s *priv = &g_dwmmc;
  uint32_t hcon;

  priv->base = base;
  uint32_t depth;

  priv->dev = g_dwmmc_ops;

  /* FIFO 数据窗口的偏移由 IP 版本决定，不是数据总线宽度（见头文件说明）。
   * 取错的话数据写进 CDTHRCTL，不报错但一个字节也到不了卡上。
   */

  hcon = dw_getreg(priv->base, DWMMC_HCON);
  UNUSED(hcon);

  priv->fifo_off =
      (DWMMC_GET_VERID(dw_getreg(priv->base, DWMMC_VERID)) >= DWMMC_VERID_240A) ?
      DWMMC_DATA_240A : DWMMC_DATA_OLD;

  /* 源时钟：上面把选源设成了 xin24m、不分频。 */



  /* FIFO 水位：msize=0（1 字突发），收发水位都取深度的一半。
   *
   * ★ 深度用 FIFOTH 复位值反推的实测值，不用 dtb 里写的 256。
   *   复位时 RX 水位是 depth/2-1，所以 depth = (读回值+1)*2；但
   *   Linux 在有 DT 属性时以 DT 为准，两者对不上时以硬件为准更稳妥 ——
   *   水位设得超过实际深度，FIFO 永远达不到阈值，数据就排不出去。
   */

  depth = (((dw_getreg(priv->base, DWMMC_FIFOTH) >> 16) & 0xfff) + 1);
  if (depth < 8 || depth > 4096)
    {
      depth = 128;
    }
  /* ★ msize 取 2，不是 0。
   *
   *   厂商驱动 dw_mmc.c 的初始化：
   *     host->fifoth_val = SDMMC_SET_FIFOTH(0x2, fifo_size / 2 - 1,
   *                                              fifo_size / 2);
   *   msize 是 FIFO 与卡之间的突发长度编码，写错会让搬运行为异常。
   */

  dw_putreg(priv->base, DWMMC_FIFOTH,
            DWMMC_SET_FIFOTH(0x2, depth / 2 - 1, depth / 2));

  /* 厂商在初始化时显式清 CLKSRC（该寄存器在新版 IP 上保留不用，
   * 但残留值会影响时钟选择）。
   */

  dw_putreg(priv->base, DWMMC_CLKSRC, 0);

  dw_putreg(priv->base, DWMMC_CTRL, DWMMC_CTRL_INT_ENABLE);

  syslog(LOG_INFO,
         "DWMMC: sdio_dev 就绪 FIFO 窗口=0x%03" PRIx32
         " 深度=%" PRIu32 "\n", priv->fifo_off, depth);

  return &priv->dev;
}

#endif /* CONFIG_RK3576_DWMMC */
