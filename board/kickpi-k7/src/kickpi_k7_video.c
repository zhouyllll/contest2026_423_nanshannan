/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_video.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 把 IMX415 包成 NuttX 视频框架的 imgsensor，和 CIF 的 imgdata 一起
 * 注册成 V4L2 设备 /dev/video0。
 *
 * ★ 为什么要有这一层：
 *
 *   ai_agent 的视觉能力（摄像头拍照 + Vision LLM 分析）只认 V4L2 设备。
 *   我们原来的 `cam` 命令走的是板级私有接口，agent 用不了。
 *
 * ★ 传感器的寄存器序列、探测、增益/VMAX 控制都在 kickpi_k7_camera.c，
 *   本文件只做**接口形状的转换**，不复制那边的逻辑 —— 复制一份等于
 *   多一个会和硬件失配的副本。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>

#include <nuttx/video/imgsensor.h>
#include <nuttx/video/v4l2_cap.h>
#include <sys/videoio.h>

#include "rk3576_video.h"
#include "kickpi_k7.h"

#if defined(CONFIG_RK3576_VIDEO) && defined(CONFIG_VIDEO)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* IMX415 在本板 cam3 上跑的模式。
 *
 * 出处：kickpi_k7_camera.c 的 g_imx415_mode_1932x1096 寄存器表 ——
 * 那张表是实测跑通 30fps 的那一组，这里的宽高必须与它一致，
 * 否则 CIF 按错误的尺寸算跨距，画面会斜。
 */

#define IMX415_WIDTH        1932
#define IMX415_HEIGHT       1096

/* 每像素 2 字节：IMX415 出 10-bit RAW，CIF 透传、按 16 位对齐落盘。 */

#define IMX415_BPP          2

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool imx415_is_available(FAR struct imgsensor_s *sensor);
static int  imx415_sensor_init(FAR struct imgsensor_s *sensor);
static int  imx415_sensor_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *imx415_get_driver_name(FAR struct imgsensor_s *s);
static int  imx415_validate_frame_setting(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type, uint8_t nr_fmt,
              FAR imgsensor_format_t *fmt,
              FAR imgsensor_interval_t *interval);
static int  imx415_sensor_start_capture(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type, uint8_t nr_fmt,
              FAR imgsensor_format_t *fmt,
              FAR imgsensor_interval_t *interval);
static int  imx415_sensor_stop_capture(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgsensor_ops_s g_imx415_ops =
{
  .is_available           = imx415_is_available,
  .init                   = imx415_sensor_init,
  .uninit                 = imx415_sensor_uninit,
  .get_driver_name        = imx415_get_driver_name,
  .validate_frame_setting = imx415_validate_frame_setting,
  .start_capture          = imx415_sensor_start_capture,
  .stop_capture           = imx415_sensor_stop_capture,

  /* get_frame_interval / get_supported_value / get_value / set_value
   * 先不实现。上半部对缺失的 op 会返回 -ENOTTY，行为明确；
   * 填一个假的实现反而会让上层以为控制项可用。
   * 曝光/增益已有 kickpi_camera_gain()/vmax()，接进来是下一步。
   */
};

static struct imgsensor_s g_imx415_sensor =
{
  .ops = &g_imx415_ops,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool imx415_is_available(FAR struct imgsensor_s *sensor)
{
  /* 板级初始化时已经探过 I2C 并读到过芯片 ID；这里再问一次会重复
   * 上电时序。kickpi_camera_status() 返回探测结果。
   */

  return kickpi_camera_status() >= 0;
}

static int imx415_sensor_init(FAR struct imgsensor_s *sensor)
{
  return OK;
}

static int imx415_sensor_uninit(FAR struct imgsensor_s *sensor)
{
  kickpi_camera_stream(false);
  return OK;
}

static FAR const char *imx415_get_driver_name(FAR struct imgsensor_s *s)
{
  return "IMX415";
}

static int imx415_validate_frame_setting(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type, uint8_t nr_fmt,
              FAR imgsensor_format_t *fmt,
              FAR imgsensor_interval_t *interval)
{
  if (nr_fmt != 1 || fmt == NULL)
    {
      return -EINVAL;
    }

  /* ★ 只认这一个模式。
   *
   *   现在只有 g_imx415_mode_1932x1096 这一张实测跑通的寄存器表，
   *   声称支持别的分辨率就是撒谎 —— 上层会设一个我们其实没配的尺寸，
   *   CIF 按错的跨距搬运，出来是斜纹，而且看不出是谁的错。
   */

  if (fmt[0].width != IMX415_WIDTH || fmt[0].height != IMX415_HEIGHT)
    {
      return -EINVAL;
    }

  if (fmt[0].pixelformat != V4L2_PIX_FMT_SBGGR10)
    {
      return -EINVAL;
    }

  return OK;
}

static int imx415_sensor_start_capture(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type, uint8_t nr_fmt,
              FAR imgsensor_format_t *fmt,
              FAR imgsensor_interval_t *interval)
{
  int ret;

  ret = imx415_validate_frame_setting(sensor, type, nr_fmt, fmt, interval);
  if (ret < 0)
    {
      return ret;
    }

  /* 先开接收侧（D-PHY + CSI host），再让传感器出流 ——
   * 反过来的话前几帧会打在还没就绪的接收端上。
   */

  ret = kickpi_camera_receiver(true);
  if (ret < 0)
    {
      return ret;
    }

  return kickpi_camera_stream(true);
}

static int imx415_sensor_stop_capture(FAR struct imgsensor_s *sensor,
              imgsensor_stream_type_t type)
{
  kickpi_camera_stream(false);
  kickpi_camera_receiver(false);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_video_initialize
 *
 * Description:
 *   注册 /dev/video0。必须在 kickpi_k7_camera_initialize() 之后调用 ——
 *   传感器的探测、MCLK、寄存器表都在那一步做，这里只做设备注册。
 *
 ****************************************************************************/

int kickpi_k7_video_initialize(void)
{
  FAR struct imgsensor_s *sensors[1];
  int ret;

  sensors[0] = &g_imx415_sensor;

  ret = capture_register("/dev/video0", rk3576_video_imgdata(),
                         sensors, 1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/video0 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "摄像头: /dev/video0 就绪（IMX415 %dx%d SBGGR10）\n",
         IMX415_WIDTH, IMX415_HEIGHT);
  return OK;
}

#endif /* CONFIG_RK3576_VIDEO && CONFIG_VIDEO */
