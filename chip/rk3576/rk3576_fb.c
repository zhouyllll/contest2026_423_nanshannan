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
