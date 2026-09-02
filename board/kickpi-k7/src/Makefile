############################################################################
# vendor/rockchip/boards/rk3576/kickpi-k7/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = kickpi_k7_boardinit.c
CSRCS += kickpi_k7_appinit.c

ifeq ($(CONFIG_DEV_GPIO),y)
CSRCS += kickpi_k7_gpio.c
endif

ifeq ($(CONFIG_RK3576_I2C),y)
CSRCS += kickpi_k7_i2c.c
endif

# 屏与触摸共用同一条电源轨，任一方启用就需要这个文件
ifneq ($(CONFIG_INPUT_FT5X06)$(CONFIG_RK3576_VOP2),)
CSRCS += kickpi_k7_lcd.c
endif

ifeq ($(CONFIG_INPUT_FT5X06),y)
CSRCS += kickpi_k7_touch.c
endif

ifeq ($(CONFIG_AUDIO_ES8388),y)
CSRCS += kickpi_k7_audio.c
endif

include $(TOPDIR)/boards/Board.mk
