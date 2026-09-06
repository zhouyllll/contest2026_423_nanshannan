/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_imgproc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 去马赛克（Bayer -> RGB）与 JPEG 编码。
 *
 * ★ 为什么需要
 *
 *   传感器出的是 Bayer 阵列，CIF 透传不做任何转换，所以取回来的是
 *   单通道 RAW12。原来的显示路径只取灰度（见 kickpi_camera_show 的说明：
 *   "彩色留到链路确认无误之后"），而 Vision LLM 要的是 JPEG。
 *   这个文件补上中间这两步。
 *
 * ★ 逐行处理，不做整帧缓冲
 *
 *   1932x1096 的 RGB888 是 6.35MB，而 ai_agent 跑起来之后堆只剩约 6.1MB
 *   —— 整帧转换直接分配不出来。所以每次只去马赛克**一行**，
 *   立刻喂给 libjpeg，峰值内存是一行 RGB（5.8KB）加上 libjpeg 自己的
 *   工作区。JPEG 也直接写文件（jpeg_stdio_dest），不在内存里攒。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>

#ifdef CONFIG_LIB_JPEG_TURBO
#  include <jpeglib.h>
#endif

#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_IMGPROC

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: raw_at
 *
 * Description:
 *   取一个 Bayer 采样，坐标越界时做镜像夹取。
 *
 *   ★ 边界用夹取而不是补零：补零会在图像四周造成一圈发暗的边，
 *     看起来像"取图少了几行"，很容易被误判成 CIF 的问题。
 *
 ****************************************************************************/

static inline uint16_t raw_at(FAR const uint16_t *raw, int stride_pix,
                              int width, int height, int x, int y)
{
  if (x < 0)
    {
      x = 1;
    }
  else if (x >= width)
    {
      x = width - 2;
    }

  if (y < 0)
    {
      y = 1;
    }
  else if (y >= height)
    {
      y = height - 2;
    }

  return raw[y * stride_pix + x];
}

/****************************************************************************
 * Name: to8
 *
 * Description:
 *   12 位采样映射到 8 位。
 *
 *   ★ 默认曝光下 12 位像素只落在很窄的一段（实测 196~299），直接右移 4
 *     得到的是 12~18，出来几乎全黑 —— 那会让"拍到了"看起来像"没拍到"。
 *     所以支持传入实测的黑/白电平做线性拉伸；不传（white <= black）时
 *     退回朴素的右移。
 *
 ****************************************************************************/

static inline uint8_t to8(uint16_t v, uint16_t black, uint16_t white)
{
  int t;

  if (white <= black)
    {
      return (uint8_t)(v >> 4);        /* RAW12 -> 8 位 */
    }

  if (v <= black)
    {
      return 0;
    }

  if (v >= white)
    {
      return 255;
    }

  t = ((int)(v - black) * 255) / (int)(white - black);
  return (uint8_t)t;
}

/****************************************************************************
 * Name: demosaic_row
 *
 * Description:
 *   双线性去马赛克，一次一行。
 *
 *   ★ Bayer 相位（哪个颜色在 (0,0)）没法凭空确定 —— 它取决于传感器的
 *     裁剪起点是奇是偶。**必须拿一张彩色标定图实测**：相位错了图像
 *     不会崩，只会红蓝互换或整体偏色，看起来像"白平衡不准"，
 *     很容易归错因。所以做成参数，默认值只是个起点。
 *
 *     phase: 0=RGGB  1=GRBG  2=GBRG  3=BGGR（(0,0) 处的颜色）
 *
 ****************************************************************************/

static void demosaic_row(FAR const uint16_t *raw, int stride_pix,
                         int width, int height, int y, int phase,
                         uint16_t black, uint16_t white,
                         FAR uint8_t *rgb)
{
  int x;

  for (x = 0; x < width; x++)
    {
      uint16_t r;
      uint16_t g;
      uint16_t b;
      int cx = (x & 1) ^ (phase & 1);
      int cy = (y & 1) ^ ((phase >> 1) & 1);

      /* cx/cy 归一之后一律按 RGGB 处理：
       *   (0,0)=R  (1,0)=G  (0,1)=G  (1,1)=B
       */

      uint16_t c  = raw_at(raw, stride_pix, width, height, x, y);
      uint16_t l  = raw_at(raw, stride_pix, width, height, x - 1, y);
      uint16_t rr = raw_at(raw, stride_pix, width, height, x + 1, y);
      uint16_t u  = raw_at(raw, stride_pix, width, height, x, y - 1);
      uint16_t d  = raw_at(raw, stride_pix, width, height, x, y + 1);
      uint16_t ul = raw_at(raw, stride_pix, width, height, x - 1, y - 1);
      uint16_t ur = raw_at(raw, stride_pix, width, height, x + 1, y - 1);
      uint16_t dl = raw_at(raw, stride_pix, width, height, x - 1, y + 1);
      uint16_t dr = raw_at(raw, stride_pix, width, height, x + 1, y + 1);

      if (cy == 0 && cx == 0)                    /* R 位 */
        {
          r = c;
          g = (uint16_t)((l + rr + u + d) >> 2);
          b = (uint16_t)((ul + ur + dl + dr) >> 2);
        }
      else if (cy == 0 && cx == 1)               /* R 行上的 G 位 */
        {
          g = c;
          r = (uint16_t)((l + rr) >> 1);
          b = (uint16_t)((u + d) >> 1);
        }
      else if (cy == 1 && cx == 0)               /* B 行上的 G 位 */
        {
          g = c;
          b = (uint16_t)((l + rr) >> 1);
          r = (uint16_t)((u + d) >> 1);
        }
      else                                       /* B 位 */
        {
          b = c;
          g = (uint16_t)((l + rr + u + d) >> 2);
          r = (uint16_t)((ul + ur + dl + dr) >> 2);
        }

      rgb[x * 3 + 0] = to8(r, black, white);
      rgb[x * 3 + 1] = to8(g, black, white);
      rgb[x * 3 + 2] = to8(b, black, white);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_imgproc_jpeg
 *
 * Description:
 *   把一帧 Bayer RAW 去马赛克并编码成 JPEG 写入 path。
 *
 * Input Parameters:
 *   raw        - RAW 数据，每像素 uint16
 *   stride_pix - 行跨距，以 uint16 计（不是宽度！CIF 要求 256 字节对齐，
 *                1932x2=3864 会被对齐到 4096，末尾有填充）
 *   width      - 有效宽度（像素）
 *   height     - 高度
 *   phase      - Bayer 相位，见 demosaic_row 的说明
 *   black,white- 线性拉伸的黑/白电平；white<=black 时退回右移 4
 *   quality    - JPEG 质量 1~100
 *   path       - 输出文件路径
 *
 ****************************************************************************/

int kickpi_imgproc_jpeg(FAR const uint16_t *raw, int stride_pix,
                        int width, int height, int phase,
                        uint16_t black, uint16_t white,
                        int quality, FAR const char *path)
{
#ifndef CONFIG_LIB_JPEG_TURBO
  nerr("ERROR: 未启用 CONFIG_LIB_JPEG_TURBO\n");
  return -ENOSYS;
#else
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  FAR uint8_t *rgbrow;
  FAR FILE *fp;
  int y;

  if (raw == NULL || path == NULL || width <= 0 || height <= 0)
    {
      return -EINVAL;
    }

  /* 只分配一行。整帧 RGB 是 width*height*3，1932x1096 就是 6.35MB，
   * agent 跑起来之后的堆余量放不下 —— 见文件头的说明。
   */

  rgbrow = kmm_malloc((size_t)width * 3);
  if (rgbrow == NULL)
    {
      nerr("ERROR: 分配 %d 字节行缓冲失败\n", width * 3);
      return -ENOMEM;
    }

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      nerr("ERROR: 打不开 %s: %d\n", path, errno);
      kmm_free(rgbrow);
      return -errno;
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width      = width;
  cinfo.image_height     = height;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  for (y = 0; y < height; y++)
    {
      JSAMPROW rows[1];

      demosaic_row(raw, stride_pix, width, height, y, phase,
                   black, white, rgbrow);

      rows[0] = (JSAMPROW)rgbrow;
      jpeg_write_scanlines(&cinfo, rows, 1);
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  fclose(fp);
  kmm_free(rgbrow);

  return OK;
#endif
}

/****************************************************************************
 * Name: kickpi_imgproc_jpeg_mem
 *
 * Description:
 *   同 kickpi_imgproc_jpeg，但编码到调用者给的内存缓冲区。
 *
 *   ★ V4L2 那条路要用这个：上层给的是一块 buffer，不是文件路径，
 *     而且回调要报**压缩后**的真实字节数。
 *
 *   ★ 用 jpeg_mem_dest 而不是自己攒：libjpeg 会在缓冲区不够时自己
 *     realloc，但我们给的是上层的固定缓冲区，不能被 realloc 掉。
 *     所以先用一个自有缓冲区编码，成功后再拷进去并校验长度 ——
 *     宁可多一次拷贝，也不能让 libjpeg 把上层的 buffer 换掉。
 *
 * Returned Value:
 *   成功返回写入的字节数（>0），失败返回负的 errno。
 *
 ****************************************************************************/

int kickpi_imgproc_jpeg_mem(FAR const uint16_t *raw, int stride_pix,
                            int width, int height, int phase,
                            uint16_t black, uint16_t white, int quality,
                            FAR uint8_t *out, size_t outlen)
{
#ifndef CONFIG_LIB_JPEG_TURBO
  return -ENOSYS;
#else
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  FAR unsigned char *jbuf = NULL;
  unsigned long jlen = 0;
  FAR uint8_t *rgbrow;
  int y;
  int ret;

  if (raw == NULL || out == NULL || outlen == 0 ||
      width <= 0 || height <= 0)
    {
      return -EINVAL;
    }

  rgbrow = kmm_malloc((size_t)width * 3);
  if (rgbrow == NULL)
    {
      return -ENOMEM;
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &jbuf, &jlen);

  cinfo.image_width      = width;
  cinfo.image_height     = height;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  for (y = 0; y < height; y++)
    {
      JSAMPROW rows[1];

      demosaic_row(raw, stride_pix, width, height, y, phase,
                   black, white, rgbrow);
      rows[0] = (JSAMPROW)rgbrow;
      jpeg_write_scanlines(&cinfo, rows, 1);
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  kmm_free(rgbrow);

  if (jbuf == NULL)
    {
      return -EIO;
    }

  if (jlen > outlen)
    {
      nerr("ERROR: JPEG %lu 字节放不进 %zu 字节的缓冲区\n", jlen, outlen);
      free(jbuf);
      return -E2BIG;
    }

  memcpy(out, jbuf, jlen);
  ret = (int)jlen;
  free(jbuf);          /* jpeg_mem_dest 用的是 malloc，要用 free */

  return ret;
#endif
}

/****************************************************************************
 * Name: kickpi_imgproc_selftest
 *
 * Description:
 *   用合成的 Bayer 图验证「去马赛克 + JPEG」这条通路。
 *
 *   ★ 为什么要有它
 *
 *     摄像头模块当前接不上（三个接口都探测不到 IMX415），没有真实数据
 *     就没法验证这条链路。合成一张**颜色已知**的 Bayer 图跑一遍，
 *     至少能回答：相位映射对不对、跨距索引对不对、JPEG 能不能解码。
 *     这些恰好是最容易写错、又最难从一张真实照片上看出来的地方。
 *
 *     图案是左中右三条竖带，分别是纯红、纯绿、纯蓝。解码后如果颜色
 *     顺序对，说明相位和通道映射没错；如果红蓝互换，说明 phase 差 1。
 *
 ****************************************************************************/

int kickpi_imgproc_selftest(int phase, FAR const char *path)
{
  const int w = 192;
  const int h = 128;
  const int stride = w;                 /* 自测图不需要对齐填充 */
  FAR uint16_t *raw;
  int x;
  int y;
  int ret;

  raw = kmm_malloc((size_t)stride * h * sizeof(uint16_t));
  if (raw == NULL)
    {
      return -ENOMEM;
    }

  for (y = 0; y < h; y++)
    {
      for (x = 0; x < w; x++)
        {
          int cx = (x & 1) ^ (phase & 1);
          int cy = (y & 1) ^ ((phase >> 1) & 1);
          int band = x / (w / 3);        /* 0=红 1=绿 2=蓝 */
          int is_r = (cy == 0 && cx == 0);
          int is_b = (cy == 1 && cx == 1);
          int is_g = !is_r && !is_b;
          int lit;

          lit = (band == 0 && is_r) || (band == 1 && is_g) ||
                (band == 2 && is_b);

          /* 亮的采样给满量程，暗的给 0 —— 用 12 位的量程 */

          raw[y * stride + x] = lit ? 4095 : 0;
        }
    }

  ret = kickpi_imgproc_jpeg(raw, stride, w, h, phase, 0, 4095, 90, path);
  kmm_free(raw);

  if (ret == OK)
    {
      syslog(LOG_INFO,
             "图像自测: 已写出 %s（%dx%d，相位 %d，红/绿/蓝三条竖带）\n",
             path, w, h, phase);
    }

  return ret;
}

#endif /* CONFIG_KICKPI_K7_IMGPROC */
