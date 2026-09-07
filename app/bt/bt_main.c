/****************************************************************************
 * app/bt/bt_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 蓝牙手动初始化命令。
 *
 * ★ 为什么不放在启动路径：固件加载要等模组的 HCI 应答，验证通过之前
 *   随时可能卡住；而 board_app_initialize() 跑在 nsh 任务上下文，它一卡
 *   控制台就起不来，loader 也发不进去，只能 MASKROM 恢复。做成命令之后，
 *   最坏情况只是这条命令不返回，板子仍然可以烧。
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <arch/board/board.h>

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2 || strcmp(argv[1], "init") == 0)
    {
      ret = kickpi_k7_bt_initialize();
      printf("ret=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  printf("用法: bt init\n");
  return 1;
}
