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
#include <stdlib.h>
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
      printf("用法: cam on|off|rx|cap|jpeg|jpegtest|show|white|black|bars|half|vhalf|line|box\n");
      printf("      cam ub <0-2>     还原 U-Boot 窗口并写图案\n");
      printf("      cam morph <0-7>  从 U-Boot 参数出发逐个变量地改（7 复现故障）\n");
      printf("      cam regs         打印 ESMART1/VP1 寄存器现状\n");
      printf("      cam gain <0-240> [shr]  模拟增益，每级 0.3dB\n");
      printf("      cam show [0|1]   送屏，参数 0 关伽马用于对照\n");
      printf("      cam preview [n]  连续预览 n 帧（默认 300），敲键停止\n");
      printf("      cam vmax <行数>  改帧率，1143≈83fps 3165≈30fps\n");
      printf("      cam fbinfo|stat\n");
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
  else if (strcmp(argv[1], "cap") == 0)
    {
      ret = kickpi_camera_capture();
    }
  else if (strcmp(argv[1], "jpeg") == 0)
    {
      /* cam jpeg <路径> [相位] [质量]
       *
       * 把最近取到的一帧去马赛克并编码成 JPEG。
       *
       * ★ 相位默认 0（RGGB），但**必须拿彩色标定图实测确认** ——
       *   相位错了图像不会崩，只会红蓝互换或整体偏色，看起来像
       *   白平衡问题，很容易归错因。
       */

      ret = kickpi_camera_jpeg(argc > 2 ? argv[2] : "/tmp/cam.jpg",
                               argc > 3 ? atoi(argv[3]) : 0,
                               argc > 4 ? atoi(argv[4]) : 85);
    }
  else if (strcmp(argv[1], "jpegtest") == 0)
    {
      /* cam jpegtest [路径] [相位]
       *
       * 用合成的红/绿/蓝三色带 Bayer 图验证「去马赛克 + JPEG」通路。
       * 摄像头接不上时也能跑 —— 它验的是相位映射和跨距索引，
       * 恰好是最容易写错、又最难从真实照片上看出来的地方。
       */

      ret = kickpi_imgproc_selftest(argc > 3 ? atoi(argv[3]) : 0,
                                    argc > 2 ? argv[2] : "/tmp/bayer.jpg");
    }
  else if (strcmp(argv[1], "show") == 0)
    {
      ret = kickpi_camera_show(argc > 2 ? atoi(argv[2]) : 1);
    }
  else if (strcmp(argv[1], "white") == 0)
    {
      ret = kickpi_camera_fbtest(0);
    }
  else if (strcmp(argv[1], "black") == 0)
    {
      ret = kickpi_camera_fbtest(1);
    }
  else if (strcmp(argv[1], "bars") == 0)
    {
      ret = kickpi_camera_fbtest(2);
    }
  else if (strcmp(argv[1], "half") == 0)
    {
      ret = kickpi_camera_fbtest(3);
    }
  else if (strcmp(argv[1], "vhalf") == 0)
    {
      ret = kickpi_camera_fbtest(4);
    }
  else if (strcmp(argv[1], "line") == 0)
    {
      ret = kickpi_camera_fbtest(5);
    }
  else if (strcmp(argv[1], "box") == 0)
    {
      ret = kickpi_camera_fbtest(6);
    }
  else if (strcmp(argv[1], "ub") == 0)
    {
      ret = kickpi_camera_ubtest(argc > 2 ? atoi(argv[2]) : 0);
    }
  else if (strcmp(argv[1], "morph") == 0)
    {
      if (argc < 3)
        {
          printf("morph 要一个步骤号 0~7\n");
          return 1;
        }

      ret = kickpi_camera_morph(atoi(argv[2]));
    }
  else if (strcmp(argv[1], "gain") == 0)
    {
      if (argc < 3)
        {
          printf("用法: cam gain <0-240> [shr]   每级 0.3dB\n");
          return 1;
        }

      ret = kickpi_camera_gain(atoi(argv[2]),
                               argc > 3 ? atoi(argv[3]) : -1);
    }
  else if (strcmp(argv[1], "preview") == 0)
    {
      ret = kickpi_camera_preview(argc > 2 ? atoi(argv[2]) : 0);
    }
  else if (strcmp(argv[1], "vmax") == 0)
    {
      if (argc < 3)
        {
          printf("用法: cam vmax <1143-1048575>  一帧总行数，越小越快\n");
          return 1;
        }

      ret = kickpi_camera_vmax(atoi(argv[2]));
    }
  else if (strcmp(argv[1], "regs") == 0)
    {
      ret = kickpi_camera_regs();
    }
  else if (strcmp(argv[1], "fbinfo") == 0)
    {
      ret = kickpi_camera_fbinfo();
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
