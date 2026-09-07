/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* 音频：ES8388 codec + SAI1。
 *
 * 板上连接（出处：原厂 dtb 的 es8388-sound 节点）：
 *     rockchip,cpu   = sai@2a610000   → SAI1
 *     rockchip,codec = es8388@10      → I2C3 上的 0x10
 *
 * codec 驱动本体是上游现成的 drivers/audio/es8388.c，本文件只负责
 * 把 I2C 与 I2S 两个句柄交给它，并注册成 /dev/audio/pcm0。
 *
 * ★ ES8388 的 0x10 已在板上实测应答（i2c dev -b 3 扫到），
 *   因此这条链路的 I2C 一侧是确定的，不像触摸那样带推测成分。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>

#include "rk3576_i2c.h"
#include "rk3576_sai.h"
#include "kickpi_k7.h"

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_I2C_BUS      3        /* ES8388 所在总线，已实测 */
#define AUDIO_I2C_ADDR     0x10     /* 已实测应答              */
#define AUDIO_I2C_FREQ     400000
#define AUDIO_SAI_PORT     1

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct es8388_lower_s g_es8388_lower =
{
  .frequency = AUDIO_I2C_FREQ,
  .address   = AUDIO_I2C_ADDR,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   把 ES8388 与 SAI1 组合成音频设备并注册。
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void)
{
  struct audio_lowerhalf_s *codec;
  struct audio_lowerhalf_s *pcm;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  i2c = rk3576_i2cbus_initialize(AUDIO_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: 音频 I2C%d 初始化失败\n", AUDIO_I2C_BUS);
      return -ENODEV;
    }

  i2s = rk3576_sai_initialize(AUDIO_SAI_PORT);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: SAI%d 初始化失败\n", AUDIO_SAI_PORT);
      return -ENODEV;
    }

  codec = es8388_initialize(i2c, i2s, &g_es8388_lower);
  if (codec == NULL)
    {
      syslog(LOG_ERR, "ERROR: ES8388 初始化失败（I2C%d:0x%02x 无应答？）\n",
             AUDIO_I2C_BUS, AUDIO_I2C_ADDR);
      return -ENODEV;
    }

  /* 套一层 PCM 解码，使 /dev/audio/pcm0 能直接吃 WAV */

  pcm = pcm_decode_initialize(codec);
  if (pcm == NULL)
    {
      syslog(LOG_ERR, "ERROR: PCM 解码层初始化失败\n");
      return -ENODEV;
    }

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/audio/pcm0 失败: %d\n", ret);
      return ret;
    }

  /* ★ 再把**未经 PCM 解码包装**的编解码器注册成 pcm1，专供录音。
   *
   *   pcm_decode 是给放音用的解码器：它在收到缓冲区时会调 pcm_parsewav()
   *   去解析 WAV 头。录音递进去的是**空缓冲区**，解析必然失败 ——
   *   实测 AUDIOIOC_ENQUEUEBUFFER 直接返回 ENOENT。
   *
   *   也就是说 pcm0 这条路天然只能放音。ES8388 本身是全双工的
   *   （getcaps 上报 INPUT|OUTPUT，原厂 Android 下同一颗芯片
   *   playback 1 : capture 1），所以把裸设备另外注册一个名字，录音走它。
   *
   *   两个设备共用同一个下半部：同一时刻只跑一个方向，这与该编解码器
   *   的实际用法一致。
   */

  ret = audio_register("pcm1", codec);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/audio/pcm1（录音）失败: %d\n", ret);
      /* 不致命：放音仍可用 */
    }

  syslog(LOG_INFO,
         "音频: /dev/audio/pcm0 放音 + pcm1 录音"
         "（ES8388@I2C%d:0x%02x + SAI%d）\n",
         AUDIO_I2C_BUS, AUDIO_I2C_ADDR, AUDIO_SAI_PORT);
  return OK;
}

#endif /* CONFIG_AUDIO_ES8388 && CONFIG_RK3576_SAI */
