/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_reboot.c
 *
 * 复位与「重启进下载模式」。
 *
 * ★ 为什么值得单独做这件事：
 *
 *   每烧一次固件都要手按 recovery 键让板子进 loader 模式，一轮调试
 *   下来要按几十次。Rockchip 的引导器本来就支持软件触发 —— 往一个
 *   复位后仍保留的寄存器写魔数，U-Boot 启动时读到就进下载模式。
 *
 *   依据来自原厂 dtb 的 syscon-reboot-mode 节点：
 *
 *     syscon@26024000   compatible = "rockchip,rk3576-pmu0-grf"
 *     reboot-mode { offset = <64>;              // 0x40
 *                   mode-normal   = <0x5242C300>;
 *                   mode-loader   = <0x5242C301>;
 *                   mode-recovery = <0x5242C303>; ... }
 *
 *   复位手段用 CRU 的全局软复位（TRM Part1）：
 *     CRU_GLB_SRST_FST_VAL  0x0C08，写 0xfdb9 触发
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <syslog.h>

#include "arm64_internal.h"
#include "rk3576_reboot.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PMU0 GRF 里的重启原因寄存器。这个寄存器的内容在软复位后保留，
 * 引导器据此决定进哪种模式。
 */

#define RK3576_PMU0_GRF_ADDR      0x26024000
#define RK3576_REBOOT_MODE_OFF    0x40

#define RK3576_REBOOT_NORMAL      0x5242c300
#define RK3576_REBOOT_LOADER      0x5242c301
#define RK3576_REBOOT_RECOVERY    0x5242c303

/* CRU 全局软复位。写入 0xfdb9 触发第一级全局复位。 */

#define RK3576_CRU_GLB_SRST_FST   0x0c08
#define RK3576_GLB_SRST_MAGIC     0xfdb9

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rk3576_do_reset(uint32_t mode)
{
  putreg32(mode, RK3576_PMU0_GRF_ADDR + RK3576_REBOOT_MODE_OFF);

  /* 读回确认写进去了。这个寄存器要跨复位保留，写不进去的话下面复位
   * 完就是一次普通重启，而不是进下载模式 —— 两者现象差别很大，
   * 事先确认比事后猜省事。
   */

  syslog(LOG_INFO, "重启模式寄存器 = 0x%08lx（期望 0x%08lx）\n",
         (unsigned long)getreg32(RK3576_PMU0_GRF_ADDR +
                                 RK3576_REBOOT_MODE_OFF),
         (unsigned long)mode);

  /* 让串口把上面这行发完再复位 */

  up_mdelay(50);

  putreg32(RK3576_GLB_SRST_MAGIC,
           RK3576_CRU_ADDR + RK3576_CRU_GLB_SRST_FST);

  /* 复位是异步的，这里等它发生 */

  for (; ; )
    {
      up_udelay(1000);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rk3576_reboot_loader(void)
{
  syslog(LOG_INFO,
         "即将重启进下载模式，无需按 recovery 键。\n"
         "USB 会断开重连，之后可直接用 rkdeveloptool 烧写。\n");
  rk3576_do_reset(RK3576_REBOOT_LOADER);
}

void rk3576_reboot_normal(void)
{
  syslog(LOG_INFO, "正在重启\n");
  rk3576_do_reset(RK3576_REBOOT_NORMAL);
}
