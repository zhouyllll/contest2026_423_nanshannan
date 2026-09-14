/****************************************************************************
 * packages/demos/contest2026_423_k7diag/k7diag_main.c
 *
 * KICKPI-K7 板级诊断 —— 把采样和判断放到板子里做。
 *
 * ★ 为什么要有这个程序
 *
 *   排查触摸和显示时我一直在 nsh 里用 `mw` 直接读寄存器，结果被观测
 *   本身坑了：1.5M 波特率没有流控，而每条 nsh 命令还附带一行
 *   `nxposix_spawn_exec: ERROR: exec failed: 2`，两者撞在一起 ——
 *   实测连读 14 次 EXT_PORT 只有 2 次完整回来。
 *
 *   用会丢字节的通道去读寄存器，读回来的数本身就不可信。排查最怕的
 *   不是没有数据，是**被不可信的数据带偏**。所以把循环采样和判断放到
 *   板上，串口只出现一行结论。
 *
 * 子命令
 *   k7diag tp [秒]     采样触摸中断脚，报告有没有被拉低过
 *   k7diag fb <图案>   绕开 LVGL 直接往帧缓冲写图案并自己刷 cache
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <nuttx/arch.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <poll.h>
#include <time.h>
#include <arch/board/board.h>

uint32_t rk3576_gpio_irq_count(int bank, uint32_t *last_status);
void kickpi_touch_stat(uint32_t *samples, uint32_t *lows,
                       uint32_t *lowraw, uint32_t *edges);
extern uint32_t g_ft5x06_workers;
extern uint32_t g_ft5x06_reads;
extern uint32_t g_ft5x06_valids;
extern uint32_t g_ft5x06_delivered;
extern uint32_t g_ft5x06_lastraw;
extern uint32_t g_ft5x06_lastxy;

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO0 在常开域，基址与 GPIO1~4 不在一段（见 hardware/rk3576_memorymap.h）。
 * EXT_PORT 是只读的引脚实际电平，32 位一次给出整个 bank。
 */

#define GPIO0_BASE        0x27320000
#define GPIO_EXT_PORT     0x0070
#define GPIO_INT_EN_H     0x0014      /* pin16-31 的使能，pin21 => bit5 */
#define GPIO_INT_STATUS   0x0050
#define GPIO_INT_RAWSTAT  0x0058

#define TP_INT_PIN        BOARD_TP_INT_PIN     /* GPIO0_C5 = 21 */

static inline uint32_t rd32(uintptr_t a)
{
  return *(volatile uint32_t *)a;
}

/****************************************************************************
 * k7diag tp —— 触摸中断脚采样
 ****************************************************************************/

static int diag_tp(int secs)
{
  uint32_t ext;
  uint32_t raw;
  uint32_t last = 0;
  uint32_t isrs;
  int lows = 0;
  int raws = 0;
  int lowraw = 0;
  int edges = 0;
  int prev;
  int cur;
  int i;
  int n = secs * 200;                  /* 5ms 一次 */

  ext  = rd32(GPIO0_BASE + GPIO_EXT_PORT);
  prev = (ext >> TP_INT_PIN) & 1;

  printf("k7diag tp: 采样 %d 秒（5ms 一次），请现在按住屏幕并划动\n", secs);
  printf("  起始: INT=%d  INT_EN_H=0x%08" PRIx32 "  RAWSTATUS=0x%08" PRIx32 "\n",
         prev,
         rd32(GPIO0_BASE + GPIO_INT_EN_H),
         rd32(GPIO0_BASE + GPIO_INT_RAWSTAT));

  for (i = 0; i < n; i++)
    {
      ext = rd32(GPIO0_BASE + GPIO_EXT_PORT);
      raw = rd32(GPIO0_BASE + GPIO_INT_RAWSTAT);
      cur = (ext >> TP_INT_PIN) & 1;

      if (cur == 0)
        {
          lows++;

          /* ★ 这一对才是判据：引脚**正低着**的那一刻，中断原始状态位
           *   是不是也立起来了。只在开头结尾各读一次是不够的 ——
           *   结尾若恰好是高电平，RAWSTATUS=0 什么都说明不了（我上一版
           *   就是这么测的，白测了一轮）。
           */

          if ((raw >> TP_INT_PIN) & 1)
            {
              lowraw++;
            }
        }

      if ((raw >> TP_INT_PIN) & 1)
        {
          raws++;
        }

      if (cur != prev)
        {
          edges++;
        }

      prev = cur;
      usleep(5000);
    }

  isrs = rk3576_gpio_irq_count(0, &last);

  printf("  结果: 采样 %d 次 | 引脚低 %d | RAW 置位 %d | 低且RAW %d | 跳变 %d\n",
         n, lows, raws, lowraw, edges);
  printf("  bank0 ISR 进入 %" PRIu32 " 次，最近 INT_STATUS=0x%08" PRIx32 "\n",
         isrs, last);

  if (lows == 0)
    {
      printf("  判读: INT 线始终为高 —— 控制器没发中断请求，问题在片外/FT8756。\n");
    }
  else if (lowraw == 0)
    {
      printf("  判读: 引脚确实被拉低，但 RAWSTATUS 从不置位 ——\n");
      printf("        GPIO 的**电平检测逻辑没工作**（dbclk / 触发方式 / 极性）。\n");
    }
  else if (isrs == 0)
    {
      printf("  判读: RAWSTATUS 会置位，但 ISR 一次没进 ——\n");
      printf("        中断卡在 MASK 或 GIC 那一层，没到 CPU。\n");
    }
  else
    {
      printf("  判读: ISR 进了 %" PRIu32 " 次 —— 中断链路通，\n", isrs);
      printf("        问题在 ft5x06 驱动读数据或上报那一侧。\n");
    }

  return 0;
}

/****************************************************************************
 * k7diag fb —— 绕开 LVGL 写测试图案
 *
 * ★ 为什么必须绕开 LVGL
 *
 *   之前两次想用"填充帧缓冲"来区分「LVGL 画错了」和「VOP2 取错了」，
 *   都失败了 —— LVGL 每 250ms 还在重画，把写进去的图案盖掉，最后看到
 *   的仍是原来的画面。实验从设计上就被污染。
 *
 *   这里直接 mmap /dev/fb0 自己写，并且**自己调 FBIO_UPDATE 刷 cache**，
 *   配合先 `kill` 掉界面进程，屏上就只剩这里写进去的东西。
 *
 * 图案按行号自编码，坏在哪一行可以直接从屏上读出来：
 *   ramp  第 i 行整行填 (i & 0xff) 的灰阶 —— 纵向均匀渐变，任何一行错位
 *         都会在渐变里形成可见的断层
 *   bars  每 64 行换一种纯色（红绿蓝白循环），边界是直线；边界若变斜或
 *         错位，说明跨距或地址有问题
 *   grid  黑底 + 每 16 像素一条白线 —— 竖线变斜说明跨距错，横线缺失说明
 *         那几行没取到
 ****************************************************************************/

static int diag_fb(const char *mode)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct fb_area_s area;
  uint32_t *fb;
  uint32_t x;
  uint32_t y;
  int fd;

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      printf("打不开 /dev/fb0\n");
      return 1;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      printf("取不到 fb 信息\n");
      close(fd);
      return 1;
    }

  fb = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FILE, fd, 0);
  if (fb == MAP_FAILED)
    {
      printf("mmap 失败\n");
      close(fd);
      return 1;
    }

  printf("k7diag fb %s: %ux%u 跨距 %u 字节 @%p\n",
         mode, vinfo.xres, vinfo.yres, pinfo.stride, fb);

  for (y = 0; y < vinfo.yres; y++)
    {
      uint32_t *row = (uint32_t *)((uintptr_t)fb + (uintptr_t)y * pinfo.stride);

      for (x = 0; x < vinfo.xres; x++)
        {
          uint32_t c;

          if (strcmp(mode, "bars") == 0)
            {
              static const uint32_t bar[4] =
                {
                  0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff
                };

              c = bar[(y >> 6) & 3];
            }
          else if (strcmp(mode, "grid") == 0)
            {
              c = ((x % 16) == 0 || (y % 16) == 0) ? 0x00ffffff : 0;
            }
          else /* ramp */
            {
              uint32_t g = y & 0xff;

              c = (g << 16) | (g << 8) | g;
            }

          /* 每 64 行压一条纯白线做刻度，便于数出坏在第几行 */

          if ((y % 64) == 0)
            {
              c = 0x00ffffff;
            }

          row[x] = c;
        }
    }

  /* 自己刷 cache —— 不依赖 LVGL 的 flush 回调 */

  area.x = 0;
  area.y = 0;
  area.w = vinfo.xres;
  area.h = vinfo.yres;

  if (ioctl(fd, FBIO_UPDATE, (unsigned long)&area) < 0)
    {
      printf("  警告: FBIO_UPDATE 失败，cache 可能没刷回\n");
    }

  printf("  已写入并刷回。每 64 行一条白线，共 %u 条。\n",
         vinfo.yres / 64);

  munmap(fb, pinfo.fblen);
  close(fd);
  return 0;
}


/****************************************************************************
 * Name: diag_anim / diag_touch
 *
 * ★ 这两个子命令都**完全绕开 LVGL**
 *
 *   板上同时出现"刷新时有黑色细横线"和"触摸点了没反应"，而两者都经过
 *   LVGL。只要不把 LVGL 摘出去，就分不清是显示链路的问题、触摸链路的
 *   问题，还是 LVGL 自己的问题 —— 之前几轮就卡在这里。
 *
 *   anim  ：自己按双缓冲翻页画动画。动的是整屏，撕裂/黑线一眼可见。
 *           如果它干净，说明 VOP2 + 翻页 + 刷 cache 这条链没问题，
 *           黑线只能来自 LVGL 的画法。
 *   touch ：自己读 /dev/input0，把屏幕分成 6 个色块，点中哪个就把它
 *           点亮并在串口打坐标。如果它能响应，说明触摸链路直到应用层
 *           都是通的，问题只在 LVGL 的绑定/事件。
 ****************************************************************************/

struct fbctx_s
{
  int       fd;
  uint32_t *mem;          /* 整块映射（含两个缓冲） */
  uint32_t  w;
  uint32_t  h;
  uint32_t  stride;
  size_t    fblen;
  int       nbuf;         /* 1 或 2 */
  int       cur;          /* 当前画哪一块 */
  struct fb_planeinfo_s pinfo;
};

static int fb_open(struct fbctx_s *c)
{
  struct fb_videoinfo_s vinfo;

  c->fd = open("/dev/fb0", O_RDWR);
  if (c->fd < 0)
    {
      printf("打不开 /dev/fb0\n");
      return -1;
    }

  memset(&c->pinfo, 0, sizeof(c->pinfo));
  if (ioctl(c->fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(c->fd, FBIOGET_PLANEINFO, (unsigned long)&c->pinfo) < 0)
    {
      printf("取不到 fb 信息\n");
      close(c->fd);
      return -1;
    }

  c->w      = vinfo.xres;
  c->h      = vinfo.yres;
  c->stride = c->pinfo.stride;
  c->fblen  = c->pinfo.fblen;
  c->nbuf   = (c->pinfo.yres_virtual == vinfo.yres * 2) ? 2 : 1;
  c->cur    = 0;

  c->mem = mmap(NULL, c->fblen, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FILE, c->fd, 0);
  if (c->mem == MAP_FAILED)
    {
      printf("mmap 失败\n");
      close(c->fd);
      return -1;
    }

  printf("fb: %" PRIu32 "x%" PRIu32 " 跨距=%" PRIu32
         " yres_virtual=%" PRIu32 " -> %s\n",
         c->w, c->h, c->stride, c->pinfo.yres_virtual,
         c->nbuf == 2 ? "双缓冲" : "★单缓冲（会撕裂）");
  return 0;
}

/* 返回当前要画的那一块的首地址 */

static uint32_t *fb_back(struct fbctx_s *c)
{
  size_t off = (c->nbuf == 2 && c->cur) ?
               (size_t)c->stride * c->h : 0;
  return (uint32_t *)((uintptr_t)c->mem + off);
}

/* 刷 cache 并翻页 */

static void fb_present(struct fbctx_s *c)
{
  struct fb_area_s area;

  area.x = 0;
  area.y = (c->nbuf == 2 && c->cur) ? c->h : 0;
  area.w = c->w;
  area.h = c->h;
  ioctl(c->fd, FBIO_UPDATE, (unsigned long)&area);

  if (c->nbuf == 2)
    {
      c->pinfo.yoffset = c->cur ? c->h : 0;
      ioctl(c->fd, FBIOPAN_DISPLAY, (unsigned long)&c->pinfo);
      c->cur ^= 1;
    }
}

static void fb_close(struct fbctx_s *c)
{
  munmap(c->mem, c->fblen);
  close(c->fd);
}

static int diag_anim(int secs)
{
  struct fbctx_s c;
  struct timespec t0;
  struct timespec now;
  uint32_t frames = 0;

  if (fb_open(&c) < 0)
    {
      return 1;
    }

  printf("动画 %d 秒：一条 120 像素高的亮带从上往下扫，背景是竖直渐变。\n"
         "  亮带边缘应当笔直、连续；出现黑色细横线或断裂就是撕裂/没刷回。\n",
         secs);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (; ; )
    {
      uint32_t *fb = fb_back(&c);
      uint32_t  bar;
      uint32_t  y;
      long      ms;

      clock_gettime(CLOCK_MONOTONIC, &now);
      ms = (now.tv_sec - t0.tv_sec) * 1000 +
           (now.tv_nsec - t0.tv_nsec) / 1000000;
      if (ms > (long)secs * 1000)
        {
          break;
        }

      /* 亮带位置随时间走，整屏每帧都重画 —— 撕裂无处可藏 */

      bar = (uint32_t)((ms / 4) % c.h);

      for (y = 0; y < c.h; y++)
        {
          uint32_t *row = (uint32_t *)((uintptr_t)fb +
                                       (uintptr_t)y * c.stride);
          uint32_t  d   = (y >= bar && y < bar + 120) ? 1 : 0;
          uint32_t  x;

          for (x = 0; x < c.w; x++)
            {
              if (d)
                {
                  row[x] = 0x00ffffff;              /* 亮带：纯白 */
                }
              else
                {
                  uint32_t g = (x * 255) / c.w;     /* 背景：横向渐变 */
                  row[x] = (g << 16) | (0x40 << 8) | (255 - g);
                }
            }
        }

      fb_present(&c);
      frames++;
    }

  printf("共 %" PRIu32 " 帧，约 %" PRIu32 " fps。\n",
         frames, frames / (secs ? secs : 1));
  fb_close(&c);
  return 0;
}

static int diag_touch(int secs)
{
  static const uint32_t color[6] =
  {
    0x00ff0000, 0x0000ff00, 0x000000ff,
    0x00ffff00, 0x00ff00ff, 0x0000ffff
  };
  static const char *name[6] =
  {
    "红", "绿", "蓝", "黄", "品红", "青"
  };

  struct fbctx_s c;
  struct timespec t0;
  struct timespec now;
  int      tfd;
  int      hits = 0;
  int      last = -1;

  if (fb_open(&c) < 0)
    {
      return 1;
    }

  tfd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
  if (tfd < 0)
    {
      printf("打不开 /dev/input0\n");
      fb_close(&c);
      return 1;
    }

  printf("触摸测试 %d 秒：屏幕分成 2 列 x 3 行共 6 块。\n"
         "  点哪一块，哪一块变白，串口同时打出坐标和块号。\n"
         "  这条路径不经过 LVGL —— 有反应就说明触摸链路到应用层是通的。\n",
         secs);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (; ; )
    {
      struct touch_sample_s sample;
      uint32_t *fb = fb_back(&c);
      uint32_t  y;
      int       cell = last;
      long      ms;
      ssize_t   n;

      clock_gettime(CLOCK_MONOTONIC, &now);
      ms = (now.tv_sec - t0.tv_sec) * 1000 +
           (now.tv_nsec - t0.tv_nsec) / 1000000;
      if (ms > (long)secs * 1000)
        {
          break;
        }

      n = read(tfd, &sample, sizeof(sample));
      if (n >= (ssize_t)SIZEOF_TOUCH_SAMPLE_S(1) && sample.npoints >= 1)
        {
          int px = sample.point[0].x;
          int py = sample.point[0].y;

          if ((sample.point[0].flags & TOUCH_UP) != 0)
            {
              printf("  抬手 (%d,%d)\n", px, py);
            }
          else
            {
              int col = (px * 2) / (int)c.w;
              int row = (py * 3) / (int)c.h;

              if (col < 0) col = 0;
              if (col > 1) col = 1;
              if (row < 0) row = 0;
              if (row > 2) row = 2;

              cell = row * 2 + col;
              if (cell != last)
                {
                  hits++;
                  printf("  按下 (%4d,%4d) -> 第 %d 块（%s）\n",
                         px, py, cell, name[cell]);
                }
            }
        }

      /* 画 6 个色块，被点中的那块画白 */

      for (y = 0; y < c.h; y++)
        {
          uint32_t *r = (uint32_t *)((uintptr_t)fb +
                                     (uintptr_t)y * c.stride);
          uint32_t  x;
          int       row = (int)((y * 3) / c.h);

          for (x = 0; x < c.w; x++)
            {
              int col = (int)((x * 2) / c.w);
              int idx = row * 2 + col;

              r[x] = (idx == cell) ? 0x00ffffff : color[idx];
            }
        }

      fb_present(&c);
      last = cell;
      usleep(20000);
    }

  printf("共识别 %d 次落点。%s\n", hits,
         hits > 0 ? "触摸链路到应用层是通的。" :
                    "★一次都没读到 —— 问题在 LVGL 之前。");
  close(tfd);
  fb_close(&c);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "tp") == 0)
    {
      return diag_tp(argc >= 3 ? atoi(argv[2]) : 10);
    }

  if (argc >= 2 && strcmp(argv[1], "stat") == 0)
    {
      uint32_t sm;
      uint32_t lo;
      uint32_t lr;
      uint32_t ed;
      uint32_t last = 0;
      uint32_t isrs = rk3576_gpio_irq_count(0, &last);

      kickpi_touch_stat(&sm, &lo, &lr, &ed);

      printf("触摸中断脚（开机起常驻采样，5ms 一次）\n");
      printf("  采样 %" PRIu32 " | 引脚低 %" PRIu32 " | 低且RAW %" PRIu32
             " | 跳变 %" PRIu32 "\n", sm, lo, lr, ed);
      printf("  bank0 ISR 进入 %" PRIu32 " 次，最近 INT_STATUS=0x%08" PRIx32 "\n",
             isrs, last);

      printf("  ft5x06: worker %" PRIu32 " | 读成功 %" PRIu32
             " | 解码有效 %" PRIu32 " | 交付样本 %" PRIu32 "\n",
             g_ft5x06_workers, g_ft5x06_reads,
             g_ft5x06_valids, g_ft5x06_delivered);
      printf("  最近一帧 buf[0..3]=0x%08" PRIx32 "  最近坐标 x=%" PRIu32
             " y=%" PRIu32 "\n",
             g_ft5x06_lastraw, g_ft5x06_lastxy >> 16,
             g_ft5x06_lastxy & 0xffff);

      if (lo == 0)
        {
          printf("  判读: 引脚从未被拉低 —— 要么没人碰屏，要么控制器不发中断。\n");
        }
      else if (lr == 0)
        {
          printf("  判读: 引脚被拉低过 %" PRIu32 " 次，但 RAWSTATUS 从不置位 ——\n", lo);
          printf("        GPIO 的电平检测逻辑没工作（dbclk / 触发方式 / 极性）。\n");
        }
      else if (isrs == 0)
        {
          printf("  判读: RAWSTATUS 会置位但 ISR 一次没进 —— 卡在 MASK 或 GIC。\n");
        }
      else
        {
          printf("  判读: 中断链路通，问题在 ft5x06 驱动读数据/上报那一侧。\n");
        }

      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "anim") == 0)
    {
      return diag_anim(argc >= 3 ? atoi(argv[2]) : 10);
    }

  if (argc >= 2 && strcmp(argv[1], "touch") == 0)
    {
      return diag_touch(argc >= 3 ? atoi(argv[2]) : 20);
    }

  if (argc >= 2 && strcmp(argv[1], "fb") == 0)
    {
      return diag_fb(argc >= 3 ? argv[2] : "ramp");
    }

  printf("用法:\n");
  printf("  k7diag stat           读常驻采样的累计结果（不用对时）\n");
  printf("  k7diag tp [秒]        现场采样触摸中断脚\n");
  printf("  k7diag fb ramp|bars|grid   绕开 LVGL 写测试图案\n");
  printf("  k7diag anim [秒]           绕开 LVGL 的双缓冲翻页动画（查撕裂/黑线）\n");
  printf("  k7diag touch [秒]          绕开 LVGL 的色块点击测试（查触摸链路）\n");
  return 1;
}
