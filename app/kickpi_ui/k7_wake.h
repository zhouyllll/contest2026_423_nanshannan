/****************************************************************************
 * app/kickpi_ui/k7_wake.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音唤醒："你好 openvela" + 问题。
 *   openvela：采集、环形缓冲、云端识别（mimo-v2.5-asr）、唤醒词匹配
 *   Linux（A72）：k7d 做端点检测，告诉这边一句话从哪到哪
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_WAKE_H
#define __APP_KICKPI_UI_K7_WAKE_H

#include <stdbool.h>
#include <stddef.h>

enum k7_wake_event_e
{
  K7_WAKE_NONE = 0,
  K7_WAKE_HEARD,       /* 识别出一句话，但不是唤醒词（text 里是原话） */
  K7_WAKE_ONLY,        /* 只说了唤醒词，等下一句当问题               */
  K7_WAKE_ASK,         /* 唤醒词 + 问题（或唤醒后的下一句）：question  */
};

struct k7_wake_status_s
{
  bool running;        /* 开关打开且 Linux 那边已经接上       */
  bool linking;        /* 开关打开，还在等 Linux 握手         */
  bool speaking;       /* Linux 报"开始说话"，还没说完        */
  bool recognizing;    /* 有一句在识别中                       */
  bool woken;          /* 说过唤醒词，正在等问题               */
  int  level;          /* 0~100                                 */
};

int  k7_wake_start(void);
void k7_wake_stop(void);

/* 朗读、录音机要用音频设备时先暂停采集，用完恢复。暂停会等采集真正
 * 停下（最多约 2 秒）才返回。
 */

void k7_wake_pause(bool pause);

void k7_wake_get_status(struct k7_wake_status_s *st);

/* 取一个事件（没有返回 K7_WAKE_NONE）。text 是识别原话，question 是
 * 要交给助手的问题（K7_WAKE_ASK 时有效）。
 */

int  k7_wake_get_event(char *text, size_t tcap, char *question,
                       size_t qcap);

#endif /* __APP_KICKPI_UI_K7_WAKE_H */
