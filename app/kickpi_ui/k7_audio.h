/****************************************************************************
 * app/kickpi_ui/k7_audio.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * ES8388 录音 / 放音的最小封装，给界面的录音机和语音唤醒共用。
 * 两个方向都走 /dev/audio/pcm1（裸编解码器），固定 48kHz / 立体声 / 16 位。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_AUDIO_H
#define __APP_KICKPI_UI_K7_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define K7A_RATE 48000

/* 每收到一块采集数据回调一次：st 是交错立体声，frames 是帧数（一帧两个
 * 采样）。返回 false 表示够了，停止采集。回调跑在调用 k7a_capture 的
 * 线程里，别在里面碰 LVGL。
 */

typedef bool (*k7a_capture_cb_t)(const int16_t *st, size_t frames,
                                  void *priv);

/* 采集，直到回调返回 false 或者超过 max_ms（上界，不会无限阻塞）。
 * 返回 0 成功，负数为 -errno。
 */

int k7a_capture(k7a_capture_cb_t cb, void *priv, int max_ms);

/* 放一段 48kHz 单声道（两个声道放同样的内容），放完才返回。 */

int k7a_play_mono(const int16_t *mono, size_t frames);

/* 放音音量 0~100（%），对耳机和喇叭同时生效。放音进行中调用会立即生效，
 * 否则在下一次放音开始时生效。
 */

void k7a_set_volume(int percent);
int k7a_get_volume(void);

/* 采集与放音共用一个设备，同一时刻只能有一个在用。忙时返回 true。 */

bool k7a_busy(void);

#endif /* __APP_KICKPI_UI_K7_AUDIO_H */
