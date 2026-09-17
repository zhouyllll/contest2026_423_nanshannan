/****************************************************************************
 * packages/demos/contest2026_423_kickpi_ui/kickpi_ui_main.c
 *
 * KICKPI-K7 板载仪表盘 —— 把这块 BSP 上跑着的东西直接画到屏上。
 *
 *   VOP2 帧缓冲 /dev/fb0    ->  LVGL 显示
 *   FT8756 触摸 /dev/input0 ->  LVGL 输入
 *
 * 四页：
 *
 *   AMP      双核实时状态。数字全部来自 rk3576_rptun_getstat()，也就是
 *            openvela 这一侧 rptun 驱动自己的计数器，不是写死的样例数据。
 *            带一个门铃自检按钮（正例 + 反例）。
 *   DEVICES  设备节点在不在。能安全自检的那几个给按钮，危险的明确不给
 *            （理由写在 g_devices[] 旁边）。
 *   LIVE     堆和门铃速率两条曲线，250ms 一格，**只在这一页可见时刷新**。
 *   ABOUT    分辨率、构建信息，以及原来那个触摸圆点 —— 它一个动作同时
 *            证明显示和输入两条链路，留着。
 *
 * ★ 界面文字为什么是英文
 *
 *   LVGL 内置的 lv_font_simsun_16_cjk 的字符集是从一份日/繁混合的符号表
 *   生成的，简体常用字缺得厉害 —— 实测「双核 设 门铃 检 握 发 收 态 备」
 *   这些全都不在里面，画出来是空白方块。要中文界面就得自己用 lv_font_conv
 *   生成子集字库（需要 node.js 工具链），或者 TINY_TTF 内嵌一份几 MB 的
 *   中文 TTF —— 两者都为了"标签好看"付出与收益不相称的代价。
 *
 *   所以界面用短英文标签，解释留在注释、串口日志和文档里。这是个明确的
 *   取舍，不是疏忽。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <inttypes.h>
#include <malloc.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <nuttx/video/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>
#include <jpeglib.h>
#include <cJSON.h>
#include <velaclaw/client.h>

#include <arch/board/board.h>

#ifdef CONFIG_RK3576_RPTUN
#  include <arch/chip/amp.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define UI_BG        0x0e1116
#define UI_CARD      0x181d25
#define UI_ACCENT    0x4fc3f7
#define UI_OK        0x66bb6a
#define UI_BAD       0xef5350
#define UI_DIM       0x8a96a3

#define CHART_POINTS 60          /* 60 x 250ms = 15 秒窗口 */
#define TICK_MS      250

/* 门铃自检借用的 group。必须是 rptun **没占用**的那个，否则自检会打乱
 * 正在跑的握手状态机。rptun 用 group3（收）/group0（发），5 是空的。
 */

#define SELFTEST_GROUP 5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct devrow_s
{
  const char *label;
  const char *path;
  const char *note;      /* 不在时给出的一句解释，NULL 表示没什么好说的 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 设备清单。
 *
 * ★ 为什么没有"点一下就跑 xTS 自检"
 *
 *   这块板上有两组已知会把机器带走的用例，都记在 docs/ 里：
 *     - cmocka_driver_block 从 0 扇区开始写，对着 /dev/mmcsd0 跑等于毁
 *       eMMC 上的 GPT 和 bootloader；
 *     - xTS 1.3.7 的 spidev/i2cdev 用例会把板子挂死，只能断电恢复。
 *   仪表盘上一个误触就触发这种事，代价和收益完全不成比例。所以这里只
 *   展示"节点在不在"，真正的自检留在 nsh 里手工跑 —— 除了 mailbox 那个，
 *   它是纯软件回环，不碰任何存储和外部总线。
 */

static const struct devrow_s g_devices[] =
{
  { "framebuffer",  "/dev/fb0",          "U-Boot 没点亮显示链路"        },
  { "touch",        "/dev/input0",       "FT8756 未探到"                },
  { "console",      "/dev/console",      NULL                           },
  { "rpmsg link",   "/dev/rpmsg/linux",  "Linux 侧还没建通道"           },
  { "eMMC",         "/dev/mmcsd0",       "amp-dual 下存储默认关闭"      },
  { "TF card",      "/dev/mmcsd1",       "没插卡，或存储关闭"           },
  { "I2C bus 1",    "/dev/i2c1",         NULL                           },
  { "SPI bus 0",    "/dev/spi0",         NULL                           },
  { "GPIO out",     "/dev/gpio2",        NULL                           },
  { "GPIO in",      "/dev/gpio3",        NULL                           },
  { "watchdog",     "/dev/watchdog0",    NULL                           },
  { "video in",     "/dev/video0",       "CIF/CSI 未使能"               },
  { "random",       "/dev/urandom",      NULL                           },
};

static lv_obj_t  *g_tabview;
static lv_obj_t  *g_tab_live;

/* AMP 页 */

static lv_obj_t  *g_amp_badges[3];
static lv_obj_t  *g_amp_values[5];
static lv_obj_t  *g_amp_selftest;

/* DEVICES 页 */

static lv_obj_t  *g_dev_marks[sizeof(g_devices) / sizeof(g_devices[0])];
static lv_obj_t  *g_dev_hint;

/* LIVE 页 */

static lv_obj_t        *g_chart_heap;
static lv_chart_series_t *g_ser_heap;
static lv_obj_t        *g_chart_kick;
static lv_chart_series_t *g_ser_kick;
static lv_obj_t        *g_live_stats;

/* ABOUT 页 */

static lv_obj_t  *g_dot;
static lv_obj_t  *g_touch_label;
static unsigned   g_touch_count;

static uint32_t   g_last_kicks;
static time_t     g_t0;

/* Camera snapshots are captured by the same V4L2 tool used to verify the
 * agent camera path.  Only the UI thread touches LVGL objects.
 */

#define UI_CAMERA_FILE "/tmp/k7-ui-camera.jpg"
#define UI_AGENT_TIMEOUT_S 240
static lv_obj_t     *g_camera_image;
static lv_obj_t     *g_camera_status;
static lv_obj_t     *g_camera_button;
static lv_draw_buf_t *g_camera_frame;
static pid_t         g_camera_pid = -1;
static lv_obj_t     *g_agent_status;
static velaclaw_client_t *g_agent_client;
static pthread_mutex_t g_agent_lock = PTHREAD_MUTEX_INITIALIZER;
static char          g_agent_reply[512];
static bool          g_agent_reply_ready;
static bool          g_agent_busy;
static time_t        g_agent_started;
static lv_obj_t     *g_guard_switch;
static lv_obj_t     *g_guard_state;
static bool          g_guard_enabled;
static bool          g_guard_request;
static time_t        g_guard_next;
static int           g_guard_keys = -1;
static int           g_guard_cup = -1;

struct ui_jpeg_error_s
{
  struct jpeg_error_mgr pub;
  jmp_buf jump;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool path_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static lv_obj_t *card_create(lv_obj_t *parent, const char *title)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  if (title != NULL)
    {
      lbl = lv_label_create(card);
      lv_label_set_text(lbl, title);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ACCENT), 0);
    }

  return card;
}

/* 一行 "名字 ....... 值"，返回值那一半，方便之后 set_text。 */

static lv_obj_t *kv_create(lv_obj_t *parent, const char *key)
{
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_t *k;
  lv_obj_t *v;

  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  k = lv_label_create(row);
  lv_label_set_text(k, key);
  lv_obj_set_style_text_color(k, lv_color_hex(UI_DIM), 0);

  v = lv_label_create(row);
  lv_label_set_text(v, "-");
  lv_obj_set_style_text_color(v, lv_color_white(), 0);

  return v;
}

static void badge_set(lv_obj_t *badge, const char *text, bool good)
{
  lv_label_set_text(badge, text);
  lv_obj_set_style_bg_color(badge,
                            lv_color_hex(good ? UI_OK : UI_BAD), 0);
}

static lv_obj_t *badge_create(lv_obj_t *parent)
{
  lv_obj_t *b = lv_label_create(parent);

  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(UI_BAD), 0);
  lv_obj_set_style_text_color(b, lv_color_black(), 0);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_pad_all(b, 4, 0);
  lv_label_set_text(b, "?");

  return b;
}

/****************************************************************************
 * AMP 页
 ****************************************************************************/

#ifdef CONFIG_RK3576_RPTUN
static void selftest_cb(lv_event_t *e)
{
  uint32_t cmd = 0;
  uint32_t data = 0;
  int pos;
  int neg;

  (void)e;

  lv_label_set_text(g_amp_selftest, "running...");
  lv_refr_now(NULL);

  /* 正例：自己给自己按门铃，必须收到原样的 cmd/data。 */

  pos = rk3576_mailbox_selftest(SELFTEST_GROUP, 0x5a5a5a5a, 0xa5a5a5a5,
                                &cmd, &data);

  /* 反例：只写 CMD 不触发，必须超时。
   *
   * 没有这一条，正例通过也说明不了什么 —— 如果读到的是别处留下的残留
   * 状态，正例一样"通过"。反例失败（也就是居然收到了）才是真问题。
   */

  neg = rk3576_mailbox_selftest_notrigger(SELFTEST_GROUP, NULL, NULL);

  if (pos == 0 && cmd == 0x5a5a5a5a && data == 0xa5a5a5a5 && neg != 0)
    {
      lv_label_set_text_fmt(g_amp_selftest,
                            LV_SYMBOL_OK " pass  rx %08" PRIx32
                            "/%08" PRIx32 ", no-trigger timed out",
                            cmd, data);
      lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_OK), 0);
    }
  else
    {
      lv_label_set_text_fmt(g_amp_selftest,
                            LV_SYMBOL_CLOSE " fail  pos=%d rx %08" PRIx32
                            "/%08" PRIx32 "  neg=%d", pos, cmd, data, neg);
      lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_BAD), 0);
    }
}
#endif

static void amp_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *row;
  int i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "openvela A53 x4   <->   Linux A72 x4");

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, 0);

  for (i = 0; i < 3; i++)
    {
      g_amp_badges[i] = badge_create(row);
    }

  card = card_create(tab, "doorbell");
  g_amp_values[0] = kv_create(card, "kicks RX");
  g_amp_values[1] = kv_create(card, "kicks TX");
  g_amp_values[2] = kv_create(card, "TX busy");
  g_amp_values[3] = kv_create(card, "last cmd");
  g_amp_values[4] = kv_create(card, "last data");

  card = card_create(tab, "shared memory (must match Linux DTS)");
#ifdef CONFIG_RK3576_RPTUN
  lv_label_set_text_fmt(kv_create(card, "vring0 ->Linux"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_VRING0_DA,
                        (long)RK3576_RPTUN_VRING_SIZE / 1024);
  lv_label_set_text_fmt(kv_create(card, "vring1 ->vela"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_VRING1_DA,
                        (long)RK3576_RPTUN_VRING_SIZE / 1024);
  lv_label_set_text_fmt(kv_create(card, "buffer pool"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_POOL_DA,
                        (long)RK3576_RPTUN_POOL_LEN / 1024);
#else
  lv_label_set_text(kv_create(card, "rptun"), "not built in");
#endif

  card = card_create(tab, "mailbox selftest (group 5, loopback only)");
  g_amp_selftest = lv_label_create(card);
  lv_label_set_text(g_amp_selftest, "not run");
  lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_DIM), 0);
  lv_label_set_long_mode(g_amp_selftest, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_amp_selftest, LV_PCT(100));

#ifdef CONFIG_RK3576_RPTUN
  {
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_t *lbl = lv_label_create(btn);

    lv_label_set_text(lbl, LV_SYMBOL_PLAY "  run selftest");
    lv_obj_add_event_cb(btn, selftest_cb, LV_EVENT_CLICKED, NULL);
  }
#endif
}

static void amp_refresh(void)
{
#ifdef CONFIG_RK3576_RPTUN
  struct rk3576_rptun_stat_s st;

  if (rk3576_rptun_getstat(&st) < 0)
    {
      badge_set(g_amp_badges[0], "rptun ?", false);
      return;
    }

  badge_set(g_amp_badges[0],
            st.registered ? "rptun up" : "rptun down", st.registered);
  badge_set(g_amp_badges[1],
            st.driver_ok ? "handshake ok" : "no handshake", st.driver_ok);
  badge_set(g_amp_badges[2],
            path_exists("/dev/rpmsg/linux") ? "/dev/rpmsg/linux"
                                            : "no rpmsg node",
            path_exists("/dev/rpmsg/linux"));

  lv_label_set_text_fmt(g_amp_values[0], "%" PRIu32, st.kicks_rx);
  lv_label_set_text_fmt(g_amp_values[1], "%" PRIu32, st.kicks_tx);
  lv_label_set_text_fmt(g_amp_values[2], "%" PRIu32, st.tx_busy);
  lv_label_set_text_fmt(g_amp_values[3], "%08" PRIx32, st.last_cmd);
  lv_label_set_text_fmt(g_amp_values[4], "%08" PRIx32, st.last_data);
#else
  badge_set(g_amp_badges[0], "rptun not built", false);
#endif
}

/****************************************************************************
 * DEVICES 页
 ****************************************************************************/

static void dev_refresh(void)
{
  int missing = 0;
  size_t i;

  for (i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
    {
      bool ok = path_exists(g_devices[i].path);

      lv_label_set_text(g_dev_marks[i],
                        ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_color(g_dev_marks[i],
                                  lv_color_hex(ok ? UI_OK : UI_BAD), 0);
      if (!ok)
        {
          missing++;
        }
    }

  lv_label_set_text_fmt(g_dev_hint, "%d of %d present",
                        (int)(sizeof(g_devices) / sizeof(g_devices[0]))
                        - missing,
                        (int)(sizeof(g_devices) / sizeof(g_devices[0])));
}

static void dev_refresh_cb(lv_event_t *e)
{
  (void)e;
  dev_refresh();
}

static void dev_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *btn;
  size_t i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "device nodes");

  for (i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
    {
      lv_obj_t *row = lv_obj_create(card);
      lv_obj_t *name;

      lv_obj_remove_style_all(row);
      lv_obj_set_width(row, LV_PCT(100));
      lv_obj_set_height(row, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

      name = lv_label_create(row);
      lv_label_set_text_fmt(name, "%s  %s",
                            g_devices[i].label, g_devices[i].path);
      lv_obj_set_style_text_color(name, lv_color_white(), 0);

      g_dev_marks[i] = lv_label_create(row);
    }

  g_dev_hint = lv_label_create(card);
  lv_obj_set_style_text_color(g_dev_hint, lv_color_hex(UI_DIM), 0);

  btn = lv_button_create(card);
  lv_obj_add_event_cb(btn, dev_refresh_cb, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(btn), LV_SYMBOL_REFRESH "  rescan");

  dev_refresh();
}

/****************************************************************************
 * LIVE 页
 ****************************************************************************/

static int task_count(void)
{
  DIR *d = opendir("/proc");
  struct dirent *ent;
  int n = 0;

  if (d == NULL)
    {
      return -1;
    }

  while ((ent = readdir(d)) != NULL)
    {
      if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9')
        {
          n++;
        }
    }

  closedir(d);
  return n;
}

static void live_build(lv_obj_t *tab)
{
  lv_obj_t *card;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "free heap (KiB)");
  g_chart_heap = lv_chart_create(card);
  lv_obj_set_width(g_chart_heap, LV_PCT(100));
  lv_obj_set_height(g_chart_heap, 120);
  lv_chart_set_point_count(g_chart_heap, CHART_POINTS);
  lv_chart_set_update_mode(g_chart_heap, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(g_chart_heap, 3, 0);
  g_ser_heap = lv_chart_add_series(g_chart_heap, lv_color_hex(UI_ACCENT),
                                   LV_CHART_AXIS_PRIMARY_Y);

  card = card_create(tab, "doorbells per second");
  g_chart_kick = lv_chart_create(card);
  lv_obj_set_width(g_chart_kick, LV_PCT(100));
  lv_obj_set_height(g_chart_kick, 120);
  lv_chart_set_point_count(g_chart_kick, CHART_POINTS);
  lv_chart_set_update_mode(g_chart_kick, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(g_chart_kick, 3, 0);
  lv_chart_set_range(g_chart_kick, LV_CHART_AXIS_PRIMARY_Y, 0, 20);
  g_ser_kick = lv_chart_add_series(g_chart_kick, lv_color_hex(UI_OK),
                                   LV_CHART_AXIS_PRIMARY_Y);

  card = card_create(tab, "counters");
  g_live_stats = lv_label_create(card);
  lv_obj_set_style_text_color(g_live_stats, lv_color_white(), 0);
  lv_label_set_text(g_live_stats, "-");
}

static void live_refresh(void)
{
  struct mallinfo mi = mallinfo();
  uint32_t kicks = 0;
  int32_t rate = 0;
  int tasks = task_count();

#ifdef CONFIG_RK3576_RPTUN
  struct rk3576_rptun_stat_s st;

  if (rk3576_rptun_getstat(&st) == 0)
    {
      kicks = st.kicks_rx + st.kicks_tx;
    }
#endif

  rate = (int32_t)(kicks - g_last_kicks) * (1000 / TICK_MS);
  g_last_kicks = kicks;

  lv_chart_set_next_value(g_chart_heap, g_ser_heap,
                          (int32_t)(mi.fordblks / 1024));
  lv_chart_set_next_value(g_chart_kick, g_ser_kick, rate);

  lv_label_set_text_fmt(g_live_stats,
                        "heap  %u / %u KiB used\n"
                        "tasks %d\n"
                        "up    %lu s\n"
                        "kicks %" PRIu32 " total, %" PRId32 "/s",
                        (unsigned)(mi.uordblks / 1024),
                        (unsigned)(mi.arena / 1024),
                        tasks,
                        (unsigned long)(time(NULL) - g_t0),
                        kicks, rate);
}

/****************************************************************************
 * CAMERA / DESK pages
 ****************************************************************************/

/****************************************************************************
 * 大图缓冲从系统堆分配
 *
 * ★ lv_draw_buf_create() 走 LVGL 自己的内存池，本配置只有 256KB
 *   （CONFIG_LV_MEM_SIZE_KILOBYTES）。一张 640x360 的 ARGB8888 就要
 *   921KB，必然分配失败 —— 表现是点"打开相机"没有任何反应、拍照后
 *   "JPEG decode failed"。系统堆还有几十 MB，图像数据放那边，
 *   只把描述结构交给 LVGL。
 ****************************************************************************/

static lv_draw_buf_t *ui_draw_buf_create(uint32_t w, uint32_t h,
                                         lv_color_format_t cf)
{
  lv_draw_buf_t *buf = calloc(1, sizeof(*buf));
  uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
  size_t size = (size_t)stride * h;
  uint8_t *data = memalign(LV_DRAW_BUF_ALIGN, size);

  if (buf == NULL || data == NULL ||
      lv_draw_buf_init(buf, w, h, cf, stride,
                       data, size) != LV_RESULT_OK)
    {
      free(buf);
      free(data);
      syslog(LOG_ERR, "界面: 图像缓冲 %ux%u 分配失败\n",
             (unsigned)w, (unsigned)h);
      return NULL;
    }

  memset(data, 0, size);
  return buf;
}

static void ui_draw_buf_destroy(lv_draw_buf_t *buf)
{
  if (buf != NULL)
    {
      lv_image_cache_drop(buf);
      free(buf->data);
      free(buf);
    }
}

static void ui_jpeg_error(j_common_ptr info)
{
  struct ui_jpeg_error_s *err = (struct ui_jpeg_error_s *)info->err;
  longjmp(err->jump, 1);
}

static lv_draw_buf_t *camera_decode(const char *path)
{
  struct jpeg_decompress_struct jpeg;
  struct ui_jpeg_error_s error;
  lv_draw_buf_t *frame = NULL;
  FILE *file;
  uint8_t *row = NULL;
  bool created = false;
  unsigned int x;

  file = fopen(path, "rb");
  if (file == NULL)
    {
      return NULL;
    }

  memset(&jpeg, 0, sizeof(jpeg));
  jpeg.err = jpeg_std_error(&error.pub);
  error.pub.error_exit = ui_jpeg_error;
  if (setjmp(error.jump) != 0)
    {
      goto fail;
    }

  jpeg_create_decompress(&jpeg);
  created = true;
  jpeg_stdio_src(&jpeg, file);
  jpeg_read_header(&jpeg, TRUE);
  jpeg.scale_num = 1;
  jpeg.scale_denom = 2;
  jpeg.out_color_space = JCS_RGB;
  jpeg_start_decompress(&jpeg);

  if (jpeg.output_width == 0 || jpeg.output_height == 0 ||
      jpeg.output_width > 1280 || jpeg.output_height > 720 ||
      jpeg.output_components != 3)
    {
      goto fail;
    }

  frame = ui_draw_buf_create(jpeg.output_width, jpeg.output_height,
                             LV_COLOR_FORMAT_ARGB8888);
  row = malloc(jpeg.output_width * 3);
  if (frame == NULL || row == NULL)
    {
      goto fail;
    }

  while (jpeg.output_scanline < jpeg.output_height)
    {
      JSAMPROW scanline = row;
      uint32_t *pixels = (uint32_t *)
        lv_draw_buf_goto_xy(frame, 0, jpeg.output_scanline);

      if (jpeg_read_scanlines(&jpeg, &scanline, 1) != 1)
        {
          goto fail;
        }

      for (x = 0; x < jpeg.output_width; x++)
        {
          pixels[x] = 0xff000000u |
                      ((uint32_t)row[x * 3] << 16) |
                      ((uint32_t)row[x * 3 + 1] << 8) |
                      row[x * 3 + 2];
        }
    }

  jpeg_finish_decompress(&jpeg);
  jpeg_destroy_decompress(&jpeg);
  free(row);
  fclose(file);
  return frame;

fail:
  if (created)
    {
      jpeg_destroy_decompress(&jpeg);
    }

  if (frame != NULL)
    {
      ui_draw_buf_destroy(frame);
    }

  free(row);
  fclose(file);
  return NULL;
}

static void camera_start(void)
{
  char *argv[] =
    {
      "v4l2cap", "1280", "720", UI_CAMERA_FILE, NULL
    };
  int ret;

  if (g_camera_pid > 0)
    {
      return;
    }

  unlink(UI_CAMERA_FILE);
  ret = posix_spawn(&g_camera_pid, "v4l2cap", NULL, NULL, argv, NULL);
  if (ret != 0)
    {
      g_camera_pid = -1;
      lv_label_set_text_fmt(g_camera_status,
                            "Cannot start camera capture: %d", ret);
      return;
    }

  lv_obj_add_state(g_camera_button, LV_STATE_DISABLED);
  lv_label_set_text(g_camera_status, "Capturing desk image...");
}

/****************************************************************************
 * 相机页：全屏实时画面
 *
 * ★ 线程分工
 *
 *   取帧 + 缩放在后台线程里做（kickpi_camera_live_frame 会阻塞到下一帧，
 *   约 33ms），写进 g_live_back；写完在锁里和 g_live_ready 交换指针。
 *   LVGL 只在自己的线程里动对象：定时器发现有新帧，就把 g_live_ready
 *   拷进图片用的 draw_buf 再 invalidate。这样 LVGL 渲染时读的那块内存
 *   永远不会被后台线程改写。
 *
 * ★ 和拍照、agent 互斥
 *
 *   预览、v4l2cap、agent 的 camera_capture 用的是同一个 CIF。拍照和问
 *   agent 之前都先关掉相机页（停流），板级那边也会对后来者返回 -EBUSY。
 ****************************************************************************/

/* 帧率诊断：各段耗时累计（微秒），相机页开着时每 3 秒打一行 */

static uint64_t g_st_grab_us;
static uint32_t g_st_grab_n;
static uint64_t g_st_loop_us;
static uint32_t g_st_loop_n;
static uint64_t g_st_flush_us;
static uint32_t g_st_flush_n;
static uint32_t g_st_frames_n;
static uint64_t g_st_t0;

static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

#define LIVE_W        640
#define LIVE_H        360
#define LIVE_TIMER_MS 30

static lv_obj_t      *g_live_page;
static lv_obj_t      *g_live_img;
static lv_obj_t      *g_live_info;
static lv_draw_buf_t *g_live_buf;
static lv_timer_t    *g_live_timer;
static uint32_t      *g_live_back;
static uint32_t      *g_live_ready;
static bool           g_live_new;
static volatile bool  g_live_run;
static bool           g_live_thread_ok;
static pthread_t      g_live_thread;
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static int            g_live_err;
static unsigned       g_live_frames;
static unsigned       g_live_shown;
static time_t         g_live_t0;

static void *live_thread(void *arg)
{
  (void)arg;

  while (g_live_run)
    {
      uint64_t t0 = now_us();
      int ret = kickpi_camera_live_frame(g_live_back, LIVE_W, LIVE_H, 500);

      g_st_grab_us += now_us() - t0;
      g_st_grab_n++;

      pthread_mutex_lock(&g_live_lock);
      if (ret == OK)
        {
          uint32_t *t = g_live_ready;

          g_live_ready = g_live_back;
          g_live_back = t;
          g_live_new = true;
          g_live_frames++;
        }
      else
        {
          g_live_err = ret;
        }

      pthread_mutex_unlock(&g_live_lock);

      if (ret != OK && ret != -ETIMEDOUT)
        {
          break;
        }
    }

  return NULL;
}

static void live_timer_cb(lv_timer_t *t)
{
  bool fresh = false;
  int err;

  (void)t;

  pthread_mutex_lock(&g_live_lock);
  if (g_live_new)
    {
      memcpy(g_live_buf->data, g_live_ready,
             (size_t)LIVE_W * LIVE_H * 4);
      g_live_new = false;
      fresh = true;
    }

  err = g_live_err;
  g_live_err = 0;
  pthread_mutex_unlock(&g_live_lock);

  if (g_st_t0 == 0)
    {
      g_st_t0 = now_us();
    }
  else if (now_us() - g_st_t0 >= 3000000)
    {
      syslog(LOG_INFO, "界面: 3 秒内 取帧 %u 次 均 %u ms | 循环 %u 次 均 %u ms"
             " | 刷屏 %u 次 均 %u ms | 显示新帧 %u\n",
             g_st_grab_n,
             g_st_grab_n ? (unsigned)(g_st_grab_us / g_st_grab_n / 1000) : 0,
             g_st_loop_n,
             g_st_loop_n ? (unsigned)(g_st_loop_us / g_st_loop_n / 1000) : 0,
             g_st_flush_n,
             g_st_flush_n ? (unsigned)(g_st_flush_us / g_st_flush_n / 1000) : 0,
             g_st_frames_n);
      g_st_grab_us = g_st_loop_us = g_st_flush_us = 0;
      g_st_grab_n = g_st_loop_n = g_st_flush_n = g_st_frames_n = 0;
      g_st_t0 = now_us();
    }

  if (fresh)
    {
      g_st_frames_n++;
      g_live_shown++;
      lv_image_cache_drop(g_live_buf);
      lv_obj_invalidate(g_live_img);
    }

  if (err != 0 && err != -ETIMEDOUT)
    {
      lv_label_set_text_fmt(g_live_info, "Camera stopped: %d", err);
    }
  else if (fresh && (g_live_shown % 15) == 1)
    {
      time_t dt = time(NULL) - g_live_t0;
      unsigned fps = dt > 0 ? (unsigned)(g_live_shown / dt) : 0;

      lv_label_set_text_fmt(g_live_info, "Live  %u frames  %u fps",
                            g_live_shown, fps);
      if ((g_live_shown % 150) == 1 && dt > 0)
        {
          syslog(LOG_INFO, "界面: 相机页 采集 %u 帧 显示 %u 帧 / %ld 秒\n",
                 g_live_frames, g_live_shown, (long)dt);
        }
    }
}

static void live_stop(void)
{
  if (g_live_timer != NULL)
    {
      lv_timer_delete(g_live_timer);
      g_live_timer = NULL;
    }

  g_live_run = false;
  if (g_live_thread_ok)
    {
      pthread_join(g_live_thread, NULL);
      g_live_thread_ok = false;
    }

  kickpi_camera_live_stop();
}

static void live_page_close(void)
{
  if (g_live_page == NULL ||
      lv_obj_has_flag(g_live_page, LV_OBJ_FLAG_HIDDEN))
    {
      return;
    }

  live_stop();
  lv_obj_add_flag(g_live_page, LV_OBJ_FLAG_HIDDEN);
}

static void live_close_cb(lv_event_t *event)
{
  (void)event;
  live_page_close();
}

static void live_snapshot_cb(lv_event_t *event)
{
  (void)event;

  /* 先停流再拍：v4l2cap 走 /dev/video0，和预览抢同一个 CIF */

  live_page_close();
  camera_start();
}

static bool live_page_create(void)
{
  lv_obj_t *bar;
  lv_obj_t *btn;

  /* XRGB：画面不透明，LVGL 直接拷贝，不做逐像素 alpha 混合 */

  g_live_buf = ui_draw_buf_create(LIVE_W, LIVE_H, LV_COLOR_FORMAT_XRGB8888);
  g_live_back = malloc((size_t)LIVE_W * LIVE_H * 4);
  g_live_ready = malloc((size_t)LIVE_W * LIVE_H * 4);
  if (g_live_buf == NULL || g_live_back == NULL || g_live_ready == NULL)
    {
      if (g_live_buf != NULL)
        {
          ui_draw_buf_destroy(g_live_buf);
          g_live_buf = NULL;
        }

      free(g_live_back);
      free(g_live_ready);
      g_live_back = NULL;
      g_live_ready = NULL;
      return false;
    }

  g_live_page = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_live_page, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_live_page, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_live_page, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_live_page, 0, 0);
  lv_obj_set_style_radius(g_live_page, 0, 0);
  lv_obj_set_style_pad_all(g_live_page, 12, 0);
  lv_obj_remove_flag(g_live_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g_live_page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_live_page, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(g_live_page, 16, 0);

  lv_obj_set_style_text_color(lv_label_create(g_live_page),
                              lv_color_hex(UI_ACCENT), 0);
  lv_label_set_text(lv_obj_get_child(g_live_page, 0), "Desk camera");

  g_live_img = lv_image_create(g_live_page);
  lv_image_set_src(g_live_img, g_live_buf);

  g_live_info = lv_label_create(g_live_page);
  lv_obj_set_style_text_color(g_live_info, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(g_live_info, "Starting camera...");

  bar = lv_obj_create(g_live_page);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  btn = lv_button_create(bar);
  lv_obj_set_size(btn, 200, 72);
  lv_label_set_text(lv_label_create(btn), "Snapshot");
  lv_obj_center(lv_obj_get_child(btn, 0));
  lv_obj_add_event_cb(btn, live_snapshot_cb, LV_EVENT_CLICKED, NULL);

  btn = lv_button_create(bar);
  lv_obj_set_size(btn, 200, 72);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BAD), 0);
  lv_label_set_text(lv_label_create(btn), "Close");
  lv_obj_center(lv_obj_get_child(btn, 0));
  lv_obj_add_event_cb(btn, live_close_cb, LV_EVENT_CLICKED, NULL);

  return true;
}

static void live_page_open(void)
{
  int ret;

  if (g_camera_pid > 0)
    {
      lv_label_set_text(g_camera_status, "Snapshot in progress, wait...");
      return;
    }

  if (g_live_page == NULL && !live_page_create())
    {
      lv_label_set_text(g_camera_status, "Camera page: out of memory");
      syslog(LOG_ERR, "界面: 相机页创建失败\n");
      return;
    }

  lv_obj_remove_flag(g_live_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_live_page);

  ret = kickpi_camera_live_start();
  syslog(LOG_INFO, "界面: 相机页打开，预览启动 %d\n", ret);
  if (ret < 0)
    {
      lv_label_set_text_fmt(g_live_info,
                            ret == -EBUSY ? "Camera busy (%d), try again" :
                            "Camera start failed: %d", ret);
      return;
    }

  g_live_new = false;
  g_live_err = 0;
  g_live_frames = 0;
  g_live_shown = 0;
  g_live_t0 = time(NULL);
  g_live_run = true;
  /* 取帧线程低于界面：画不画得出来由 LVGL 决定，取帧只管有就交 */

  {
    pthread_attr_t attr;
    struct sched_param sp;

    pthread_attr_init(&attr);
    sp.sched_priority = sched_get_priority_min(SCHED_RR) +
                        (CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_PRIORITY -
                         sched_get_priority_min(SCHED_RR)) / 2;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    ret = pthread_create(&g_live_thread, &attr, live_thread, NULL);
    pthread_attr_destroy(&attr);
  }

  if (ret != 0)
    {
      g_live_run = false;
      kickpi_camera_live_stop();
      lv_label_set_text(g_live_info, "Cannot start preview thread");
      return;
    }

  g_live_thread_ok = true;
  pthread_setname_np(g_live_thread, "ui_camera");
  lv_label_set_text(g_live_info, "Live");
  g_live_timer = lv_timer_create(live_timer_cb, LIVE_TIMER_MS, NULL);
}

static void camera_click_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  lv_indev_t *indev = lv_indev_active();
  lv_point_t pt = { 0, 0 };

  if (indev != NULL)
    {
      lv_indev_get_point(indev, &pt);
    }

  syslog(LOG_INFO, "界面: Open camera 事件 %s (%d,%d)\n",
         code == LV_EVENT_PRESSED ? "PRESSED" :
         code == LV_EVENT_RELEASED ? "RELEASED" :
         code == LV_EVENT_PRESS_LOST ? "PRESS_LOST" :
         code == LV_EVENT_CLICKED ? "CLICKED" : "?",
         (int)pt.x, (int)pt.y);

  if (code == LV_EVENT_CLICKED)
    {
      live_page_open();
    }
}

static void camera_poll(void)
{
  lv_draw_buf_t *next;
  int status;
  pid_t done;

  if (g_camera_pid <= 0)
    {
      return;
    }

  done = waitpid(g_camera_pid, &status, WNOHANG);
  if (done == 0)
    {
      return;
    }

  g_camera_pid = -1;
  lv_obj_remove_state(g_camera_button, LV_STATE_DISABLED);
  if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
      lv_label_set_text(g_camera_status, "Capture failed; check /dev/video0");
      return;
    }

  next = camera_decode(UI_CAMERA_FILE);
  if (next == NULL)
    {
      lv_label_set_text(g_camera_status, "JPEG decode failed");
      return;
    }

  lv_image_set_src(g_camera_image, NULL);
  if (g_camera_frame != NULL)
    {
      ui_draw_buf_destroy(g_camera_frame);
    }

  g_camera_frame = next;
  lv_image_set_src(g_camera_image, g_camera_frame);
  lv_label_set_text(g_camera_status,
                    "Snapshot ready. Ask the assistant for live analysis.");
}

static void agent_reply_cb(int status, const char *reply, void *cookie)
{
  (void)cookie;
  pthread_mutex_lock(&g_agent_lock);
  if (status == 0 && reply != NULL)
    {
      snprintf(g_agent_reply, sizeof(g_agent_reply), "%s", reply);
    }
  else
    {
      snprintf(g_agent_reply, sizeof(g_agent_reply),
               "Agent request failed: %d", status);
    }

  g_agent_reply_ready = true;
  pthread_mutex_unlock(&g_agent_lock);
}

static void agent_ask(const char *question)
{
  velaclaw_ask_req_t request;
  int ret;

  if (g_agent_busy)
    {
      return;
    }

  /* agent 的 camera_capture 走 /dev/video0，和预览抢 CIF */

  live_page_close();

  if (g_agent_client == NULL)
    {
      g_agent_client = velaclaw_client_open("k7-desk-ui");
      if (g_agent_client == NULL)
        {
          lv_label_set_text(g_agent_status,
                            "Agent offline. Start ai_agent first.");
          return;
        }
    }

  request.text = question;
  /* mimo-v2.5 是推理模型：拍照 + 看图 + 作答三轮，实测第二轮就要 99s */

  request.timeout_ms = UI_AGENT_TIMEOUT_S * 1000;
  ret = velaclaw_ask(g_agent_client, &request, agent_reply_cb, NULL);
  if (ret < 0)
    {
      lv_label_set_text_fmt(g_agent_status, "Agent request failed: %d",
                            ret);
      return;
    }

  g_agent_busy = true;
  g_agent_started = time(NULL);
  lv_label_set_text(g_agent_status, "Assistant is checking...");
}

static void agent_look_cb(lv_event_t *event)
{
  (void)event;
  agent_ask("Use camera_capture to inspect the current desk image. "
            "Describe only what is visible in this capture. "
            "If capture or vision fails, say so plainly.");
}

static void agent_items_cb(lv_event_t *event)
{
  (void)event;
  agent_ask("Use camera_capture to inspect the current desk image. "
            "Are the keys and cup visible? Say unknown if an item is "
            "obscured or the camera tool fails. Do not guess.");
}

static int guard_value(cJSON *root, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (cJSON_IsBool(item))
    {
      return cJSON_IsTrue(item) ? 1 : 0;
    }

  return -1;
}

static bool guard_apply(const char *reply)
{
  const char *json = strchr(reply, '{');
  cJSON *root;
  int keys;
  int cup;
  bool alert = false;

  if (json == NULL)
    {
      return false;
    }

  root = cJSON_Parse(json);
  if (root == NULL)
    {
      return false;
    }

  keys = guard_value(root, "keys");
  cup = guard_value(root, "cup");
  if (keys >= 0)
    {
      alert |= g_guard_keys == 1 && keys == 0;
      g_guard_keys = keys;
    }

  if (cup >= 0)
    {
      alert |= g_guard_cup == 1 && cup == 0;
      g_guard_cup = cup;
    }

  cJSON_Delete(root);
  lv_label_set_text_fmt(g_guard_state,
                        alert ? "ALERT: a tracked item disappeared"
                              : "Last check: keys=%s, cup=%s",
                        g_guard_keys < 0 ? "unknown" :
                          (g_guard_keys ? "visible" : "missing"),
                        g_guard_cup < 0 ? "unknown" :
                          (g_guard_cup ? "visible" : "missing"));
  return true;
}

static void guard_switch_cb(lv_event_t *event)
{
  g_guard_enabled = lv_obj_has_state(lv_event_get_target(event),
                                     LV_STATE_CHECKED);
  g_guard_next = time(NULL);
  if (!g_guard_enabled)
    {
      g_guard_keys = -1;
      g_guard_cup = -1;
      lv_label_set_text(g_guard_state, "Desk guard is off.");
    }
  else
    {
      lv_label_set_text(g_guard_state, "Desk guard enabled.");
    }
}

static void agent_poll(void)
{
  char reply[sizeof(g_agent_reply)];
  bool ready;

  pthread_mutex_lock(&g_agent_lock);
  ready = g_agent_reply_ready;
  if (ready)
    {
      memcpy(reply, g_agent_reply, sizeof(reply));
      g_agent_reply_ready = false;
    }

  pthread_mutex_unlock(&g_agent_lock);
  if (ready)
    {
      reply[sizeof(reply) - 1] = '\0';
      syslog(LOG_INFO, "界面: agent 回复（%ld 秒）: %s\n",
             (long)(time(NULL) - g_agent_started), reply);
      if (g_guard_request)
        {
          if (!guard_apply(reply))
            {
              lv_label_set_text(g_guard_state,
                                "Check inconclusive; state unchanged.");
            }

          g_guard_request = false;
          g_guard_next = time(NULL) + 60;
        }
      else
        {
          lv_label_set_text(g_agent_status, reply);
        }

      g_agent_busy = false;
    }
  else if (g_agent_busy &&
           time(NULL) - g_agent_started >= UI_AGENT_TIMEOUT_S)
    {
      lv_label_set_text(g_agent_status, "Agent response timed out.");
      g_agent_busy = false;
    }

  if (g_guard_enabled && !g_agent_busy && time(NULL) >= g_guard_next)
    {
      g_guard_request = true;
      agent_ask("Use camera_capture to inspect the desk. Return one JSON "
                "object only: {\"keys\":true|false|null,"
                "\"cup\":true|false|null}. Use null when obscured, "
                "uncertain, or capture fails. Do not guess.");
      if (!g_agent_busy)
        {
          g_guard_request = false;
          g_guard_next = time(NULL) + 10;
        }
    }
}

static void camera_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *button;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "desk camera");
  g_camera_status = lv_label_create(card);
  lv_label_set_text(g_camera_status,
                    "Open camera for live view; Snapshot saves a still.");
  lv_label_set_long_mode(g_camera_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_camera_status, LV_PCT(100));

  g_camera_button = lv_button_create(card);
  lv_label_set_text(lv_label_create(g_camera_button), "Open camera");
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_PRESS_LOST, NULL);

  g_camera_image = lv_image_create(card);
  lv_obj_set_width(g_camera_image, LV_PCT(100));

  card = card_create(tab, "desktop assistant");
  g_agent_status = lv_label_create(card);
  lv_label_set_text(g_agent_status, "Start ai_agent, then ask about the desk.");
  lv_label_set_long_mode(g_agent_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_agent_status, LV_PCT(100));

  button = lv_button_create(card);
  lv_label_set_text(lv_label_create(button), "What is on my desk?");
  lv_obj_add_event_cb(button, agent_look_cb, LV_EVENT_CLICKED, NULL);

  button = lv_button_create(card);
  lv_label_set_text(lv_label_create(button), "Keys and cup?");
  lv_obj_add_event_cb(button, agent_items_cb, LV_EVENT_CLICKED, NULL);

  g_guard_switch = lv_switch_create(card);
  lv_obj_add_event_cb(g_guard_switch, guard_switch_cb,
                      LV_EVENT_VALUE_CHANGED, NULL);
  lv_label_set_text(lv_label_create(card), "Desk guard (check every 60 s)");
  g_guard_state = lv_label_create(card);
  lv_label_set_text(g_guard_state, "Desk guard is off.");
  lv_label_set_long_mode(g_guard_state, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_guard_state, LV_PCT(100));
}

/****************************************************************************
 * ABOUT 页
 ****************************************************************************/

static void touch_event_cb(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_active();
  lv_point_t  p;

  if (indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &p);

  if (lv_event_get_code(e) == LV_EVENT_PRESSED)
    {
      g_touch_count++;
    }

  lv_obj_set_pos(g_dot, p.x - 15, p.y - 15);
  lv_label_set_text_fmt(g_touch_label, "X=%d  Y=%d   presses %u",
                        (int)p.x, (int)p.y, g_touch_count);
}

static void about_build(lv_obj_t *tab)
{
  lv_display_t *disp = lv_display_get_default();
  lv_obj_t *card;
  lv_obj_t *pad;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "board");
  lv_label_set_text(kv_create(card, "SoC"), "Rockchip RK3576");
  lv_label_set_text(kv_create(card, "board"), "KICKPI-K7");
  lv_label_set_text_fmt(kv_create(card, "framebuffer"), "%d x %d",
                        (int)lv_display_get_horizontal_resolution(disp),
                        (int)lv_display_get_vertical_resolution(disp));
  lv_label_set_text(kv_create(card, "built"), __DATE__ " " __TIME__);

  /* 触摸自检。原来这就是整个程序 —— 一个动作同时证明 VOP2 出图和
   * FT8756 上报都通了，比只看日志可靠，所以留着。
   */

  card = card_create(tab, "touch check (drag inside the box)");
  pad = lv_obj_create(card);
  lv_obj_set_width(pad, LV_PCT(100));
  lv_obj_set_height(pad, 220);
  lv_obj_set_style_bg_color(pad, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(pad, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_border_width(pad, 1, 0);
  lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

  g_dot = lv_obj_create(pad);
  lv_obj_set_size(g_dot, 30, 30);
  lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_dot, lv_color_hex(UI_BAD), 0);
  lv_obj_set_style_border_width(g_dot, 0, 0);
  lv_obj_set_pos(g_dot, -100, -100);

  g_touch_label = lv_label_create(card);
  lv_obj_set_style_text_color(g_touch_label, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(g_touch_label, "no touch yet");

  lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pad, touch_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(pad, touch_event_cb, LV_EVENT_PRESSING, NULL);
}

/****************************************************************************
 * ai_agent 开机自启动
 *
 * ★ 为什么由界面来拉起、而且 stdin 接 /dev/null
 *
 *   ai_agent 最后会起一个 CLI 线程，fgets(stdin) 读控制台。开机在后台
 *   跑的话它和 nsh 抢同一个串口，敲的字谁先读到算谁的。把它的 stdin
 *   接到 /dev/null，CLI 线程第一次读就拿到 EOF 退出，其余服务照常运行，
 *   控制台仍归 nsh。界面和 agent 在同一个地址空间，走消息总线通信。
 *
 * ★ Key 不进仓库
 *
 *   /data 是 tmpfs，重启即失，所以每次开机由这里调 set_llm。Key 来自
 *   本地的 k7_agent_key.h（.gitignore 已忽略，模板见
 *   k7_agent_key.h.example）。没有这个文件就只启动 agent、不配后端，
 *   界面上会显示 Agent 请求失败，而不是编进一个假 Key。
 ****************************************************************************/

#if __has_include("k7_agent_key.h")
#  include "k7_agent_key.h"
#endif

extern bool message_bus_ready(void);
extern void cmd_set_llm(int argc, char **argv);
extern void cmd_set_vision_llm(int argc, char **argv);

static void *agent_boot_thread(void *arg)
{
  posix_spawn_file_actions_t fa;
  char *argv[] = { "ai_agent", NULL };
  pid_t pid;
  int ret;
  int i;

  (void)arg;

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  ret = posix_spawn(&pid, "ai_agent", &fa, NULL, argv, NULL);
  posix_spawn_file_actions_destroy(&fa);
  if (ret != 0)
    {
      syslog(LOG_ERR, "界面: 启动 ai_agent 失败 %d\n", ret);
      return NULL;
    }

  for (i = 0; i < 100 && !message_bus_ready(); i++)
    {
      usleep(100 * 1000);
    }

  if (!message_bus_ready())
    {
      syslog(LOG_ERR, "界面: ai_agent 10 秒内没有就绪\n");
      return NULL;
    }

#ifdef K7_AGENT_LLM_KEY
  {
    /* Token Plan 的 Key 只认它自己的地址（token-plan-cn.xiaomimimo.com），
     * 打到预设的 api.xiaomimimo.com 一律 401；模型也只有 mimo-v2.5 系列。
     * 所以地址和模型都跟着 Key 放在本地头文件里。
     */

    char *llm[] = { "set_llm", K7_AGENT_LLM_URL, K7_AGENT_LLM_MODEL,
                    K7_AGENT_LLM_KEY, NULL };
    char *vis[] = { "set_vision_llm", K7_AGENT_LLM_HOST, K7_AGENT_VISION_MODEL,
                    K7_AGENT_LLM_KEY, NULL };

    /* 等 router 初始化完（agent_main 的 P3 在消息总线之后） */

    sleep(2);
    cmd_set_llm(4, llm);
    cmd_set_vision_llm(4, vis);
    syslog(LOG_INFO, "界面: ai_agent 已启动，后端 %s（%s）已配置\n",
           K7_AGENT_LLM_HOST, K7_AGENT_LLM_MODEL);
  }
#else
  syslog(LOG_WARNING, "界面: ai_agent 已启动，但没有 k7_agent_key.h，"
         "未配置 LLM 后端\n");
#endif

  return NULL;
}

static void agent_autostart(void)
{
  pthread_attr_t attr;
  pthread_t tid;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&tid, &attr, agent_boot_thread, NULL) == 0)
    {
      pthread_detach(tid);
    }

  pthread_attr_destroy(&attr);
}

/****************************************************************************
 * 定时刷新
 ****************************************************************************/

static void tick_cb(lv_timer_t *t)
{
  (void)t;

  camera_poll();
  agent_poll();

  /* 串口调试入口：echo 1 > /tmp/k7-open-camera 等同于点 Open camera */

  if (unlink("/tmp/k7-open-camera") == 0)
    {
      live_page_open();
    }

  /* echo 1 > /tmp/k7-ask 等同于点 "What is on my desk?" */

  if (unlink("/tmp/k7-ask") == 0)
    {
      syslog(LOG_INFO, "界面: 串口触发 agent 看桌面\n");
      agent_look_cb(NULL);
    }
  amp_refresh();

  /* LIVE 页不可见时不算、不画。曲线是"看得见才有意义"的东西，后台跑
   * 只会白占 CPU —— 这块板上 LVGL 和 rpmsg 抢的是同几颗核。
   */

  if (lv_tabview_get_tab_active(g_tabview) == 2)
    {
      live_refresh();
    }
  else
    {
      dev_refresh();
    }
}

static void build_ui(void)
{
  lv_obj_t *scr = lv_screen_active();

  lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), LV_PART_MAIN);

  g_tabview = lv_tabview_create(scr);
  lv_tabview_set_tab_bar_size(g_tabview, 48);
  lv_obj_set_size(g_tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_tabview, lv_color_hex(UI_BG), 0);

  amp_build(lv_tabview_add_tab(g_tabview, "AMP"));
  dev_build(lv_tabview_add_tab(g_tabview, "DEV"));
  g_tab_live = lv_tabview_add_tab(g_tabview, "LIVE");
  live_build(g_tab_live);
  camera_build(lv_tabview_add_tab(g_tabview, "DESK"));
  about_build(lv_tabview_add_tab(g_tabview, "ABOUT"));

  lv_timer_create(tick_cb, TICK_MS, NULL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct kickpi_fb_s
{
  int       fd;
  uint8_t  *mem;
  uint32_t  stride;
  uint32_t  xres;
  uint32_t  yres;
  size_t    fblen;
  size_t    drawbytes;
};

static struct kickpi_fb_s g_kfb;

/* ★ 绘制缓冲必须在堆上分配，不能写成静态数组。
 *
 *   第一版写成 static uint32_t g_draw[720*60]，两块共 345KB 全进了 BSS，
 *   镜像从 4993 扇区涨到 5665 扇区。而 U-Boot 的 bootamp 读 FIT 用的是
 *   **写死的扇区数 5120**（cmd/bootamp.c 的编译期宏），于是读进来的是
 *   截断的镜像，校验直接失败：
 *
 *     Verifying Hash Integrity ... sha256 Bad hash: ...
 *     Bad Data Hash / AMP Error: Load loadables, ret=-13
 *
 *   现象是"板子起不来"，但根因既不在代码逻辑也不在 LVGL —— 是镜像超过了
 *   引导器愿意读的长度。运行期的缓冲放堆上，镜像一个字节都不会涨。
 */

/* 360 行：相机页 640x360 的画面一块画完。原来 60 行要分 6 块。
 * 两块共 720x360x4x2 = 2MB，系统堆放得下。
 */

#define KICKPI_DRAW_LINES 360
static uint32_t *g_draw1;
static uint32_t *g_draw2;

static void kickpi_flush_cb(lv_display_t *disp, const lv_area_t *area,
                            uint8_t *px_map)
{
  struct kickpi_fb_s *fb = lv_display_get_driver_data(disp);
  struct fb_area_s    up;
  int32_t             w = lv_area_get_width(area);
  int32_t             y;

  for (y = area->y1; y <= area->y2; y++)
    {
      memcpy(fb->mem + (size_t)y * fb->stride + (size_t)area->x1 * 4,
             px_map + (size_t)(y - area->y1) * w * 4,
             (size_t)w * 4);
    }

  /* ★ 一帧只在最后一块时刷一次。
   *
   *   驱动的 updatearea 每次都把半个帧缓冲（3.6MB）整块刷回 DRAM
   *   （见 rk3576_fb.c 的说明）。原来每块都调一次，一帧画面被分成
   *   几块就刷几次 —— 640x360 的相机画面按 60 行分块，一帧刷 6 次，
   *   帧率被它压着。PARTIAL 模式下帧缓冲只有一块、前几块写进去就在，
   *   最后统一刷一次就够了。
   */

  if (lv_display_flush_is_last(disp))
    {
      uint64_t t0 = now_us();

      up.x = 0;
      up.y = 0;
      up.w = fb->xres;
      up.h = fb->yres;
      ioctl(fb->fd, FBIO_UPDATE, (unsigned long)&up);
      g_st_flush_us += now_us() - t0;
      g_st_flush_n++;
    }

  lv_display_flush_ready(disp);
}

static lv_display_t *kickpi_disp_create(const char *path)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  lv_display_t         *disp;

  g_kfb.fd = open(path, O_RDWR);
  if (g_kfb.fd < 0)
    {
      return NULL;
    }

  memset(&pinfo, 0, sizeof(pinfo));
  if (ioctl(g_kfb.fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(g_kfb.fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      close(g_kfb.fd);
      return NULL;
    }

  g_kfb.xres   = vinfo.xres;
  g_kfb.yres   = vinfo.yres;
  g_kfb.stride = pinfo.stride;
  g_kfb.fblen  = pinfo.fblen;
  g_kfb.mem    = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_FILE, g_kfb.fd, 0);
  if (g_kfb.mem == MAP_FAILED)
    {
      close(g_kfb.fd);
      return NULL;
    }

  {
    size_t bufbytes = (size_t)vinfo.xres * KICKPI_DRAW_LINES * 4;

    g_draw1 = malloc(bufbytes);
    g_draw2 = malloc(bufbytes);
    if (g_draw1 == NULL || g_draw2 == NULL)
      {
        printf("绘制缓冲分配失败 (%zu x2)\n", bufbytes);
        close(g_kfb.fd);
        return NULL;
      }

    g_kfb.drawbytes = bufbytes;
  }

  disp = lv_display_create(vinfo.xres, vinfo.yres);
  if (disp == NULL)
    {
      return NULL;
    }

  lv_display_set_driver_data(disp, &g_kfb);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_flush_cb(disp, kickpi_flush_cb);
  lv_display_set_buffers(disp, g_draw1, g_draw2, g_kfb.drawbytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  printf("\u663e\u793a: %" PRIu32 "x%" PRIu32 " PARTIAL, \u7ed8\u5236\u7f13\u51b2 %d \u884c\n",
         g_kfb.xres, g_kfb.yres, KICKPI_DRAW_LINES);
  return disp;
}

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t    dsc;
  lv_nuttx_result_t result;
  lv_display_t     *disp_self;

  (void)argc;
  (void)argv;

  g_t0 = time(NULL);

  lv_init();

  /* ★ 不用 lv_nuttx_init() 建显示，只借它建触摸。
   *
   *   lv_nuttx_fbdev.c 写死了 LV_DISPLAY_RENDER_MODE_DIRECT，而且只要
   *   驱动报了 yres_virtual = 2*yres 就自动走双缓冲 + FBIOPAN_DISPLAY。
   *   这条路在这块板子上一直不干净：
   *
   *     - DIRECT + 双缓冲要求两块缓冲逐帧同步（LVGL 用
   *       refr_sync_areas() 把上一帧脏区从前缓冲拷到后缓冲），
   *       任何一环对不上，屏上就是"上一帧没清干净" ——
   *       实测滑动时闪烁并出现黑色短线条；
   *     - 翻页的 CFG_DONE 要等 VSYNC 才 latch，而 VOP2 的中断我们
   *       没接，只能轮询 VP1 的 FS 标志。实测那个标志是**粘滞**
   *       的（120ms 里只跳变 3 次，60Hz 本该 ~14 次），拿它当帧
   *       边界并不可靠，k7diag anim 仍跑到 78fps（屏是 60Hz）。
   *
   *   PARTIAL 模式把这一整类问题从根上去掉：LVGL 只往一块很小的
   *   RAM 缓冲里画，flush_cb 负责把它拷进帧缓冲。帧缓冲**始终是
   *   一份完整一致的画面**，没有两块缓冲要同步，也就不需要翻页和
   *   vsync 门控。代价是每帧多一次内存拷贝，对这个仪表盘可以忽略。
   *
   *   这是绝大多数嵌入式 LVGL 的标准接法 —— 我们绕了远路才回到它。
   */

  /* ★ 顺序不能反：先建显示，再建触摸。
   *
   *   lv_indev_create() 会把输入设备绑到**当时的默认显示**上。先建触摸
   *   的话那时还没有显示，indev 的 display 是空的，随后
   *   process_single_touch() 里 lv_indev_get_display() 取到 NULL，
   *   一点屏幕就崩。第一版就是这么写的，板子直接起不来。
   */

  disp_self = kickpi_disp_create("/dev/fb0");
  if (disp_self == NULL)
    {
      printf("建立显示失败\n");
      return -1;
    }

  lv_nuttx_dsc_init(&dsc);
  dsc.fb_path    = NULL;               /* 显示上面已经建好 */
  dsc.input_path = "/dev/input0";

  lv_nuttx_init(&dsc, &result);

  /* ★ lv_nuttx_init() 会把 result 整个重置。
   *
   *   我们的显示是上面自己建的（PARTIAL 模式），而 dsc.fb_path 传的是
   *   NULL，于是 lv_nuttx_init() 里那句 `if (dsc && dsc->fb_path)` 不成立，
   *   它**不建显示**，同时把 result.disp 清成了 NULL —— 先前填进去的指针
   *   被覆盖掉了。板上现象是"显示建好了，紧接着报打不开 (null)"，然后
   *   程序退出、屏幕全黑。
   *
   *   所以自建的 disp 要用局部变量存住，等 lv_nuttx_init() 返回之后再填
   *   回 result，并把输入设备绑上去。
   */

  result.disp = disp_self;

  if (result.indev != NULL)
    {
      lv_indev_set_display(result.indev, result.disp);
    }

  if (result.disp == NULL)
    {
      printf("显示未就绪\n");
      return EXIT_FAILURE;
    }

  if (result.indev == NULL)
    {
      /* 触摸没起来不影响看画面，继续跑并明确说出来，不要让"界面出来了"
       * 掩盖掉输入其实没通。
       */

      printf("警告：打不开 %s —— 触摸不可用，仅显示\n", dsc.input_path);
    }

  printf("LVGL 就绪 %dx%d，触摸 %s\n",
         (int)lv_display_get_horizontal_resolution(result.disp),
         (int)lv_display_get_vertical_resolution(result.disp),
         result.indev != NULL ? "可用" : "不可用");

  build_ui();
  agent_autostart();

#ifdef CONFIG_LV_USE_BUILTIN_MALLOC
  {
    lv_mem_monitor_t monitor;

    lv_mem_monitor(&monitor);
    printf("LVGL heap: used=%zu total=%zu peak=%zu\n",
           monitor.total_size - monitor.free_size,
           monitor.total_size, monitor.max_used);
  }
#endif

  /* openvela 的 LVGL 分支没有 lv_nuttx_run()，循环要自己写。
   * 和 apps/examples/lvgldemo 一样：lv_timer_handler() 返回距离下一个
   * 定时器还有多久，按它睡，最少 1ms。
   */

  for (; ; )
    {
      uint64_t t0 = now_us();
      uint32_t idle = lv_timer_handler();

      g_st_loop_us += now_us() - t0;
      g_st_loop_n++;

      usleep((idle ? idle : 1) * 1000);
    }

  lv_nuttx_deinit(&result);
  lv_deinit();
  return EXIT_SUCCESS;
}
