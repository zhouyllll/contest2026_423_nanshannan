/****************************************************************************
 * app/cam/cam_main.c
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

/* 摄像头取图链路的调试入口。
 *
 * 链路是 IMX415 -> D-PHY RX -> CSI-2 host -> CIF -> 内存 -> 屏，
 * 五级。每加一级就在这里加一个子命令，好处是每一级都能**单独**下令、
 * 单独观察 —— 而不是写一个"取一帧"的大按钮，出问题时分不清是哪一级。
 *
 * 当前可用：
 *   cam on      传感器开始在 MIPI 上推数据
 *   cam off     回待机
 *   cam rx      打开接收端（D-PHY + CSI-2 host）
 *   cam stat    读 CSI-2 host 的通道状态与错误计数
 *
 * 典型用法是 on -> rx -> 等一会 -> stat：先让发送端跑起来，再打开
 * 接收端，隔一段时间再看错误计数。分三步是有意的 —— 一次做完的话
 * 读到的错误可能只是"接收端比发送端晚开"造成的启动瞬态，不是真问题。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>

#include <arch/board/board.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2)
    {
      printf("用法: cam on|off|rx|stat\n");
      return 1;
    }

  if (strcmp(argv[1], "on") == 0)
    {
      ret = kickpi_camera_stream(true);
    }
  else if (strcmp(argv[1], "off") == 0)
    {
      ret = kickpi_camera_stream(false);
    }
  else if (strcmp(argv[1], "rx") == 0)
    {
      ret = kickpi_camera_receiver(true);
    }
  else if (strcmp(argv[1], "stat") == 0)
    {
      ret = kickpi_camera_status();
    }
  else
    {
      printf("未知子命令 %s\n", argv[1]);
      return 1;
    }

  printf("ret=%d\n", ret);
  return ret < 0 ? 1 : 0;
}
