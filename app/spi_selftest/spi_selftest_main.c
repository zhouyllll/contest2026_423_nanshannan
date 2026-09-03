/****************************************************************************
 * app/spi_selftest/spi_selftest_main.c
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

/* SPI 单板自检。
 *
 * ★ 为什么不用 cmocka_driver_spidev_master
 *
 *   那个用例是**跟另一块板子的 SPI 从机配对**用的：发传输长度 → 逐块
 *   发数据 → 发 CRC32，由对端回读校验。单板上没有对端，它不可能通过；
 *   而且它一上来就连发多次，出问题时分不清是哪一次、哪一步。
 *
 * ★ 这个自检要回答的问题，按依赖顺序排
 *
 *   1. /dev/spi4 能不能打开            —— 驱动注册成功了吗
 *   2. 一次 4 字节传输能不能**返回**   —— 控制器在产生时钟吗
 *   3. 收到的数据对不对               —— MOSI/MISO 短接了才有意义
 *
 *   前两问不需要接任何线就能回答。
 *
 * ★ "能返回"并不等于"时钟在跑"
 *
 *   驱动的收发循环带停滞保护，控制器不产生时钟时也会超时跳出、正常
 *   返回，读到的同样是一片 0。光看"返回了 + 全 0"分不出这两种情况。
 *
 *   能分开的观测是**计时**：时钟真在跑，耗时 ≈ 帧数 × 8 / SCLK，
 *   会随请求频率成比例变化；停滞超时则是与频率无关的固定值（几百
 *   毫秒量级）。所以第 2 问用两个相差 10 倍的频率各测一次，比的是
 *   两次耗时的**比值**，不是绝对值 —— 绝对值受调用开销影响，比值不受。
 *
 * ★ 图案必须是变化的
 *
 *   MISO 悬空时会稳定读回 0xff（内部上拉）或 0x00。用固定图案分不出
 *   "回环通了"和"线根本没接"，必须用一组互不相同、且不等于 0x00/0xff
 *   的字节。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/spi/spi_transfer.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SPI_DEV      "/dev/spi4"
#define PROBE_LEN    4       /* 第一次只发 4 字节：出问题时短比长好查 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 互不相同、且都不是 0x00/0xff */

static const uint8_t g_pattern[] =
{
  0x5a, 0xa5, 0x01, 0x80, 0x3c, 0xc3, 0x0f, 0xf0,
  0x55, 0xaa, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int do_xfer(int fd, const uint8_t *tx, uint8_t *rx, int len,
                   uint32_t hz)
{
  struct spi_trans_s trans;
  struct spi_sequence_s seq;

  memset(&trans, 0, sizeof(trans));
  memset(&seq, 0, sizeof(seq));

  trans.txbuffer = (void *)tx;
  trans.rxbuffer = rx;
  trans.nwords   = len;
  trans.deselect = true;

  seq.mode      = 0;      /* SPIDEV_MODE0 */
  seq.nbits     = 8;
  seq.frequency = hz;
  seq.ntrans    = 1;
  seq.trans     = &trans;

  return ioctl(fd, SPIIOC_TRANSFER, (unsigned long)&seq);
}

static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static void dump(const char *tag, const uint8_t *p, int len)
{
  int i;

  printf("  %s:", tag);
  for (i = 0; i < len; i++)
    {
      printf(" %02x", p[i]);
    }

  printf("\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  uint8_t rx[sizeof(g_pattern)];
  uint32_t hz = 1000000;
  int fd;
  int ret;
  int i;
  int bad;

  if (argc > 1)
    {
      hz = (uint32_t)strtoul(argv[1], NULL, 0);
    }

  /* ---- 第 1 问：设备能不能打开 ---- */

  printf("[1] open %s ... ", SPI_DEV);
  fflush(stdout);

  fd = open(SPI_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("失败 errno=%d —— 驱动没注册上，后面不用查了\n", errno);
      return 1;
    }

  printf("OK\n");

  /* ---- 第 2 问：时钟到底在不在跑 ---- */

    {
      uint64_t t_fast;
      uint64_t t_slow;
      int n = sizeof(g_pattern);

      printf("[2] 计时：%d 字节 @ %luHz 与 @ %luHz\n",
             n, (unsigned long)hz, (unsigned long)(hz / 10));

      memset(rx, 0, sizeof(rx));
      t_fast = now_us();
      ret = do_xfer(fd, g_pattern, rx, n, hz);
      t_fast = now_us() - t_fast;
      if (ret < 0)
        {
          printf("    ioctl 失败 errno=%d\n", errno);
          close(fd);
          return 1;
        }

      memset(rx, 0, sizeof(rx));
      t_slow = now_us();
      ret = do_xfer(fd, g_pattern, rx, n, hz / 10);
      t_slow = now_us() - t_slow;
      if (ret < 0)
        {
          printf("    ioctl 失败 errno=%d\n", errno);
          close(fd);
          return 1;
        }

      printf("    快: %lluus   慢: %lluus   理论快档: %luus\n",
             (unsigned long long)t_fast, (unsigned long long)t_slow,
             (unsigned long)((uint64_t)n * 8 * 1000000 / hz));

      /* 慢档降频 10 倍，耗时应明显变长。放宽到 3 倍是给调用开销留
       * 余量 —— 传输本身很短时，ioctl 与调度的固定开销占比不低。
       */

      if (t_slow > t_fast * 3)
        {
          printf("    → 耗时随频率变化，时钟确实在跑\n");
        }
      else
        {
          printf("    → 耗时与频率无关，时钟没在跑"
                 "（两次都是停滞超时跳出）\n");
        }
    }

  /* ---- 第 3 问：数据对不对（需要 MOSI/MISO 短接）---- */

  printf("[3] %d 字节回环 ... ", (int)sizeof(g_pattern));
  fflush(stdout);

  memset(rx, 0, sizeof(rx));
  ret = do_xfer(fd, g_pattern, rx, sizeof(g_pattern), hz);
  if (ret < 0)
    {
      printf("ioctl 失败 errno=%d\n", errno);
      close(fd);
      return 1;
    }

  for (bad = -1, i = 0; i < (int)sizeof(g_pattern); i++)
    {
      if (rx[i] != g_pattern[i])
        {
          bad = i;
          break;
        }
    }

  if (bad < 0)
    {
      printf("全部原样返回 —— 回环通过\n");
    }
  else
    {
      /* 收到的全是同一个值，说明 MISO 根本没被驱动（悬空），
       * 这与"接了线但数据错"是两回事，分开报。
       */

      int same = 1;
      for (i = 1; i < (int)sizeof(g_pattern); i++)
        {
          if (rx[i] != rx[0])
            {
              same = 0;
              break;
            }
        }

      if (same)
        {
          printf("收到的 %d 字节全是 0x%02x —— MISO 没被驱动，"
                 "多半是没短接\n", (int)sizeof(g_pattern), rx[0]);
        }
      else
        {
          printf("第 %d 字节起不符\n", bad);
        }

      dump("发", g_pattern, sizeof(g_pattern));
      dump("收", rx, sizeof(g_pattern));
    }

  close(fd);
  return 0;
}
