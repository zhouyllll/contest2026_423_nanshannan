/****************************************************************************
 * app/kickpi_ui/k7_vision.h
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_VISION_H
#define __APP_KICKPI_UI_K7_VISION_H

#include <stddef.h>

/* 拍一张照片，连同 question 发给视觉模型，回答写进 answer。
 * 阻塞（通常十秒左右），在工作线程里调。返回 0 成功；失败时 answer 里
 * 是给用户看的说明。
 */

int k7_vision_ask(const char *question, char *answer, size_t cap);

#endif /* __APP_KICKPI_UI_K7_VISION_H */
