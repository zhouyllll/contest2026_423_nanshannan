/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_wdt.c
 *
 * RK3576 看门狗（Synopsys DesignWare WDT，watchdog@2ace0000）。
 *
 * 板级依据取自原厂 dtb：
 *   compatible = "snps,dw-wdt"
 *   reg        = <0x2ace0000 0x100>
 *   interrupts = <GIC_SPI 40>
 *   clocks     = tclk（xin24m）, pclk
 *
 * 时钟（clk-rk3576.c）：
 *   GATE(PCLK_WDT0, "pclk_wdt0", "pclk_bus_root", RK3576_CLKGATE_CON(16), 7)
 *   GATE(TCLK_WDT0, "tclk_wdt0", "xin24m",        RK3576_CLKGATE_CON(16), 8)
 *
 * ★ DesignWare 看门狗的超时不是任意值：它只能取 2^(16+i) 个 tclk 周期，
 *   i 为 0..15。也就是说超时时间是一组离散档位，设不出精确值。本驱动
 *   选「不小于请求值的最小档」，并把实际生效的值回填给上层 —— 上层若
 *   按请求值去算喂狗间隔，可能比实际超时更长而被复位。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/timers/watchdog.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_wdt.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_WDT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WDT_CR              0x00     /* 控制         */
#define WDT_CR_EN           (1 << 0)
#define WDT_CR_RMOD         (1 << 1) /* 0=直接复位 1=先中断再复位 */

#define WDT_TORR            0x04     /* 超时档位     */
#define WDT_CCVR            0x08     /* 当前计数值   */
#define WDT_CRR             0x0c     /* 喂狗         */
#define WDT_CRR_KICK        0x76     /* 喂狗魔数     */
#define WDT_STAT            0x10
#define WDT_EOI             0x14

#define WDT_GATE_CON        16
#define WDT_GATE_PCLK       7
#define WDT_GATE_TCLK       8

#define WDT_TCLK_HZ         24000000u   /* tclk 接 xin24m */
#define WDT_MAX_TOP         15

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_wdt_s
{
  struct watchdog_lowerhalf_s lower;   /* 必须是第一个成员 */
  uint32_t                    timeout_ms;   /* 上层请求的值 */
  uint32_t                    actual_ms;    /* 实际生效的档位 */
  bool                        started;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_wdt_s g_wdt;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t wdt_getreg(uint32_t off)
{
  return getreg32(RK3576_WDT_ADDR + off);
}

static inline void wdt_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, RK3576_WDT_ADDR + off);
}

/****************************************************************************
 * Name: wdt_top_for_ms
 *
 * Description:
 *   把毫秒换成 DesignWare 的超时档位。
 *
 *   档位 i 对应 2^(16+i) 个 tclk 周期。取不小于请求值的最小档 ——
 *   宁可超时比请求的长，也不能短：短了会在上层认为安全的间隔内复位。
 *
 ****************************************************************************/

static uint32_t wdt_top_for_ms(uint32_t ms, FAR uint32_t *actual_ms)
{
  uint64_t want = (uint64_t)ms * WDT_TCLK_HZ / 1000;
  uint32_t i;

  for (i = 0; i <= WDT_MAX_TOP; i++)
    {
      uint64_t cycles = 1ull << (16 + i);

      if (cycles >= want)
        {
          *actual_ms = (uint32_t)(cycles * 1000 / WDT_TCLK_HZ);
          return i;
        }
    }

  *actual_ms = (uint32_t)((1ull << (16 + WDT_MAX_TOP)) * 1000 / WDT_TCLK_HZ);
  return WDT_MAX_TOP;
}

/****************************************************************************
 * 下半部接口
 ****************************************************************************/

static int rk3576_wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;

  /* 先喂一次再使能，避免刚开就因残留计数被复位。 */

  wdt_putreg(WDT_CRR, WDT_CRR_KICK);

  /* RMOD=0：超时直接复位系统，不先产生中断。看门狗的意义就是在
   * 软件已经失控时把系统拉回来，中断处理程序未必还能运行。
   */

  wdt_putreg(WDT_CR, WDT_CR_EN);
  priv->started = true;
  return OK;
}

static int rk3576_wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;

  /* ★ DesignWare 看门狗一旦使能，**软件无法关闭** —— WDT_CR 的使能位
   *   是「写一次生效、只能靠复位清除」的。这里如实返回不支持，而不是
   *   假装成功：上层若以为已经停了而不再喂狗，会在超时后被复位，
   *   现象是"莫名其妙重启"，极难追查。
   */

  UNUSED(priv);
  syslog(LOG_WARNING,
         "WDT: DesignWare 看门狗使能后不能由软件关闭，忽略 stop\n");
  return -ENOSYS;
}

static int rk3576_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  UNUSED(lower);
  wdt_putreg(WDT_CRR, WDT_CRR_KICK);
  return OK;
}

static int rk3576_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  uint32_t ccvr;

  if (status == NULL)
    {
      return -EINVAL;
    }

  status->flags   = 0;
  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
    }

  /* ★ 报告**实际生效**的超时，不是上层请求的值。档位是离散的，
   * 两者往往不同；报请求值会让上层按一个并不存在的超时去安排喂狗。
   */

  status->timeout = priv->actual_ms;

  ccvr = wdt_getreg(WDT_CCVR);
  status->timeleft = (uint32_t)((uint64_t)ccvr * 1000 / WDT_TCLK_HZ);
  return OK;
}

static int rk3576_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  uint32_t actual;
  uint32_t top;

  top = wdt_top_for_ms(timeout, &actual);

  /* TORR 的低 4 位是复位后的档位，高 4 位是初始档位，两者写相同值。 */

  wdt_putreg(WDT_TORR, (top << 4) | top);
  wdt_putreg(WDT_CRR, WDT_CRR_KICK);

  priv->timeout_ms = timeout;
  priv->actual_ms  = actual;

  if (actual != timeout)
    {
      syslog(LOG_INFO,
             "WDT: 请求 %" PRIu32 "ms，档位 %" PRIu32
             " 实际 %" PRIu32 "ms（档位为 2^(16+i) 个 tclk，不连续）\n",
             timeout, top, actual);
    }

  return OK;
}

static const struct watchdog_ops_s g_wdt_ops =
{
  .start      = rk3576_wdt_start,
  .stop       = rk3576_wdt_stop,
  .keepalive  = rk3576_wdt_keepalive,
  .getstatus  = rk3576_wdt_getstatus,
  .settimeout = rk3576_wdt_settimeout,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_wdt_initialize(FAR const char *devpath)
{
  FAR struct rk3576_wdt_s *priv = &g_wdt;
  uint32_t torr;

  /* 两路时钟：pclk 供寄存器访问，tclk 驱动计数。只开 pclk 的话寄存器
   * 读写正常但计数不动 —— 这个坑在本端口的 I2C 与 SD 上都踩过。
   */

  rk3576_clk_gate(WDT_GATE_CON, WDT_GATE_PCLK, true);
  rk3576_clk_gate(WDT_GATE_CON, WDT_GATE_TCLK, true);

  priv->lower.ops = &g_wdt_ops;
  priv->started   = false;

  /* 自检：写一个档位再读回。读回一致说明 pclk 已开、基址与映射正确。
   * 注意这只验证 pclk，不验证 tclk —— tclk 是否在跑要看 CCVR 会不会变。
   */

  wdt_putreg(WDT_TORR, (3 << 4) | 3);
  torr = wdt_getreg(WDT_TORR);

  if ((torr & 0xf) != 3)
    {
      syslog(LOG_ERR,
             "ERROR: WDT TORR 写 3 读回 0x%08" PRIx32 " —— 寄存器块无响应\n",
             torr);
      return -ENODEV;
    }

  /* tclk 判据：计数器在未使能时也应随 tclk 递减。取两次读数比较，
   * 相同则说明 tclk 没在跑。
   */

  {
    uint32_t c0 = wdt_getreg(WDT_CCVR);
    uint32_t c1;

    up_udelay(200);
    c1 = wdt_getreg(WDT_CCVR);

    syslog(LOG_INFO,
           "WDT: TORR 回读正常 CCVR %" PRIu32 " -> %" PRIu32 " —— %s\n",
           c0, c1, (c0 != c1) ? "tclk 在跑" : "tclk 未动（使能后才计数）");
  }

  rk3576_wdt_settimeout(&priv->lower, 10000);

  /* watchdog_register 返回的是句柄（失败为 NULL），不是错误码。 */

  if (watchdog_register(devpath, &priv->lower) == NULL)
    {
      syslog(LOG_ERR, "ERROR: 注册 %s 失败\n", devpath);
      return -ENODEV;
    }

  return OK;
}

#endif /* CONFIG_RK3576_WDT */
