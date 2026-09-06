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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 白平衡增益的定点基准：256 = 1.0 倍 */

#define AWB_UNITY 256

/* 增益的上下限。灰世界在"画面被单一颜色占满"时会失效 —— 它会把那个
 * 颜色硬拉成灰，红布拍出来变灰布。夹住上下限不能让算法变对，但能把
 * 失效时的破坏控制住，比放任它把画面拉爆要好。
 */

#define AWB_GAIN_MIN 128           /* 0.5 倍 */
#define AWB_GAIN_MAX 1024          /* 4.0 倍 */

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
 *   采样映射到 8 位。
 *
 *   ★★ 数据是 **12 位左对齐在 16 位容器里**（有效值在 bit[15:4]），
 *      不是右对齐。
 *
 *      这一条是上板量出来的：加光时 max=65520=0xFFF0，而 65520>>4=4095
 *      正好是 12 位满量程；遮光时值落在 3072~3328，>>4 得 192~208，
 *      与工程笔记里"默认曝光下 12 位像素只占到 196~299"完全吻合。
 *      现有显示路径（kickpi_camera_show）用的也是 >>8。
 *
 *      我一开始按右对齐写成 >>4，差了 16 倍 —— 而且这个错**不会报错**，
 *      只会让 JPEG 全白。V4L2 那条路正好走的是这个回退分支。
 *
 *   ★ 默认曝光下有效值只占很窄一段，直接位移出来几乎全黑，
 *     所以优先用实测的黑/白电平做线性拉伸。
 *
 ****************************************************************************/

static inline uint8_t to8g(uint16_t v, uint16_t black, uint16_t white,
                           int gq8)
{
  int t;

  if (white <= black)
    {
      t = (int)(v >> 8);               /* 12 位左对齐于 16 位 -> 8 位 */
    }
  else if (v <= black)
    {
      t = 0;
    }
  else if (v >= white)
    {
      t = 255;
    }
  else
    {
      t = ((int)(v - black) * 255) / (int)(white - black);
    }

  /* 白平衡增益，Q8 定点。放在拉伸**之后**是有意的：拉伸把有效动态范围
   * 铺满 0..255，增益只做通道间的相对校正，两件事互不干扰。反过来先乘
   * 增益再拉伸的话，measure_range 量到的范围会随增益一起漂。
   */

  t = (t * gq8) >> 8;
  return t > 255 ? 255 : (uint8_t)t;
}

static inline uint8_t to8(uint16_t v, uint16_t black, uint16_t white)
{
  return to8g(v, black, white, AWB_UNITY);
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
                         FAR const int *gain, FAR uint8_t *rgb)
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

      rgb[x * 3 + 0] = to8g(r, black, white, gain[0]);
      rgb[x * 3 + 1] = to8g(g, black, white, gain[1]);
      rgb[x * 3 + 2] = to8g(b, black, white, gain[2]);
    }
}

/****************************************************************************
 * Name: measure_awb
 *
 * Description:
 *   灰世界白平衡：假设整幅画面的平均色是中性灰，据此算出三个通道的
 *   相对增益。
 *
 *   ★ 为什么在 RAW 的 CFA 位置上统计，而不是在去马赛克之后的 RGB 上：
 *     去马赛克会把邻域的颜色混进来，红点处的 G 是插值猜出来的，拿它
 *     去估计"绿有多强"是在统计自己的插值结果。直接数 CFA 上真实采样
 *     到的那些点，统计量才是传感器的原始响应。
 *
 *   ★ 绿的增益固定为 1.0，只调红蓝。绿的采样点是红蓝的两倍、信噪比
 *     最好，拿它当基准最稳；而且这样增益只会提亮不会压暗，不损失
 *     已经拉伸好的动态范围。
 *
 *   ★ 只统计**黑电平以上**的像素。暗部噪声是加性的、三通道大致相同，
 *     把大片死黑算进去会把三个均值一起拉向同一个噪声底，比值被冲淡，
 *     算出来的增益偏向 1.0 —— 也就是"什么都没校正"。
 *
 ****************************************************************************/

static void measure_awb(FAR const uint16_t *raw, int stride_pix,
                        int width, int height, int phase,
                        uint16_t black, FAR int *gain)
{
  uint64_t sum[3];
  uint32_t cnt[3];
  int x;
  int y;
  int i;

  memset(sum, 0, sizeof(sum));
  memset(cnt, 0, sizeof(cnt));

  gain[0] = gain[1] = gain[2] = AWB_UNITY;

  /* ★ 按 2x2 的整块采样，不能简单地隔行隔列。
   *
   *   x/y 都按 2 步进的话，(x&1) 和 (y&1) 恒为 0，cx/cy 就是常数 ——
   *   四个 CFA 位置里只会命中一个，红或蓝必然一个样本都取不到。
   *   每 4 个像素取一整块 2x2，一块里恰好是 1 个 R、2 个 G、1 个 B，
   *   比例天然正确，采样量也只有 1/4。
   */

  for (y = 0; y + 1 < height; y += 4)
    {
      for (x = 0; x + 1 < width; x += 4)
        {
          int dx;
          int dy;

          for (dy = 0; dy < 2; dy++)
            {
              for (dx = 0; dx < 2; dx++)
                {
                  int px = x + dx;
                  int py = y + dy;
                  int cx = (px & 1) ^ (phase & 1);
                  int cy = (py & 1) ^ ((phase >> 1) & 1);
                  int ch = (cy == 0 && cx == 0) ? 0 :      /* R */
                           (cy == 1 && cx == 1) ? 2 : 1;   /* B : G */
                  uint16_t v = raw[(size_t)py * stride_pix + px];

                  if (v > black)
                    {
                      sum[ch] += (uint32_t)(v - black);
                      cnt[ch]++;
                    }
                }
            }
        }
    }

  if (cnt[0] == 0 || cnt[1] == 0 || cnt[2] == 0)
    {
      /* 某个通道一个有效样本都没有 —— 画面全黑或全饱和。
       * 这时候任何增益都是瞎猜，老老实实返回 1.0。
       */

      return;
    }

  {
    uint32_t mr = (uint32_t)(sum[0] / cnt[0]);
    uint32_t mg = (uint32_t)(sum[1] / cnt[1]);
    uint32_t mb = (uint32_t)(sum[2] / cnt[2]);

    if (mr == 0 || mb == 0)
      {
        return;
      }

    gain[0] = (int)(((uint64_t)mg * AWB_UNITY) / mr);
    gain[1] = AWB_UNITY;
    gain[2] = (int)(((uint64_t)mg * AWB_UNITY) / mb);
  }

  for (i = 0; i < 3; i++)
    {
      if (gain[i] < AWB_GAIN_MIN)
        {
          gain[i] = AWB_GAIN_MIN;
        }
      else if (gain[i] > AWB_GAIN_MAX)
        {
          gain[i] = AWB_GAIN_MAX;
        }
    }

  ninfo("AWB 增益 R/G/B = %d/%d/%d (x256)\n", gain[0], gain[1], gain[2]);
}

/****************************************************************************
 * Name: measure_range
 *
 * Description:
 *   扫一遍算出这一帧的实际动态范围。
 *
 *   ★ 为什么值得多扫一遍：默认曝光下有效值只占满量程的 5%，
 *     不拉伸的话编出来是一张几乎全黑的图，Vision LLM 什么也看不出来。
 *     相对 JPEG 编码的开销，多一次顺序扫描可以忽略。
 *
 *   ★ 用 1% / 99% 分位而不是 min/max —— 单个坏点就能把 min/max 拉飞，
 *     那样拉伸出来的图会整体发灰。
 *
 ****************************************************************************/

static void measure_range(FAR const uint16_t *raw, int stride_pix,
                          int width, int height,
                          FAR uint16_t *black, FAR uint16_t *white)
{
  uint32_t hist[256];
  uint32_t total = 0;
  uint32_t acc;
  uint32_t lo;
  uint32_t hi;
  int x;
  int y;
  int i;

  memset(hist, 0, sizeof(hist));

  /* 隔行隔列采样就够定范围了，省一半时间 */

  for (y = 0; y < height; y += 2)
    {
      for (x = 0; x < width; x += 2)
        {
          hist[raw[y * stride_pix + x] >> 8]++;
          total++;
        }
    }

  if (total == 0)
    {
      *black = 0;
      *white = 0;
      return;
    }

  lo = total / 100;
  hi = total - lo;

  acc = 0;
  *black = 0;
  for (i = 0; i < 256; i++)
    {
      acc += hist[i];
      if (acc >= lo)
        {
          *black = (uint16_t)(i << 8);
          break;
        }
    }

  acc = 0;
  *white = 65535;
  for (i = 0; i < 256; i++)
    {
      acc += hist[i];
      if (acc >= hi)
        {
          *white = (uint16_t)(i << 8 | 0xff);
          break;
        }
    }

  if (*white <= *black)
    {
      *black = 0;
      *white = 0;                      /* 退回固定位移 */
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
  int gain[3];
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

  if (white <= black)
    {
      /* 调用者没给范围，自己量 —— 见 measure_range 的说明 */

      measure_range(raw, stride_pix, width, height, &black, &white);
    }

  measure_awb(raw, stride_pix, width, height, phase, black, gain);

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
                   black, white, gain, rgbrow);

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

int kickpi_imgproc_jpeg_scaled(FAR const uint16_t *raw, int stride_pix,
                               int srcw, int srch, int dstw, int dsth,
                               int phase, uint16_t black, uint16_t white,
                               int quality, FAR uint8_t *out, size_t outlen)
{
#ifndef CONFIG_LIB_JPEG_TURBO
  return -ENOSYS;
#else
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  FAR unsigned char *jbuf = NULL;
  unsigned long jlen = 0;
  FAR uint8_t *rgbrow = NULL;
  FAR uint32_t *acc = NULL;
  FAR uint16_t *xcnt = NULL;
  FAR uint8_t *dstrow = NULL;
  int gain[3];
  int emitted = 0;
  int accrows = 0;
  int curdy = 0;
  int ret;
  int x;
  int y;

  if (raw == NULL || out == NULL || outlen == 0 ||
      srcw <= 0 || srch <= 0 || dstw <= 0 || dsth <= 0)
    {
      return -EINVAL;
    }

  /* 只做缩小。放大没有意义 —— 传感器给不出更多信息，插出来的像素只会
   * 让 JPEG 变大而画面不变清楚，还会让调用者以为拿到了更高的分辨率。
   */

  if (dstw > srcw || dsth > srch)
    {
      nerr("ERROR: 不支持放大 %dx%d -> %dx%d\n", srcw, srch, dstw, dsth);
      return -EINVAL;
    }

  rgbrow = kmm_malloc((size_t)srcw * 3);
  if (rgbrow == NULL)
    {
      return -ENOMEM;
    }

  if (white <= black)
    {
      measure_range(raw, stride_pix, srcw, srch, &black, &white);
    }

  measure_awb(raw, stride_pix, srcw, srch, phase, black, gain);

  if (dstw != srcw || dsth != srch)
    {
      /* 面积平均（box）缩小，不是抽点。
       *
       * ★ 抽点会把 Bayer 的马赛克结构混叠进来 —— 每隔 n 个像素取一个，
       *   取到的恰好总是同一种 CFA 位置，于是缩出来的图整体偏向那个
       *   通道的颜色。面积平均把一个目标像素覆盖的所有源像素都算进去，
       *   四种位置的比例自然均衡。
       *
       * ★ 累加器按目标行走，源行来一行就并进去，最后一次性除。全程
       *   只多占 dstw*3*4 + dstw*2 字节，不需要整帧 RGB。
       */

      acc = kmm_malloc((size_t)dstw * 3 * sizeof(uint32_t));
      xcnt = kmm_malloc((size_t)dstw * sizeof(uint16_t));
      dstrow = kmm_malloc((size_t)dstw * 3);
      if (acc == NULL || xcnt == NULL || dstrow == NULL)
        {
          ret = -ENOMEM;
          goto errout;
        }

      /* 每个目标列吃进多少源列 —— 每行都一样，先算好 */

      memset(xcnt, 0, (size_t)dstw * sizeof(uint16_t));
      for (x = 0; x < srcw; x++)
        {
          xcnt[x * dstw / srcw]++;
        }

      memset(acc, 0, (size_t)dstw * 3 * sizeof(uint32_t));
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &jbuf, &jlen);

  cinfo.image_width      = dstw;
  cinfo.image_height     = dsth;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  for (y = 0; y < srch; y++)
    {
      JSAMPROW rows[1];

      demosaic_row(raw, stride_pix, srcw, srch, y, phase,
                   black, white, gain, rgbrow);

      if (acc == NULL)
        {
          rows[0] = (JSAMPROW)rgbrow;
          jpeg_write_scanlines(&cinfo, rows, 1);
          emitted++;
          continue;
        }

      {
        int dy = y * dsth / srch;

        if (dy != curdy && accrows > 0)
          {
            /* 这一目标行的源行收齐了，除掉计数发出去 */

            for (x = 0; x < dstw; x++)
              {
                uint32_t n = (uint32_t)xcnt[x] * (uint32_t)accrows;
                int c;

                for (c = 0; c < 3; c++)
                  {
                    dstrow[x * 3 + c] = n ? (uint8_t)(acc[x * 3 + c] / n) : 0;
                  }
              }

            rows[0] = (JSAMPROW)dstrow;
            jpeg_write_scanlines(&cinfo, rows, 1);
            emitted++;

            memset(acc, 0, (size_t)dstw * 3 * sizeof(uint32_t));
            accrows = 0;
            curdy = dy;
          }

        for (x = 0; x < srcw; x++)
          {
            int dx = x * dstw / srcw;

            acc[dx * 3 + 0] += rgbrow[x * 3 + 0];
            acc[dx * 3 + 1] += rgbrow[x * 3 + 1];
            acc[dx * 3 + 2] += rgbrow[x * 3 + 2];
          }

        accrows++;
      }
    }

  /* 最后一行 */

  if (acc != NULL && accrows > 0 && emitted < dsth)
    {
      JSAMPROW rows[1];

      for (x = 0; x < dstw; x++)
        {
          uint32_t n = (uint32_t)xcnt[x] * (uint32_t)accrows;
          int c;

          for (c = 0; c < 3; c++)
            {
              dstrow[x * 3 + c] = n ? (uint8_t)(acc[x * 3 + c] / n) : 0;
            }
        }

      rows[0] = (JSAMPROW)dstrow;
      jpeg_write_scanlines(&cinfo, rows, 1);
      emitted++;
    }

  /* libjpeg 要求喂满声明的高度才肯收尾。整数映射的舍入偶尔会少一行，
   * 缺的用最后一行补上 —— 少一行的图比一个失败的编码有用得多。
   */

  while (emitted < dsth)
    {
      JSAMPROW rows[1];

      rows[0] = (JSAMPROW)(dstrow ? dstrow : rgbrow);
      jpeg_write_scanlines(&cinfo, rows, 1);
      emitted++;
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  if (jbuf == NULL)
    {
      ret = -EIO;
      goto errout;
    }

  if (jlen > outlen)
    {
      nerr("ERROR: JPEG %lu 字节放不进 %zu 字节的缓冲区\n", jlen, outlen);
      free(jbuf);
      ret = -E2BIG;
      goto errout;
    }

  memcpy(out, jbuf, jlen);
  ret = (int)jlen;
  free(jbuf);          /* jpeg_mem_dest 用的是 malloc，要用 free */

errout:
  kmm_free(rgbrow);
  kmm_free(acc);
  kmm_free(xcnt);
  kmm_free(dstrow);
  return ret;
#endif
}

int kickpi_imgproc_jpeg_mem(FAR const uint16_t *raw, int stride_pix,
                            int width, int height, int phase,
                            uint16_t black, uint16_t white, int quality,
                            FAR uint8_t *out, size_t outlen)
{
  return kickpi_imgproc_jpeg_scaled(raw, stride_pix, width, height,
                                    width, height, phase, black, white,
                                    quality, out, outlen);
}

/****************************************************************************
 * Name: kickpi_imgproc_bayerstat
 *
 * Description:
 *   统计 Bayer 四个位置各自的平均值。
 *
 *   ★ 这是**确定相位**的工具，比看照片可靠得多。
 *
 *     相位就是"(0,0) 处是哪个颜色"，它取决于传感器裁剪起点的奇偶。
 *     拿一个纯色物体（红最好）充满画面，哪个位置最亮，那个位置就是
 *     对应的颜色 —— 这是直接测量，不经过去马赛克，也不需要把图片
 *     传回主机。
 *
 *     Bayer 的四个位置里有两个是 G，它们在任何光照下都应当接近；
 *     若两个 G 差很多，说明取到的根本不是规整的 Bayer 阵列
 *     （跨距算错、或者 CIF 的裁剪没对齐），那是比相位更严重的问题。
 *
 *     判读：设四个位置为 (0,0) (1,0) (0,1) (1,1)。
 *       两个相近的是 G，它们必然在对角线上。
 *       另两个一个是 R 一个是 B，对着红色物体时亮的那个是 R。
 *       R 在 (0,0) -> 相位 0(RGGB)   R 在 (1,0) -> 相位 1(GRBG)
 *       R 在 (0,1) -> 相位 2(GBRG)   R 在 (1,1) -> 相位 3(BGGR)
 *
 ****************************************************************************/

int kickpi_imgproc_bayerstat(FAR const uint16_t *raw, int stride_pix,
                             int width, int height)
{
  uint64_t sum[4] = { 0, 0, 0, 0 };
  uint32_t cnt[4] = { 0, 0, 0, 0 };
  int x;
  int y;
  int i;

  if (raw == NULL || width < 4 || height < 4)
    {
      return -EINVAL;
    }

  /* 只统计中心一半区域 —— 边角有暗角和镜头渐晕，会把判断带偏 */

  for (y = height / 4; y < height * 3 / 4; y++)
    {
      for (x = width / 4; x < width * 3 / 4; x++)
        {
          int k = ((y & 1) << 1) | (x & 1);

          sum[k] += raw[y * stride_pix + x];
          cnt[k]++;
        }
    }

  syslog(LOG_INFO, "Bayer 四位平均（中心区域）：\n");
  for (i = 0; i < 4; i++)
    {
      syslog(LOG_INFO, "  (%d,%d) = %llu\n", i & 1, (i >> 1) & 1,
             cnt[i] ? (unsigned long long)(sum[i] / cnt[i]) : 0ull);
    }

  syslog(LOG_INFO,
         "判读：两个相近的是 G（必在对角线）；对红色物体时另两个中亮的是 R。\n"
         "      R 在 (0,0)->相位0  (1,0)->相位1  (0,1)->相位2  (1,1)->相位3\n");

  return OK;
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
