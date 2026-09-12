/****************************************************************************
 * packages/demos/contest2026_423_kickpi_ui/kickpi_ui_main.c
 *
 * KICKPI-K7 板级 UI 演示 —— 验证 BSP 与 openvela 上层图形栈打通。
 *
 * 这个程序不做复杂界面，只把整条通路跑一遍：
 *
 *   VOP2 帧缓冲 /dev/fb0  ->  LVGL 显示驱动
 *   FT8756 触摸 /dev/input0 -> LVGL 输入设备
 *
 * 屏上给出可见反馈（触摸点跟随手指、计数递增），触摸一下就能同时确认
 * 显示与输入两条链路 —— 比只看日志可靠。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *g_label_pos;
static lv_obj_t *g_dot;
static unsigned  g_touch_count;

/****************************************************************************
 * Private Functions
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

  /* 圆点跟随手指 —— 显示与输入同时得到验证。 */

  lv_obj_set_pos(g_dot, p.x - 15, p.y - 15);
  lv_label_set_text_fmt(g_label_pos, "X=%d  Y=%d\n触摸次数 %u",
                        (int)p.x, (int)p.y, g_touch_count);
}

static void build_ui(void)
{
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *title;
  lv_display_t *disp = lv_display_get_default();

  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

  title = lv_label_create(scr);
  lv_label_set_text(title, "openvela on KICKPI-K7\nRK3576 BSP");
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x4fc3f7), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  /* 把实际分辨率打出来 —— 它来自 VP1 的时序寄存器，不是写死的常量，
   * 显示出来正好核对 BSP 读到的模式是否正确。
   */

  {
    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text_fmt(info, "%d x %d",
                          (int)lv_display_get_horizontal_resolution(disp),
                          (int)lv_display_get_vertical_resolution(disp));
    lv_obj_set_style_text_color(info, lv_color_hex(0x9e9e9e), 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 110);
  }

  g_label_pos = lv_label_create(scr);
  lv_label_set_text(g_label_pos, "请触摸屏幕");
  lv_obj_set_style_text_align(g_label_pos, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(g_label_pos, lv_color_hex(0xffffff), 0);
  lv_obj_align(g_label_pos, LV_ALIGN_CENTER, 0, 0);

  g_dot = lv_obj_create(scr);
  lv_obj_set_size(g_dot, 30, 30);
  lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_dot, lv_color_hex(0xff5252), 0);
  lv_obj_set_style_border_width(g_dot, 0, 0);
  lv_obj_set_pos(g_dot, -100, -100);

  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_PRESSING, NULL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t    dsc;
  lv_nuttx_result_t result;

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
      /* 触摸没起来不影响看画面，继续跑并明确说出来，
       * 不要让"界面出来了"掩盖掉输入其实没通。
       */

      printf("警告：打不开 %s —— 触摸不可用，仅显示\n", dsc.input_path);
    }

  printf("LVGL 就绪 %dx%d，触摸 %s\n",
         (int)lv_display_get_horizontal_resolution(result.disp),
         (int)lv_display_get_vertical_resolution(result.disp),
         result.indev != NULL ? "可用" : "不可用");

  build_ui();
  lv_nuttx_run(&result);

  lv_nuttx_deinit(&result);
  lv_deinit();
  return EXIT_SUCCESS;
}
