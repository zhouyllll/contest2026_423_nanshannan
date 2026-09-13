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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>

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
 * 定时刷新
 ****************************************************************************/

static void tick_cb(lv_timer_t *t)
{
  (void)t;

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
  about_build(lv_tabview_add_tab(g_tabview, "ABOUT"));

  lv_timer_create(tick_cb, TICK_MS, NULL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t    dsc;
  lv_nuttx_result_t result;

  (void)argc;
  (void)argv;

  g_t0 = time(NULL);

  lv_init();

  lv_nuttx_dsc_init(&dsc);
  dsc.fb_path    = "/dev/fb0";
  dsc.input_path = "/dev/input0";

  lv_nuttx_init(&dsc, &result);

  if (result.disp == NULL)
    {
      printf("打不开 %s —— 显示未就绪\n", dsc.fb_path);
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

  /* openvela 的 LVGL 分支没有 lv_nuttx_run()，循环要自己写。
   * 和 apps/examples/lvgldemo 一样：lv_timer_handler() 返回距离下一个
   * 定时器还有多久，按它睡，最少 1ms。
   */

  for (; ; )
    {
      uint32_t idle = lv_timer_handler();

      usleep((idle ? idle : 1) * 1000);
    }

  lv_nuttx_deinit(&result);
  lv_deinit();
  return EXIT_SUCCESS;
}
