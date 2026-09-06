/****************************************************************************
 * chip/rk3576/rk3576_video.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3576 VICAP/CIF 的 V4L2 下半部（imgdata）。
 *
 * 把已有的 rk3576_cif_* 接口包成 NuttX 视频框架要求的 imgdata_ops_s，
 * 由 capture_register() 与 imgsensor 一起注册成 /dev/videoN。
 *
 * ★ 为什么要做这一层：
 *
 *   ai_agent 的视觉能力（摄像头拍照 + Vision LLM 分析）只认 V4L2 设备，
 *   我们原来的 `cam` 命令是直接调 rk3576_cif_* 的私有接口，agent 用不了。
 *
 * ★ 与原有取图路径的关键差别：中断，而不是轮询。
 *
 *   rk3576_cif_wait_frame() 是**轮询** CIF_MIPI_INTSTAT 的，适合
 *   `cam cap` 那种"要一帧就等一帧"的用法。而 V4L2 的 start_capture()
 *   交给你一个回调，要求帧结束时主动通知上层 —— 没法用轮询实现，
 *   否则得常驻一个线程空转。
 *
 *   所以这里挂真正的 CIF 中断（GIC_SPI 318 -> IRQ 350，出处
 *   rk3576.dtsi 的 rkcif@27c10000），在中断里清标志、算时间戳、
 *   调回调。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <sys/time.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/video/v4l2_cap.h>
#include <sys/videoio.h>

#include <arch/irq.h>

#include "rk3576_cif.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_VIDEO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CIF 中断。出处 rk3576.dtsi：
 *   rkcif: rkcif@27c10000 { interrupts = <GIC_SPI 318 IRQ_TYPE_LEVEL_HIGH>; };
 * SPI 号加 32 得到中断号，与本端口其它外设一致。
 */

#define RK3576_IRQ_CIF        (318 + 32)

/* 我们只用 host3（cam3 走的那一路，dtb 记载 imx415_3@0x37）。
 * 换接口时这里和板级的 sensor 一起改。
 */

#define RK3576_VIDEO_HOST     3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_video_s
{
  struct imgdata_s     data;        /* 必须是第一个成员 */

  bool                 capturing;
  bool                 irq_attached;

  uint16_t             width;
  uint16_t             height;
  uint32_t             framesize;   /* 一帧的字节数，回调要报 */

  uint8_t             *buf;         /* 上层给的目标缓冲区 */

  imgdata_capture_t    callback;
  void                *arg;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_video_init(FAR struct imgdata_s *data);
static int rk3576_video_uninit(FAR struct imgdata_s *data);
static int rk3576_video_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                uint8_t *addr, uint32_t size);
static int rk3576_video_validate_frame_setting(
                                FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval);
static int rk3576_video_start_capture(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval,
                                FAR imgdata_capture_t callback,
                                FAR void *arg);
static int rk3576_video_stop_capture(FAR struct imgdata_s *data);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_rk3576_video_ops =
{
  .init                   = rk3576_video_init,
  .uninit                 = rk3576_video_uninit,
  .set_buf                = rk3576_video_set_buf,
  .validate_frame_setting = rk3576_video_validate_frame_setting,
  .start_capture          = rk3576_video_start_capture,
  .stop_capture           = rk3576_video_stop_capture,
};

static struct rk3576_video_s g_rk3576_video =
{
  .data =
    {
      .ops = &g_rk3576_video_ops,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_video_interrupt
 *
 * Description:
 *   CIF 帧结束中断。清标志，把这一帧交给上层。
 *
 *   ★ 中断状态位必须在这里清 —— CIF 的中断是电平触发的（dtsi 写的是
 *     IRQ_TYPE_LEVEL_HIGH），不清就会立刻重入。GMAC 那边刚栽过同一个跟头。
 *
 ****************************************************************************/

static int rk3576_video_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)arg;
  struct timeval ts;
  uint32_t status;

  status = rk3576_cif_status(RK3576_VIDEO_HOST);

  /* 清标志（写 1 清）。不清则中断线一直拉着。 */

  rk3576_cif_clear_status(RK3576_VIDEO_HOST, status);

  if (priv->capturing && priv->callback != NULL)
    {
      gettimeofday(&ts, NULL);

      /* result=0 表示这一帧是好的。CIF 侧目前没有区分错误帧的判据，
       * 有了再补 —— 现在报成功而实际是坏帧，比谎称失败更容易被发现。
       */

      priv->callback(0, priv->framesize, &ts, priv->arg);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_init
 ****************************************************************************/

static int rk3576_video_init(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  /* 时钟、D-PHY、CSI host 的上电在板级初始化里已经做过
   * （rk3576_cif_probe 那条路径），这里只做本层的状态复位。
   */

  priv->capturing = false;
  priv->callback  = NULL;
  priv->arg       = NULL;
  priv->buf       = NULL;

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_uninit
 ****************************************************************************/

static int rk3576_video_uninit(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  if (priv->capturing)
    {
      rk3576_video_stop_capture(data);
    }

  if (priv->irq_attached)
    {
      up_disable_irq(RK3576_IRQ_CIF);
      irq_detach(RK3576_IRQ_CIF);
      priv->irq_attached = false;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_set_buf
 *
 * Description:
 *   上层指定这一帧写到哪。
 *
 *   ★ CIF 是硬件乒乓（FRM0/FRM1 两个地址寄存器）。V4L2 一次只给一个
 *     缓冲区，所以两个槽都指向它 —— 等于退化成单缓冲。
 *     这会在高帧率下丢帧，但正确性没问题：DMA 始终写在上层给的地址上。
 *     要用满乒乓得让上层一次给两个 buffer，那是 V4L2 队列的事，
 *     等基本通路跑通再说。
 *
 ****************************************************************************/

static int rk3576_video_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                uint8_t *addr, uint32_t size)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;
  int ret;

  if (addr == NULL || size == 0)
    {
      return -EINVAL;
    }

  priv->buf       = addr;
  priv->framesize = size;

  ret = rk3576_cif_set_buffer(RK3576_VIDEO_HOST, 0, (uintptr_t)addr);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_cif_set_buffer(RK3576_VIDEO_HOST, 1, (uintptr_t)addr);
}

/****************************************************************************
 * Name: rk3576_video_validate_frame_setting
 *
 * Description:
 *   上层问"这个格式/尺寸能不能拍"。
 *
 *   ★ CIF 在我们这条链路上是**透传**的：MIPI 收到什么就写什么，
 *     不做去马赛克也不做格式转换。IMX415 出的是 10-bit RAW，
 *     打包后按 2 字节/像素落盘，所以这里只认 SBGGR10 这一种。
 *     RGB/YUV 要等去马赛克做完才能声称支持 —— 现在谎报支持，
 *     上层会拿到一堆看不懂的字节。
 *
 ****************************************************************************/

static int rk3576_video_validate_frame_setting(
                                FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL)
    {
      return -EINVAL;
    }

  if (datafmts[0].pixelformat != V4L2_PIX_FMT_SBGGR10)
    {
      return -EINVAL;
    }

  if (datafmts[0].width == 0 || datafmts[0].height == 0)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_start_capture
 ****************************************************************************/

static int rk3576_video_start_capture(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval,
                                FAR imgdata_capture_t callback,
                                FAR void *arg)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;
  int ret;

  ret = rk3576_video_validate_frame_setting(data, nr_datafmts,
                                            datafmts, interval);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->buf == NULL)
    {
      nerr("ERROR: 还没设过缓冲区就要开始取图\n");
      return -EINVAL;
    }

  priv->width    = datafmts[0].width;
  priv->height   = datafmts[0].height;
  priv->callback = callback;
  priv->arg      = arg;

  if (!priv->irq_attached)
    {
      ret = irq_attach(RK3576_IRQ_CIF, rk3576_video_interrupt, priv);
      if (ret < 0)
        {
          nerr("ERROR: 挂 CIF 中断失败: %d\n", ret);
          return ret;
        }

      priv->irq_attached = true;
    }

  ret = rk3576_cif_start(RK3576_VIDEO_HOST,
                         (uintptr_t)priv->buf, (uintptr_t)priv->buf,
                         priv->width, priv->height);
  if (ret < 0)
    {
      return ret;
    }

  priv->capturing = true;
  up_enable_irq(RK3576_IRQ_CIF);

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_stop_capture
 ****************************************************************************/

static int rk3576_video_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  up_disable_irq(RK3576_IRQ_CIF);
  priv->capturing = false;
  priv->callback  = NULL;

  return rk3576_cif_stop(RK3576_VIDEO_HOST);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_video_imgdata
 *
 * Description:
 *   拿到 imgdata 实例，交给板级去和 imgsensor 一起 capture_register()。
 *
 ****************************************************************************/

FAR struct imgdata_s *rk3576_video_imgdata(void)
{
  return &g_rk3576_video.data;
}

#endif /* CONFIG_RK3576_VIDEO */
