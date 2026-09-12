/****************************************************************************
 * apps/packages/demos/contest2026_423_ampctl/ampctl_main.c
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

/****************************************************************************
 * AMP 排查工具。
 *
 * 这个命令存在的理由和 spi_selftest 一样：**AMP 出问题时，"没反应"有太多
 * 种原因**——对端没起来、门铃走错 group、vring 地址对不上、握手没完成、
 * 端点没绑定。一条 "ping 超时" 区分不了这五种，而它们的修法完全不同。
 *
 * 所以这里把链路拆成三层，每层单独有输出：
 *
 *   status  只看本端：mailbox 寄存器 + rptun 计数。**不需要对端在线**，
 *           因此永远有输出，是"是不是我这边就没配对"的判据。
 *   ping    建端点 → 发一帧 → 等回应，有超时。测的是端到端。
 *   listen  只收不发，用来看对端主动发了什么。
 *
 * 每一层都有界：没有任何一条路径会无限等待，最长就是 -t 给的毫秒数。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>

#include <nuttx/rpmsg/rpmsg.h>

#include <arch/chip/amp.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AMPCTL_EPT_NAME   "amp-echo"
#define AMPCTL_DEF_TMO_MS 3000

/* 自检用的 group。必须是 rptun 没占用的 —— 见 rk3576_mailbox_selftest()
 * 的说明：在 rptun 的 group 上放假门铃会把它推进没有对端的非法状态。
 */

#define AMPCTL_SELFTEST_GROUP 5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ampctl_ctx_s
{
  struct rpmsg_endpoint ept;
  sem_t                 sem;      /* 收到回应时 post                 */
  bool                  bound;    /* 端点已经建起来                  */
  char                  rxbuf[256];
  size_t                rxlen;
  const char           *cpuname;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_mbox_regname[8] =
{
  "A2B_INTEN", "A2B_STATUS", "A2B_CMD", "A2B_DATA",
  "B2A_INTEN", "B2A_STATUS", "B2A_CMD", "B2A_DATA"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ampctl_usage(void)
{
  printf("用法: ampctl <子命令>\n"
         "  status            本端链路状态（不需要对端在线）\n"
         "  ping [-t ms] [文本]  发一帧并等回应，默认超时 %d ms\n"
         "  listen [-t ms]    只收不发，打印对端发来的帧\n"
         "  selftest          自己给自己按门铃，验证接收路径（不需要对端）\n",
         AMPCTL_DEF_TMO_MS);
}

/****************************************************************************
 * Name: ampctl_status
 *
 * Description:
 *   只读本端状态。三组信息各自回答一个问题：
 *
 *     rptun 计数  —— 门铃通没通？握手成没成？
 *     mailbox 寄存器 —— 硬件层面有没有卡住的 pending 位？
 *     /dev/rpmsg  —— rpmsg 设备有没有真的注册出来？
 *
 ****************************************************************************/

static int ampctl_status(void)
{
  struct rk3576_rptun_stat_s st;
  uint32_t regs[8];
  int i;
  int ret;

  ret = rk3576_rptun_getstat(&st);
  if (ret < 0)
    {
      printf("读 rptun 状态失败: %d\n", ret);
      return ret;
    }

  printf("== rptun ==\n");
  printf("  已注册    : %s\n", st.registered ? "是" : "否");
  printf("  握手完成  : %s%s\n", st.driver_ok ? "是" : "否",
         st.driver_ok ? "" : "  ← 还没收到对端的第一次门铃");
  printf("  收到门铃  : %" PRIu32 "\n", st.kicks_rx);
  printf("  发出门铃  : %" PRIu32 "\n", st.kicks_tx);
  printf("  发被挡回  : %" PRIu32 "%s\n", st.tx_busy,
         st.tx_busy > 0 ? "  ← 对端没在清信箱" : "");
  printf("  最后收到  : cmd=%08" PRIx32 " data=%08" PRIx32 "%s\n",
         st.last_cmd, st.last_data,
         (st.kicks_rx == 0 && st.last_data != 0) ?
           "  ← 收到了但不是本链路的" : "");

  printf("== 共享内存 ==\n");
  printf("  vring0(发) : %08x\n", RK3576_RPTUN_VRING0_DA);
  printf("  vring1(收) : %08x\n", RK3576_RPTUN_VRING1_DA);
  printf("  缓冲池     : %08x + %dK\n",
         RK3576_RPTUN_POOL_DA, RK3576_RPTUN_POOL_LEN / 1024);

  printf("== mailbox group%d（收）==\n", CONFIG_RK3576_RPTUN_RX_GROUP);
  ret = rk3576_mailbox_dump(CONFIG_RK3576_RPTUN_RX_GROUP, regs);
  if (ret >= 0)
    {
      for (i = 0; i < 8; i++)
        {
          printf("  %-10s %08" PRIx32 "\n", g_mbox_regname[i], regs[i]);
        }
    }

  printf("== mailbox group%d（发）==\n", CONFIG_RK3576_RPTUN_TX_GROUP);
  ret = rk3576_mailbox_dump(CONFIG_RK3576_RPTUN_TX_GROUP, regs);
  if (ret >= 0)
    {
      for (i = 0; i < 8; i++)
        {
          printf("  %-10s %08" PRIx32 "\n", g_mbox_regname[i], regs[i]);
        }
    }

  /* rpmsg 设备节点存在 == rptun 已经跑完 dev_start。 */

  printf("== rpmsg 设备 ==\n");
  if (access("/dev/rpmsg/" CONFIG_RK3576_RPTUN_CPUNAME, F_OK) == 0)
    {
      printf("  /dev/rpmsg/%s 存在\n", CONFIG_RK3576_RPTUN_CPUNAME);
    }
  else
    {
      printf("  /dev/rpmsg/%s 不存在 —— rptun 还卡在 -EAGAIN\n",
             CONFIG_RK3576_RPTUN_CPUNAME);
    }

  return OK;
}

/****************************************************************************
 * Name: ampctl_selftest
 *
 * Description:
 *   门铃接收路径自检。分两步，**第二步同样重要**：
 *
 *     正例：写一次 → 应该进中断，且读回的两个字与写入一致
 *     反例：不写   → 应该超时
 *
 *   只跑正例是不够的。一个永远返回"成功"的自检比没有自检更坏 —— 本项目
 *   已经被四个这样的假仪器坑过（RXFIFOLR、GPIO 探针、计数器没使能的
 *   RX_DATA_CNT、参数未初始化的回环）。每一个都给出了**自信的错误答案**，
 *   而不是"未知"。所以这里必须证明它会失败。
 *
 ****************************************************************************/

static int ampctl_selftest(void)
{
  uint32_t rx_cmd = 0;
  uint32_t rx_data = 0;
  unsigned int group = AMPCTL_SELFTEST_GROUP;
  int ret;

  if (group == CONFIG_RK3576_RPTUN_RX_GROUP ||
      group == CONFIG_RK3576_RPTUN_TX_GROUP)
    {
      printf("自检用的 group%u 和 rptun 撞了，换一个\n", group);
      return -EBUSY;
    }

  printf("在空闲的 mailbox group%u 上自检（不影响 rptun）\n", group);

  /* 正例 */

  ret = rk3576_mailbox_selftest(group, 0xa5a50003, 0x524d5347,
                                &rx_cmd, &rx_data);
  if (ret == OK)
    {
      printf("  正例 ✓ 中断到达，读回 cmd=%08" PRIx32 " data=%08" PRIx32 "\n",
             rx_cmd, rx_data);
    }
  else
    {
      printf("  正例 ✗ %d（%s）\n", ret,
             ret == -ETIMEDOUT ? "没进中断：时钟/IRQ 号/INTEN 三者之一不对" :
             ret == -EIO       ? "进了中断但读回的值对不上" : "参数或占用");
      return ret;
    }

  /* 反例：同一条路径，但不触发。必须超时 —— 否则说明它根本不在测中断。 */

  rx_cmd = rx_data = 0;
  ret = rk3576_mailbox_selftest_notrigger(group, &rx_cmd, &rx_data);
  if (ret == -ETIMEDOUT)
    {
      printf("  反例 ✓ 不写 DATA 时如期超时 —— 说明正例测的确实是中断\n");
      return OK;
    }

  printf("  反例 ✗ 不触发也返回了 %d —— 这个自检不可信，别用它下结论\n", ret);
  return -EIO;
}

/****************************************************************************
 * Name: ampctl_ept_cb
 *
 * Description:
 *   端点收到数据。拷走并唤醒等待方 —— data 在回调返回后就不再有效。
 *
 ****************************************************************************/

static int ampctl_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                         uint32_t src, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  UNUSED(src);

  ctx->rxlen = len < sizeof(ctx->rxbuf) - 1 ? len : sizeof(ctx->rxbuf) - 1;
  memcpy(ctx->rxbuf, data, ctx->rxlen);
  ctx->rxbuf[ctx->rxlen] = '\0';

  rpmsg_post(ept, &ctx->sem);
  return 0;
}

/****************************************************************************
 * Name: ampctl_device_created
 *
 * Description:
 *   rpmsg 设备起来了。只有名字对得上的那一个才建端点 —— 板上可能不止
 *   一条 rpmsg 链路。
 *
 ****************************************************************************/

static void ampctl_device_created(struct rpmsg_device *rdev, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), ctx->cpuname) != 0)
    {
      return;
    }

  if (rpmsg_create_ept(&ctx->ept, rdev, AMPCTL_EPT_NAME,
                       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                       ampctl_ept_cb, NULL) == 0)
    {
      ctx->ept.priv = ctx;
      ctx->bound    = true;
    }
}

static void ampctl_device_destroy(struct rpmsg_device *rdev, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), ctx->cpuname) == 0 && ctx->bound)
    {
      rpmsg_destroy_ept(&ctx->ept);
      ctx->bound = false;
    }
}

/****************************************************************************
 * Name: ampctl_open
 *
 * Description:
 *   注册回调并等端点建起来。rpmsg_register_callback() 对**已经存在**的
 *   设备会立刻回调，所以不会漏掉"注册之前设备就好了"的情况。
 *
 ****************************************************************************/

static int ampctl_open(struct ampctl_ctx_s *ctx, int timeout_ms)
{
  int waited = 0;

  memset(ctx, 0, sizeof(*ctx));
  ctx->cpuname = CONFIG_RK3576_RPTUN_CPUNAME;
  sem_init(&ctx->sem, 0, 0);

  rpmsg_register_callback(ctx, ampctl_device_created,
                          ampctl_device_destroy, NULL, NULL);

  /* 有界等待：每 20ms 看一次，最多等 timeout_ms。 */

  while (!ctx->bound && waited < timeout_ms)
    {
      usleep(20 * 1000);
      waited += 20;
    }

  if (!ctx->bound)
    {
      rpmsg_unregister_callback(ctx, ampctl_device_created,
                                ampctl_device_destroy, NULL, NULL);
      sem_destroy(&ctx->sem);
      printf("端点没建起来（等了 %d ms）。先跑 ampctl status 看握手完成没有。\n",
             timeout_ms);
      return -ETIMEDOUT;
    }

  return OK;
}

static void ampctl_close(struct ampctl_ctx_s *ctx)
{
  rpmsg_unregister_callback(ctx, ampctl_device_created,
                            ampctl_device_destroy, NULL, NULL);
  if (ctx->bound)
    {
      rpmsg_destroy_ept(&ctx->ept);
      ctx->bound = false;
    }

  sem_destroy(&ctx->sem);
}

static int ampctl_ping(int timeout_ms, const char *text)
{
  struct ampctl_ctx_s ctx;
  int ret;

  ret = ampctl_open(&ctx, timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  printf("端点已绑定，发送 \"%s\"\n", text);

  ret = rpmsg_send(&ctx.ept, text, strlen(text) + 1);
  if (ret < 0)
    {
      printf("发送失败: %d\n", ret);
      goto out;
    }

  ret = rpmsg_tickwait(&ctx.ept, &ctx.sem, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("等回应超时（%d ms）。对端收到了吗？看 ampctl status 的"
             "「发出门铃」有没有涨。\n", timeout_ms);
    }
  else
    {
      printf("回应 %zu 字节: \"%s\"\n", ctx.rxlen, ctx.rxbuf);
      ret = OK;
    }

out:
  ampctl_close(&ctx);
  return ret;
}

static int ampctl_listen(int timeout_ms)
{
  struct ampctl_ctx_s ctx;
  int ret;

  ret = ampctl_open(&ctx, timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  printf("监听中，最长 %d ms…\n", timeout_ms);

  ret = rpmsg_tickwait(&ctx.ept, &ctx.sem, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("这段时间内对端没发东西。\n");
    }
  else
    {
      printf("收到 %zu 字节: \"%s\"\n", ctx.rxlen, ctx.rxbuf);
      ret = OK;
    }

  ampctl_close(&ctx);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int timeout = AMPCTL_DEF_TMO_MS;
  const char *text = "hello from openvela";
  int i;

  if (argc < 2)
    {
      ampctl_usage();
      return EXIT_FAILURE;
    }

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
          timeout = atoi(argv[++i]);
        }
      else
        {
          text = argv[i];
        }
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return ampctl_status() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "ping") == 0)
    {
      return ampctl_ping(timeout, text) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "selftest") == 0)
    {
      return ampctl_selftest() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "listen") == 0)
    {
      return ampctl_listen(timeout) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  ampctl_usage();
  return EXIT_FAILURE;
}
