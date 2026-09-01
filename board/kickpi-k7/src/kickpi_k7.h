/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Functions Definitions
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
/****************************************************************************
 * Name: kickpi_k7_gpio_initialize
 *
 * Description:
 *   注册板上 GPIO 输出引脚为 /dev/gpioN。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_gpio_initialize(void);
#endif

#ifdef CONFIG_RK3576_I2C
/****************************************************************************
 * Name: kickpi_k7_i2c_initialize
 *
 * Description:
 *   初始化板上 I2C 总线并注册为 /dev/i2cN。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_i2c_initialize(void);
#endif

#ifdef CONFIG_INPUT_GT9XX
/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   注册 GT9xx 电容触摸为 /dev/input0。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void);
#endif

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)
/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   ES8388 codec + SAI1，注册为 /dev/audio/pcm0。
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H */
