/****************************************************************************
 * chip/rk3576/rk3576_video.h
 *
 * RK3576 VICAP/CIF 的 V4L2 下半部（imgdata）。
 *
 * 板级拿到这个 imgdata 实例后，与 IMX415 的 imgsensor 一起调
 * capture_register("/dev/video0", ...) 注册成 V4L2 设备。
 *
 ****************************************************************************/

#ifndef __CHIP_RK3576_RK3576_VIDEO_H
#define __CHIP_RK3576_RK3576_VIDEO_H

#include <nuttx/config.h>
#include <nuttx/video/imgdata.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Name: rk3576_video_imgdata
 *
 * Description:
 *   取得 CIF 的 imgdata 实例。
 *
 *   ★ 调用前 CIF 必须已经由板级初始化过（时钟、D-PHY、CSI host）。
 *     本层只负责把取图接口包成 V4L2 要的形状，不做上电。
 *
 ****************************************************************************/

FAR struct imgdata_s *rk3576_video_imgdata(void);

#endif /* __ASSEMBLY__ */
#endif /* __CHIP_RK3576_RK3576_VIDEO_H */
