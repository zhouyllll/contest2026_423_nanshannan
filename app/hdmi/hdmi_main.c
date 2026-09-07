/****************************************************************************
 * app/hdmi/hdmi_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI 调试命令。当前只有 probe —— 先自检再下结论。
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <arch/board/board.h>

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2 || strcmp(argv[1], "probe") == 0)
    {
      ret = rk3576_hdmi_probe();
      printf("ret=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  printf("用法: hdmi probe\n");
  return 1;
}
