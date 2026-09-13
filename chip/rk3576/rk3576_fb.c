/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_fb.c
 *
 * RK3576 VOP2 帧缓冲，接到 NuttX 的 fb 框架上（/dev/fb0）。
 *
 * ★ 本驱动**不初始化显示链路**，只接管。
 *
 *   出厂 U-Boot 已经完成面板上电、发送厂商初始化序列、配置 D-PHY / DSI /
 *   VOP2 并在 VP1 上出图。那份面板初始化序列在厂商 dtsi 里，我们拿不到，
 *   一旦复位面板就无法恢复（表现为背光亮但永远无显示）。
 *
 *   因此这里只做三件事：从 VP1 的时序寄存器读出真实分辨率、分配帧缓冲、
 *   把 ESMART1 图层指过去。时序、DSI、D-PHY、面板一律不碰。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/video/fb.h>
#include <nuttx/kmalloc.h>

#include "arm64_internal.h"
#include "rk3576_vop2.h"

#ifdef CONFIG_RK3576_FB

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct fb_vtable_s   g_fb_vtable;
static struct fb_videoinfo_s g_videoinfo;
static struct fb_planeinfo_s g_planeinfo;
static void                *g_fbmem;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rk3576_getvideoinfo(struct fb_vtable_s *vtable,
                               struct fb_videoinfo_s *vinfo)
{
  if (vtable == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  *vinfo = g_videoinfo;
  return OK;
}

static int rk3576_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                               struct fb_planeinfo_s *pinfo)
{
  if (vtable == NULL || pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  *pinfo = g_planeinfo;
  return OK;
}

#ifdef CONFIG_FB_UPDATE

/****************************************************************************
 * Name: rk3576_updatearea
 *
 * Description:
 *   把被改动的那几行从 D-cache 刷回 DRAM。
 *
 * ★ 这就是"花屏"的原因
 *
 *   帧缓冲在 openvela 自己的堆里，而那段 DRAM 是按 MT_NORMAL（写回式
 *   可缓存）映射的。CPU 画完的像素先停在 cache 里，而 VOP2 是直接读
 *   DRAM 的非一致性主控 —— 它取到的是尚未写回的旧数据，屏上就是花的。
 *
 *   LVGL 的 NuttX 帧缓冲驱动每刷完一帧都会调 ioctl(FBIO_UPDATE)
 *   （lv_nuttx_fbdev.c）。我们**没实现这个回调**，ioctl 直接返回
 *   -ENOTTY，而 LVGL 的错误日志因为 LV_USE_LOG 默认关着也看不到 ——
 *   于是两边都不报错，只有屏上是花的。
 *
 *   chip/rk3576/rk3576_boot.c 里那条"自己在堆里分配的缓冲，VOP2 取出来
 *   的内容是错乱的，原因尚未查清"说的就是这件事，现在查清了。
 *
 *   只刷改动的行，不是整屏：720x1280x4 = 3.6MB，整屏刷会把每帧的开销
 *   都花在 cache 维护上。
 *
 ****************************************************************************/

static int rk3576_updatearea(struct fb_vtable_s *vtable,
                             const struct fb_area_s *area)
{
  uintptr_t start;
  uintptr_t end;

  UNUSED(vtable);

  if (g_fbmem == NULL || area == NULL)
    {
      return -ENODEV;
    }

  /* ★ 先整屏刷，不按 dirty area 刷。
   *
   *   板上现象是"绝大部分正常，少数几条**黑色细横条纹**"。黑色正是
   *   帧缓冲初始化时 memset(0) 的值 —— 也就是说那几行 LVGL 画过了，
   *   但没被写回 DRAM。两种可能都会造成这个：
   *
   *     1. 刷新范围按行算，而行宽 2880 字节不是 cache line 的整数倍
   *        （128 字节行时 2880/128 = 22.5），两端的半条 line 如果被
   *        向内取整就漏掉了；
   *     2. LVGL 报告的 dirty area 没覆盖它实际写过的全部行。
   *
   *   整屏刷对两者都成立，先用它把"是不是刷新覆盖问题"验掉 —— 一次
   *   3.6MB 的 clean 在这块 A53 上大约 1ms，而这是个每秒刷 4 次的
   *   仪表盘，代价可以接受。确认之后再收窄回按行刷（那时要把范围向外
   *   对齐到 cache line，而不是向内）。
   */

  UNUSED(area);

  start = (uintptr_t)g_fbmem;
  end   = start + g_planeinfo.fblen;

  up_flush_dcache(start, end);
  return OK;
}

#endif /* CONFIG_FB_UPDATE */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  size_t   fbsize;
  int      ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_fbmem != NULL)
    {
      return OK;              /* 已初始化 */
    }

  /* 分辨率从 VP1 的时序寄存器读，不写死 —— 那是 U-Boot 按厂商 dtsi
   * 配好的值，比文档可靠。
   */

  ret = rk3576_vop2_get_mode(&width, &height);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 读不到显示模式: %d\n", ret);
      return ret;
    }

  /* U-Boot 用的是 ARGB8888，沿用之，避免改格式牵连一串寄存器。 */

  stride = width * 4;
  fbsize = (size_t)stride * height;

  g_fbmem = kmm_memalign(256, fbsize);
  if (g_fbmem == NULL)
    {
      syslog(LOG_ERR, "ERROR: 帧缓冲分配失败 %zu 字节\n", fbsize);
      return -ENOMEM;
    }

  memset(g_fbmem, 0, fbsize);
  up_flush_dcache((uintptr_t)g_fbmem, (uintptr_t)g_fbmem + fbsize);

  ret = rk3576_vop2_fb_setup((uintptr_t)g_fbmem, width, height, stride);
  if (ret < 0)
    {
      kmm_free(g_fbmem);
      g_fbmem = NULL;
      return ret;
    }

  g_videoinfo.fmt     = FB_FMT_RGB32;
  g_videoinfo.xres    = width;
  g_videoinfo.yres    = height;
  g_videoinfo.nplanes = 1;

  g_planeinfo.fbmem   = g_fbmem;
  g_planeinfo.fblen   = fbsize;
  g_planeinfo.stride  = stride;
  g_planeinfo.display = display;
  g_planeinfo.bpp     = 32;

  g_fb_vtable.getvideoinfo = rk3576_getvideoinfo;
  g_fb_vtable.getplaneinfo = rk3576_getplaneinfo;
#ifdef CONFIG_FB_UPDATE
  g_fb_vtable.updatearea    = rk3576_updatearea;
#endif

  syslog(LOG_INFO,
         "FB: %" PRIu32 "x%" PRIu32 " ARGB8888 %zu 字节 @%p\n",
         width, height, fbsize, g_fbmem);
  return OK;
}

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || g_fbmem == NULL)
    {
      return NULL;
    }

  return &g_fb_vtable;
}

void up_fbuninitialize(int display)
{
  UNUSED(display);

  /* 不释放帧缓冲，也不关显示 —— 关掉之后无法重新初始化面板。 */
}

#endif /* CONFIG_RK3576_FB */
