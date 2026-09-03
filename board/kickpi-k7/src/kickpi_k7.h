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

/****************************************************************************
 * Name: kickpi_k7_spi_initialize
 *
 * Description:
 *   注册 SPI4 为 /dev/spi4。板上只有它引到了 40 针扩展口。
 *
 ****************************************************************************/

int kickpi_k7_spi_initialize(void);

/****************************************************************************
 * Name: kickpi_spi_loopback
 *
 * Description:
 *   SPI4 的 MOSI/MISO 短接回环自检，需要人工把排针 19/21 脚接起来。
 *
 ****************************************************************************/

int kickpi_spi_loopback(void);
#endif

#ifdef CONFIG_INPUT_FT5X06
/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   注册 GT9xx 电容触摸为 /dev/input0。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void);
#endif


/****************************************************************************
 * Name: kickpi_k7_lcd_power
 *
 * Description:
 *   开关 VCC3V3_LCD_S0 电源轨（LCD_PWREN_H / GPIO0_C6），并顺带处理屏的
 *   复位脚。屏和触摸共用这条轨，必须由一处统一管理，任一方各自开关都会
 *   把另一方带掉电。
 *
 *   on=true 时返回后电源已稳定、屏复位已释放，可以开始 I2C / DSI 通信。
 *
 ****************************************************************************/

int kickpi_k7_lcd_power(bool on);

/****************************************************************************
 * Name: kickpi_k7_camera_initialize
 *
 * Description:
 *   给 IMX415 供电、唤醒并读型号寄存器确认在位。只做传感器探测，
 *   不建立取图通路（CSI2/CIF/ISP 尚未实现）。
 *
 ****************************************************************************/

int kickpi_k7_camera_initialize(void);

/****************************************************************************
 * Name: kickpi_k7_rtc_initialize
 *
 * Description:
 *   注册板上 HYM8563 为 /dev/rtc0。
 *
 ****************************************************************************/

int kickpi_k7_rtc_initialize(void);

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
