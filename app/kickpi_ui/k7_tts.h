/****************************************************************************
 * app/kickpi_ui/k7_tts.h
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_TTS_H
#define __APP_KICKPI_UI_K7_TTS_H

/* 把一段文字合成语音并放出来，放完才返回（几秒到几十秒）。
 * 在工作线程里调，别在 LVGL 线程里调。返回 0 成功，负数失败。
 */

int k7_tts_speak(const char *text);

#endif /* __APP_KICKPI_UI_K7_TTS_H */
