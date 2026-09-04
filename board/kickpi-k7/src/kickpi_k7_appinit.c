/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_appinit.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/timers/oneshot.h>
#include "arm64_arch_timer.h"
#include <stdint.h>
#include <nuttx/board.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/drivers/drivers.h>
#include <arch/board/board.h>

#include "arm64_internal.h"
#include "rk3576_power.h"
#include "rk3576_sai.h"
#include "rk3576_dcphy.h"
#include "rk3576_dsi2.h"
#include "rk3576_dwmmc.h"
#include "rk3576_wdt.h"
#include "rk3576_rng.h"
#include "rk3576_gmac.h"
#include "rk3576_vop2.h"
#include "rk3576_sdhci.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  This function is never
 *   called directly from application code, but only indirectly via the
 *   (non-standard) boardctl() interface using the command BOARDIOC_INIT.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#if defined(CONFIG_FS_PROCFS) || defined(CONFIG_DEV_GPIO) || \
    defined(CONFIG_RK3576_I2C)
  int ret;
#endif

#ifdef CONFIG_FS_TMPFS
  /* xTS 的 cmocka 用例（syscall / fs / kv）都在
   * CONFIG_TESTS_TESTSUITES_MOUNT_DIR 下建文件，缺这个目录会在第一个
   * 用例就报 "Failed to switch the mount dir"。用 tmpfs 提供，不占用
   * eMMC，也不依赖存储先就绪。
   *
   * ★ 这一段原来嵌在 CONFIG_FS_PROCFS 的 #ifdef 里面。
   *
   *   两者毫无关系，一旦有人关掉 procfs，tmpfs 会跟着一起消失，而现象
   *   出现在很远的地方 —— cmocka 用例报 "Failed to switch the mount dir"。
   *   条件编译的嵌套错误不会有任何编译期提示，只能靠看出来。
   */

  ret = mount(NULL, CONFIG_TESTS_TESTSUITES_MOUNT_DIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 tmpfs 到 %s 失败: %d\n",
             CONFIG_TESTS_TESTSUITES_MOUNT_DIR, ret);
    }

  /* /tmp 也给一块 tmpfs。
   *
   * xTS 1.3.2 的命令是 `fstest -n 10 -m /tmp` —— 用例名字叫"RAM 读写"，
   * 实际测的是在**内存文件系统**上反复建删文件、校验内容。挂载点是写死
   * 在用例命令里的，板子上没有 /tmp 就直接失败，且报的是文件系统错误，
   * 看不出缺的只是一个挂载点。
   */

  ret = mount(NULL, "/tmp", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 tmpfs 到 /tmp 失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_PROCFS
  /* 挂载 procfs。
   *
   * CONFIG_NSH_ARCHINIT=y 时 NSH 不会自己挂载 /proc，改由本函数负责
   * （NSH 通过 boardctl(BOARDIOC_INIT) 调到这里）。没有它，free、ps、
   * uptime 等命令都会报 "Could not open /proc/... (is procfs mounted?)"。
   *
   * 挂载失败不作为致命错误：procfs 只是观察窗口，缺了它系统其余部分
   * 照常工作，因此仅记录日志、继续初始化后面的外设。
   */

  ret = mount(NULL, CONFIG_NSH_PROC_MOUNTPOINT, "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 procfs 到 %s 失败: %d\n",
             CONFIG_NSH_PROC_MOUNTPOINT, ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  /* 注册板上 GPIO 为 /dev/gpoutN。同样不作为致命错误 —— GPIO 不可用时
   * 串口控制台仍应能进入，便于继续排查。
   */

  ret = kickpi_k7_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GPIO 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_I2C
  ret = kickpi_k7_i2c_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: I2C 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_SPI
  ret = kickpi_k7_spi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SPI 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_SDHCI
  /* eMMC：拿到 sdio_dev_s 句柄，交给 mmcsd 上层注册 /dev/mmcsd0。
   * 失败不阻断其余初始化 —— 没有存储时控制台仍应可用，便于继续排查。
   */

    {
      struct sdio_dev_s *sdio = rk3576_sdhci_initialize(0);

      if (sdio == NULL)
        {
          syslog(LOG_ERR, "ERROR: eMMC 控制器初始化失败\n");
        }
      else
        {
          ret = mmcsd_slotinitialize(0, sdio);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize 失败: %d\n", ret);
            }
          else
            {
              /* ★ 不能只看返回值。mmcsd_slotinitialize() 在卡未识别时
               * 也返回 OK —— 它只把 -ENODEV 之外的错误当作失败，
               * 而块设备注册发生在更里层的 mmcsd_probe()，条件是
               * 分区块数非零。所以这里实际 stat 一下设备节点，
               * 存在才算就绪。
               */

              struct stat st;

              if (stat("/dev/mmcsd0", &st) == 0)
                {
                  syslog(LOG_INFO, "eMMC: /dev/mmcsd0 就绪\n");

#ifdef CONFIG_BCH
                  /* NuttX 的块设备不能直接被 open() 当文件读 ——
                   * dd / hexdump 这类工具走的是字符设备接口。
                   * BCH 把块设备包成字符设备，便于裸读验证与取分区表。
                   * 只读注册，避免误写坏 eMMC 上原有的分区。
                   */

                  ret = bchdev_register("/dev/mmcsd0", "/dev/mmcsd0c", true);
                  if (ret < 0)
                    {
                      syslog(LOG_ERR, "ERROR: 注册 /dev/mmcsd0c 失败: %d\n",
                             ret);
                    }
                  else
                    {
                      syslog(LOG_INFO,
                             "eMMC: /dev/mmcsd0c 就绪（只读字符视图）\n");
                    }
#endif
                }
              else
                {
                  syslog(LOG_ERR,
                         "ERROR: mmcsd_slotinitialize 返回 OK 但 "
                         "/dev/mmcsd0 不存在 —— 卡未被识别\n");
                }
            }
        }
    }
#endif

#ifdef CONFIG_RK3576_VOP2
  /* 显示链路第一步：VOP2 自检 + 内置彩条。
   *
   * ★ 彩条不经过帧缓冲与图层，只要 VOP 的时序发生器、像素时钟、
   *   以及到 MIPI 接口的通路对了就会出现。把"VOP 配置"与
   *   "图层/内存通路"两类问题分开验证。
   *
   *   注意此时 DSI 主机与 D-PHY 尚未实现，屏上不会有任何显示 ——
   *   本步只验证 VOP 侧寄存器可写、时序算得对。屏幕点亮要等
   *   DSI + D-PHY + 面板初始化都做完。
   */

#if defined(CONFIG_INPUT_FT5X06) || defined(CONFIG_RK3576_VOP2)
  /* ★ 必须排在显示链路之前。屏与触摸共用 VCC3V3_LCD_S0，面板要先上电、
   * 复位释放，之后配 D-PHY / DSI 才有意义；顺序反了的话 DSI 是对着一块
   * 没电的屏在配置。触摸同理 —— 没电时扫任何总线都不会应答。
   */

  ret = kickpi_k7_lcd_power(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: LCD 电源轨打开失败: %d\n", ret);
    }
#endif

    {
      /* 时序取自 docs/refs/panel/rk3308b-mipi-display-v11.dtsi 的
       * 720x1280 5 寸屏（ST7703）。规格与本板 F050008M01 一致
       * （4 lane RGB888、68x121mm），但那是另一块屏的数据 ——
       * 见 docs/refs/panel/README.md 中标注的适用边界。
       */

      static const struct rk3576_vop2_timing_s timing =
      {
        .pixclk_hz    = 65000000,
        .hactive      = 720,
        .hfront_porch = 48,
        .hsync_len    = 8,
        .hback_porch  = 52,
        .vactive      = 1280,
        .vfront_porch = 16,
        .vsync_len    = 6,
        .vback_porch  = 15,
      };

      /* 每通道码率 = 像素时钟 * 每像素位数 / 通道数，单位 kbps。
       * D-PHY 与 DSI 的时序换算都要用它，只算一次，避免两处各算一遍
       * 后悄悄不一致。
       */

      const uint32_t lane_kbps = timing.pixclk_hz / 1000 * 24 / 4;

      UNUSED(lane_kbps);

      /* ★ 先读后写。
       *
       * 这块板的出厂固件能在这块屏上显示 Rockchip logo，说明 U-Boot
       * 已经完整跑通过面板初始化 + VOP2 + DSI + D-PHY。它留在寄存器里
       * 的是一份针对这块屏的已知可用配置 —— 包括我们拿不到的面板初始化
       * 序列所产生的效果。先把它原样读出来。
       *
       * KEEP_UBOOT_DISPLAY 为 1 时完全不碰显示，用来判断：不去动它的话
       * logo 是不是还在。这一个观测能区分「U-Boot 根本没点屏」和
       * 「U-Boot 点了、被我们改配置改灭了」，而这两种情况此前无法区分。
       */

#define KEEP_UBOOT_DISPLAY 1

      rk3576_vop2_dump_uboot_state();
#ifdef CONFIG_RK3576_DSI2
      rk3576_dsi2_dump_uboot_state();
#endif

#if KEEP_UBOOT_DISPLAY
      /* 接管而不是重建。U-Boot 已经把这块屏点亮（面板初始化序列在拿不到
       * 的厂商 dtsi 里，复位就回不来），所以只把图层扩到全屏、指向我们
       * 自己的帧缓冲，其余一律不碰。
       */

#ifdef CONFIG_RK3576_FB
      ret = fb_register(0, 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: 注册 /dev/fb0 失败: %d\n", ret);
        }
      else
        {
          syslog(LOG_INFO, "显示: /dev/fb0 就绪\n");
        }
#endif
#else
      ret = rk3576_vop2_probe();
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: VOP2 探测失败: %d\n", ret);
        }
      else
        {
          rk3576_vop2_colorbar(&timing, true);
        }

#ifdef CONFIG_RK3576_DCPHY
      /* D-PHY：PLL 锁定是整条显示链路第一个真正的硬件反馈。
       * 码率 = 像素时钟 * 每像素位数 / 通道数 = 65MHz * 24 / 4。
       */

      if (rk3576_dcphy_probe() == OK)
        {
          rk3576_dcphy_enable(lane_kbps, 4);
        }
#endif

#ifdef CONFIG_RK3576_DSI2
      if (rk3576_dsi2_probe() == OK)
        {
          rk3576_dsi2_configure(&timing, 4, lane_kbps / 1000);   /* 换成 Mbps */

          /* 面板唤醒。厂商的完整初始化序列拿不到，但这两条是
           * MIPI DCS 标准命令、与具体面板无关，且几乎所有 MIPI 屏
           * 上电后都停在睡眠态 —— 不发这两条就是背光亮、无图像。
           *
           *   0x11 exit_sleep_mode   之后须等 >120ms
           *   0x29 set_display_on
           *
           * dtype 0x05 = DCS short write, no parameter。
           */

          {
            static const uint8_t dcs_sleep_out = 0x11;
            static const uint8_t dcs_display_on = 0x29;
            int r1;
            int r2;

            r1 = rk3576_dsi2_send_cmd(0x05, &dcs_sleep_out, 1);
            up_mdelay(150);
            r2 = rk3576_dsi2_send_cmd(0x05, &dcs_display_on, 1);
            up_mdelay(50);

            /* 返回值必须打出来。命令接口忙超时的话这两条根本没发出去，
             * 而"背光亮无图像"的现象与没发是一模一样的 —— 不打就分不清。
             */

            syslog(LOG_INFO,
                   "DSI2: 面板唤醒 sleep_out=%d display_on=%d\n", r1, r2);
          }

          /* ★ 配置完停在命令模式是没有图像的直接原因 —— DSI 不接收
           * VOP 送来的像素流。必须显式切到视频模式。
           *
           * 本板不能用命令模式：原理图第 27 页 FPC Pin18 (LCD_TE)
           * 打叉未连线，没有 TE 就无法与面板刷新同步。
           */

          rk3576_dsi2_set_video_mode();
        }
#endif

      /* 整条链路配完之后再问一句：VP 到底在不在扫描。
       * 放在最后是因为 DSI 起来前 VP 可能被下游反压。
       */

      rk3576_vop2_check_scanning(0);
#endif  /* KEEP_UBOOT_DISPLAY */
    }
#endif

#ifdef CONFIG_RK3576_SAI
  /* 音频前置链路：PD_AUDIO + 三路时钟 + 引脚复用 + 版本自检。
   * 必须在 kickpi_k7_audio_initialize() 之前 —— 后者依赖这里
   * 打开的电源域与时钟。
   */

  ret = rk3576_sai_probe();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SAI 探测失败: %d\n", ret);
    }
#endif

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)
  ret = kickpi_k7_audio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 音频初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_FT5X06
  ret = kickpi_k7_touch_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_DWMMC
  /* SD 卡（TF）控制器。与 eMMC 是两种不同的 IP，各走各的驱动。 */

  ret = rk3576_dwmmc_probe();
  if (ret < 0)
    {
      /* -ENODEV 是"卡不在位"，不是故障 —— SD 卡本来就是可插拔的，
       * 这里分开说，免得把"没插卡"记成"驱动坏了"。
       */

      syslog(ret == -ENODEV ? LOG_INFO : LOG_ERR,
             "SD 卡: %s (%d)\n",
             ret == -ENODEV ? "未插卡，跳过" : "控制器探测失败", ret);
    }
  else
    {
      struct sdio_dev_s *sd = rk3576_dwmmc_initialize();

      if (sd == NULL)
        {
          syslog(LOG_ERR, "ERROR: SD 卡 sdio_dev 初始化失败\n");
        }
      else
        {
          ret = mmcsd_slotinitialize(1, sd);
          if (ret < 0)
            {
              syslog(LOG_ERR,
                     "ERROR: SD 卡 mmcsd_slotinitialize 失败: %d\n", ret);
            }
          else
            {
              /* 同 eMMC：返回值不足为凭，实际 stat 节点才算数。 */

              struct stat sdst;

              if (stat("/dev/mmcsd1", &sdst) == 0)
                {
                  syslog(LOG_INFO, "SD 卡: /dev/mmcsd1 就绪\n");
                }
              else
                {
                  syslog(LOG_ERR,
                         "ERROR: SD 卡未被识别，/dev/mmcsd1 不存在\n");
                }
            }
        }
    }
#endif

#ifdef CONFIG_RK3576_RNG
  ret = rk3576_rng_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 硬件随机数初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "随机数: /dev/random 就绪\n");
    }
#endif

#if 0  /* ★ 暂时关闭，见下 —— 打开会让整个系统的定时功能失效 */
  /* 注册 /dev/oneshot（xTS 1.3.13/14 的 cmocka_driver_oneshot 需要它）。
   *
   * ★★ 这段代码一开就把系统时基打死了，实测证据见下。
   *
   *   原来的想法是"下半部直接用 arm64 通用定时器，本板不需要额外的定时
   *   器硬件"。问题在于**那个下半部不是空闲的，它是调度器正在用的那一个**：
   *
   *     arm64_arch_timer.c:
   *       static struct oneshot_lowerhalf_s g_arm64_oneshot_lowerhalf;  <- 唯一实例
   *       void up_timer_initialize(void)
   *       { up_alarm_set_lowerhalf(arm64_oneshot_initialize()); }        <- 内核已占用
   *
   *   而 struct oneshot_lowerhalf_s 里只有**一对** callback/arg。
   *   oneshot_register("/dev/oneshot", os) 之后，谁后设谁赢，调度器的
   *   定时回调被顶掉；再加上第二次 arm64_oneshot_initialize() 里那句
   *   arm64_arch_timer_set_compare(UINT64_MAX)，当场把已经挂起的闹钟取消。
   *
   *   后果是**整个系统的定时功能安静地死掉**，而且现象极具迷惑性：
   *
   *     串口、nsh、所有命令都正常      —— 靠 UART 中断唤醒，不需要时基
   *     up_mdelay() 正常               —— 忙等读计数器，不需要时基
   *     sleep 3 永远不返回             —— 实测 25 秒不回提示符
   *     work_queue(..., 延时) 永不触发 —— 看门狗 automonitor 因此从不喂狗，
   *                                       板子每 95 秒被硬件复位一次
   *
   *   最后一条尤其阴险：它把"板子自己重启"伪装成了别的模块的问题。我在
   *   摄像头那边白白追了好几轮，每次都是"某条命令之后板子重启了"，其实
   *   跟那条命令毫无关系。
   *
   *   实测对照（摘掉这段注册之后，其余不变）：
   *     sleep 3 -> 3.3s、sleep 8 -> 8.4s
   *     喂狗每 30 秒一次，CCVR 被拉回 2147483644
   *     静置 100 秒零自发重启
   *
   * ★ 正确的修法是给 /dev/oneshot 一个**自己的**硬件定时器。
   *
   *   RK3576 有独立的 TIMER 模块，用其中一路做 oneshot 下半部，与调度器
   *   占用的 ARM 通用定时器互不相干。在那之前先关掉 —— xTS 1.3.13 少一项，
   *   总好过整个系统的定时都是坏的却记着一堆"通过"。
   */

  {
    struct oneshot_lowerhalf_s *os = arm64_oneshot_initialize();

    if (os == NULL)
      {
        syslog(LOG_ERR, "ERROR: oneshot 下半部初始化失败\n");
      }
    else
      {
        ret = oneshot_register("/dev/oneshot", os);
        if (ret < 0)
          {
            syslog(LOG_ERR, "ERROR: 注册 /dev/oneshot 失败: %d\n", ret);
          }
        else
          {
            syslog(LOG_INFO, "定时器: /dev/oneshot 就绪\n");
          }
      }
  }
#endif

#ifdef CONFIG_RTC_HYM8563
  ret = kickpi_k7_rtc_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: RTC 初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "RTC: /dev/rtc0 就绪（HYM8563@I2C2:0x51）\n");
    }
#endif

#ifdef CONFIG_RK3576_I2C
  /* 摄像头传感器探测（不建立取图通路，见 kickpi_k7_camera.c 说明） */

  ret = kickpi_k7_camera_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 摄像头探测失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_GMAC
  /* 以太网前置链路：PD_SDGMAC + 时钟 + DMA 复位 + MDIO 读 PHY ID。
   *
   * ★ 两层判据强弱不同：MAC_VERSION 只证明寄存器块活着；
   *   PHY ID 才证明 MDIO 时序、PHY 供电与连线都对。
   *   前者正常而后者读回 0xffff，说明问题在板级而非 SoC 侧。
   *
   * 板上有两路千兆网口，先探 GMAC0。
   */

  /* ★ 必须先释放 PHY 复位。DWMAC 的 DMA 软复位需要 PHY 提供的接收
   * 时钟才能完成；PHY 被摁着时 SWR 位永不自清，表现为软复位超时，
   * 而 SoC 侧的寄存器读写一切正常。本端口为此误查过时钟与模块复位两轮。
   */

  rk3576_gmac_phy_reset(BOARD_GMAC0_RST_BANK, BOARD_GMAC0_RST_PIN, true);

  ret = rk3576_gmac_probe(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GMAC0 探测失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_WDT
  /* ★ 看门狗**必须最后注册**。
   *
   *   开了 CONFIG_WATCHDOG_AUTOMONITOR 之后，watchdog_register() 当场
   *   就把看门狗跑起来并开始自动喂。注册得早，本函数后面任何一处初始化
   *   卡住都会被判成"系统失控"而复位 —— 而复位之后又会走同一条初始化
   *   路径再卡住，于是变成**每 90 秒重启一次的死循环**。
   *
   *   那比原本的"挂死不动"更难救：挂死至少还能断电重来，启动死循环连
   *   nsh 提示符都进不去，`flash.sh` 靠串口敲 loader 的免 recovery 路子
   *   就用不了了。
   *
   *   放到最后，被看着的就只有"启动完成之后"这一段 —— 那正是我们想要
   *   的范围：跑测试跑挂了，自己回来。
   */

  ret = rk3576_wdt_initialize("/dev/watchdog0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 看门狗初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "看门狗: /dev/watchdog0 就绪\n");
    }
#endif

  return OK;
}
