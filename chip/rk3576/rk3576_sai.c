/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_sai.c
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

/* RK3576 SAI（Serial Audio Interface）。
 *
 * 两部分：
 *   rk3576_sai_probe()      前置链路（电源域 + 三路时钟 + 引脚复用）与自检
 *   rk3576_sai_initialize() i2s_dev_s 实现，供 drivers/audio/es8388.c 使用
 *
 * ★ 当前是 PIO 发送，不用 DMA。
 *
 *   SAI 的 DMA 走 ARM PL330（dmas = <&dmac0 2>），而 NuttX 没有 PL330
 *   驱动。与 eMMC 同样的取舍：先用 PIO 把通路走通、证明能出声，
 *   DMA 留到需要提性能时再补。代价是发送期间占着 CPU。
 *
 *   PIO 发送在 i2s_send 里同步完成：把整个 apb 缓冲区灌进 FIFO 后
 *   才回调。上层（audio 框架）本就允许 i2s_send 阻塞。
 *
 * ★ 只做发送（喇叭），不做接收（咪头）。
 *   收发共用时钟与帧同步配置，接收路径补起来不难，但没有验证手段时
 *   写了也是负担 —— 先让喇叭出声，有了可听的判据再加录音。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "rk3576_sai.h"
#include "rk3576_cru.h"
#include "rk3576_pinmux.h"
#include "rk3576_power.h"
#include "hardware/rk3576_sai.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_SAI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SAI1_BASE   RK3576_SAI1_ADDR

/* 时钟。出处：Linux drivers/clk/rockchip/clk-rk3576.c
 *   COMPOSITE(MCLK_SAI1_8CH_SRC, ... CLKGATE_CON(8), 4)
 *   COMPOSITE_NODIV(MCLK_SAI1_8CH, ... CLKSEL_CON(46), 11, 1, ... CLKGATE_CON(8), 5)
 *   GATE(HCLK_SAI1_8CH, "hclk_sai1_8ch", "hclk_audio_root", CLKGATE_CON(8), 6)
 */

#define SAI1_GATE_CON        8
#define SAI1_GATE_MCLK_SRC   4
#define SAI1_GATE_MCLK       5
#define SAI1_GATE_HCLK       6

/* 引脚复用 sai1m0，出处：原厂 dtb 的 pinctrl 节点，功能号均为 1 */

#define SAI1_PIN_BANK        4
#define SAI1_PIN_SCLK        3
#define SAI1_PIN_LRCK        5
#define SAI1_PIN_SDO0        7
#define SAI1_PIN_SDI0       11
#define SAI1_PIN_FUNC        1

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t sai_getreg(uint32_t off)
{
  return getreg32(SAI1_BASE + off);
}

static inline void sai_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, SAI1_BASE + off);
}

/****************************************************************************
 * Name: sai_reset
 *
 * Description:
 *   复位发送通道与帧同步。复位位写 1 后自清，要轮询等它归零 ——
 *   不等就配置后续寄存器，配置会被复位过程冲掉。
 *
 ****************************************************************************/

static void sai_reset(void)
{
  int us;

  sai_putreg(RK3576_SAI_CLR, SAI_CLR_TXC | SAI_CLR_FSC);

  for (us = 0; us < 10000; us++)
    {
      if ((sai_getreg(RK3576_SAI_CLR) & (SAI_CLR_TXC | SAI_CLR_FSC)) == 0)
        {
          return;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "SAI: 复位超时 CLR=0x%08" PRIx32 "\n",
         sai_getreg(RK3576_SAI_CLR));
}

/****************************************************************************
 * i2s_dev_s 实现
 ****************************************************************************/

struct rk3576_sai_dev_s
{
  struct i2s_dev_s dev;          /* 必须是第一个成员 */
  mutex_t          lock;
  uint32_t         samplerate;
  int              datawidth;
  int              channels;
};

static struct rk3576_sai_dev_s g_sai1;

/****************************************************************************
 * Name: sai_configure
 *
 * Description:
 *   按当前的采样率/位宽/声道数配置 SAI。
 *
 *   ★ 位宽相关的三个字段含义不同，容易混：
 *       VDW  有效数据位宽 —— 实际样本多少位（16）
 *       SBW  槽位宽       —— 一个时隙占多少位（这里同为 16）
 *       SNB  每帧槽数     —— 立体声为 2
 *     VDW < SBW 时用 VDJ 决定数据在槽内左对齐还是右对齐。
 *
 *   ★ 分频：SCLK = MCLK / MDIV，而一帧需要 SNB * SBW 个 SCLK。
 *     故 MDIV = MCLK / (samplerate * SNB * SBW)。
 *     MCLK 由 CRU 定为 12.288MHz（dtb 的 assigned-clock-rates），
 *     48kHz 立体声 16 位时 MDIV = 12288000/(48000*2*16) = 8。
 *
 ****************************************************************************/

static void sai_configure(struct rk3576_sai_dev_s *priv)
{
  uint32_t slots = priv->channels;
  uint32_t sbw   = priv->datawidth;
  uint32_t mdiv;
  uint32_t txcr;

  sai_reset();

  /* I2S 标准格式：MSB 在先、右对齐、帧同步为低表示左声道 */

  txcr = SAI_XCR_VDW(priv->datawidth) |
         SAI_XCR_SBW(sbw) |
         SAI_XCR_SNB(slots) |
         SAI_XCR_CSR(1) |          /* 单条数据线 sdo0 */
         SAI_XCR_FBM_MSB |
         SAI_XCR_VDJ_R;
  sai_putreg(RK3576_SAI_TXCR, txcr);

  /* ★ 接收侧用同一组参数。
   *
   *   TXCR 与 RXCR 位定义相同，但**必须分别写** —— 只配 TXCR 的话录音
   *   方向的位宽/声道数全是复位值，SAI 收不到有意义的数据，而且不报错：
   *   表现就是缓冲区一个都不回来。
   */

  sai_putreg(RK3576_SAI_RXCR, txcr);

  /* 帧宽 = 每帧总位数；脉冲宽取一半，即标准 I2S 的 50% 占空 */

  sai_putreg(RK3576_SAI_FSCR,
             SAI_FSCR_EDGE_RISING |
             SAI_FSCR_FW(slots * sbw) |
             SAI_FSCR_FPW(sbw));

  mdiv = RK3576_SAI_BASECLK_HZ / (priv->samplerate * slots * sbw);
  if (mdiv < 1)
    {
      mdiv = 1;
    }

  sai_putreg(RK3576_SAI_CKR,
             SAI_CKR_MDIV(mdiv) |
             SAI_CKR_MSS_MASTER |   /* SoC 出时钟，codec 作从机 */
             SAI_CKR_CKP_NORMAL |
             SAI_CKR_FSP_NORMAL);

  audinfo("SAI: %" PRIu32 "Hz %dbit %dch mdiv=%" PRIu32 "\n",
          priv->samplerate, priv->datawidth, priv->channels, mdiv);
}

static uint32_t rk3576_sai_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->samplerate = rate;
  return rate;
}

static uint32_t rk3576_sai_txdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->datawidth = bits;
  return bits;
}

static int rk3576_sai_txchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->channels = channels;
  return OK;
}

static uint32_t rk3576_sai_getmclk(struct i2s_dev_s *dev)
{
  UNUSED(dev);
  return RK3576_SAI_BASECLK_HZ;
}

/****************************************************************************
 * Name: rk3576_sai_send
 *
 * Description:
 *   PIO 发送一个缓冲区。整块灌完才回调。
 *
 *   ★ 使能顺序：先开时钟与帧同步，再开发送。
 *     反过来的话，发送通道会在时钟未起时就开始取数据，FIFO 被抽空，
 *     表现为播放起始处有杂音。
 *
 ****************************************************************************/

static uint32_t rk3576_sai_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->samplerate = rate;
  return rate;
}

static uint32_t rk3576_sai_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->datawidth = bits;
  return bits;
}

static int rk3576_sai_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->channels = channels;
  return OK;
}

/****************************************************************************
 * Name: rk3576_sai_receive
 *
 * Description:
 *   把一个缓冲区收满。与 send 对称的轮询实现。
 *
 *   ★ 这个函数原先**根本不存在** —— i2s_ops_s 里只有发送侧，
 *     ES8388 驱动调 I2S_RECEIVE() 拿到的是空指针，于是录音时缓冲区
 *     一个都回不来，而上层没有任何报错。当初这个 SAI 驱动是照放音写的。
 *
 ****************************************************************************/

static int rk3576_sai_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                              i2s_callback_t callback, void *arg,
                              uint32_t timeout)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;
  uint32_t *samples;
  size_t nwords;
  size_t got = 0;
  int ret;

  UNUSED(timeout);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  sai_configure(priv);

  sai_putreg(RK3576_SAI_XFER, SAI_XFER_CLK_EN | SAI_XFER_FSS_EN);
  sai_putreg(RK3576_SAI_XFER,
             SAI_XFER_CLK_EN | SAI_XFER_FSS_EN | SAI_XFER_RXS_EN);

  samples = (uint32_t *)apb->samp;
  nwords  = apb->nmaxbytes / 4;

  /* ★ 一次性诊断：把接收侧真实的寄存器状态打出来。
   *
   *   "FIFO 全程为空"有两种完全不同的原因 —— 时钟没输出（编解码器不被
   *   驱动，自然不发数据），或 RXCR 配错（数据来了但控制器不收）。两者
   *   事后分不开，必须在这里把状态摊开看。
   */

  {
    static bool once = false;

    if (!once)
      {
        once = true;
        syslog(LOG_INFO,
               "SAI RX: 参数 %" PRIu32 "Hz %dbit %dch nwords=%zu\n",
               priv->samplerate, priv->datawidth, priv->channels, nwords);
        syslog(LOG_INFO,
               "SAI RX: XFER=0x%08" PRIx32 " RXCR=0x%08" PRIx32
               " CKR=0x%08" PRIx32 " FSCR=0x%08" PRIx32
               " RXFIFOLR=0x%08" PRIx32 "\n",
               sai_getreg(RK3576_SAI_XFER), sai_getreg(RK3576_SAI_RXCR),
               sai_getreg(RK3576_SAI_CKR), sai_getreg(RK3576_SAI_FSCR),
               sai_getreg(RK3576_SAI_RXFIFOLR));
      }
  }

  /* ★ 上界要设在**整个接收过程**上，不是每个字上。
   *
   *   我第一版给每个字设了 200ms 的等待上界，看起来"有界" —— 但循环要
   *   收 2048 个字，最坏是 2048 x 200ms ≈ 409 秒，实际等于没有上界，
   *   调用方（前台任务）跟着一起没了。
   *
   *   **逐次有界不等于总量有界。** 这个错误在本项目里已经犯到第三次：
   *   先是 nxrecorder 把控制台带走，再是 mic 的 mq_receive 无超时，
   *   现在是这里。上界必须设在"整件事"上。
   *
   *   总预算按数据量算：收满一个缓冲区在正常速率下只要几十毫秒，
   *   给 1 秒足够宽裕；到点就带着已收到的部分返回。
   */

  {
    int budget = 1000000;                 /* 总预算 1 秒，单位 us */

    while (got < nwords && budget > 0)
      {
        if ((sai_getreg(RK3576_SAI_RXFIFOLR) & 0x3f) > 0)
          {
            samples[got++] = sai_getreg(RK3576_SAI_RXDR);
            continue;
          }

        up_udelay(10);
        budget -= 10;
      }

    if (got == 0)
      {
        auderr("SAI: RX FIFO 全程为空 —— ADC 没有输出数据\n");
        ret = -ETIMEDOUT;
      }
    else if (got < nwords)
      {
        audwarn("SAI: 只收到 %zu/%zu 字（预算用尽）\n", got, nwords);
      }
  }

  sai_putreg(RK3576_SAI_XFER, 0);

  apb->nbytes  = got * 4;
  apb->curbyte = 0;

  nxmutex_unlock(&priv->lock);

  if (callback != NULL)
    {
      callback(dev, apb, arg, ret);
    }

  return ret;
}

static int rk3576_sai_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           i2s_callback_t callback, void *arg,
                           uint32_t timeout)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;
  const uint32_t *samples;
  size_t nwords;
  size_t i;
  int us;
  int ret;

  UNUSED(timeout);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  sai_configure(priv);

  sai_putreg(RK3576_SAI_XFER, SAI_XFER_CLK_EN | SAI_XFER_FSS_EN);
  sai_putreg(RK3576_SAI_XFER,
             SAI_XFER_CLK_EN | SAI_XFER_FSS_EN | SAI_XFER_TXS_EN);

  samples = (const uint32_t *)(apb->samp + apb->curbyte);
  nwords  = (apb->nbytes - apb->curbyte) / 4;

  for (i = 0; i < nwords; i++)
    {
      /* FIFO 满时等一等。SAI 的 TXFIFOLR 给出当前水位，
       * 深度按 32 个字计（保守取值，宁可多等）。
       */

      for (us = 0; us < 100000; us++)
        {
          if ((sai_getreg(RK3576_SAI_TXFIFOLR) & 0x3f) < 30)
            {
              break;
            }

          up_udelay(1);
        }

      if (us >= 100000)
        {
          auderr("SAI: FIFO 满超时，已发 %zu/%zu 字\n", i, nwords);
          ret = -ETIMEDOUT;
          break;
        }

      sai_putreg(RK3576_SAI_TXDR, samples[i]);
    }

  /* 等 FIFO 排空再停，否则尾部样本会被截断 */

  for (us = 0; us < 100000; us++)
    {
      if ((sai_getreg(RK3576_SAI_XFER) & SAI_XFER_TX_IDLE) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  sai_putreg(RK3576_SAI_XFER, 0);
  nxmutex_unlock(&priv->lock);

  if (callback != NULL)
    {
      callback(dev, apb, arg, ret);
    }

  return ret;
}

static const struct i2s_ops_s g_sai_ops =
{
  .i2s_txchannels      = rk3576_sai_txchannels,
  .i2s_txsamplerate    = rk3576_sai_txsamplerate,
  .i2s_txdatawidth     = rk3576_sai_txdatawidth,
  .i2s_send            = rk3576_sai_send,
  .i2s_rxchannels      = rk3576_sai_rxchannels,
  .i2s_rxsamplerate    = rk3576_sai_rxsamplerate,
  .i2s_rxdatawidth     = rk3576_sai_rxdatawidth,
  .i2s_receive         = rk3576_sai_receive,
  .i2s_getmclkfrequency = rk3576_sai_getmclk,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_sai_initialize
 *
 * Description:
 *   返回 i2s_dev_s 句柄，供 es8388_initialize() 使用。
 *   调用前须先 rk3576_sai_probe() 成功。
 *
 ****************************************************************************/

struct i2s_dev_s *rk3576_sai_initialize(int port)
{
  struct rk3576_sai_dev_s *priv = &g_sai1;

  if (port != 1)
    {
      return NULL;      /* 板上只用 SAI1 */
    }

  priv->dev.ops    = &g_sai_ops;
  priv->samplerate = 48000;
  priv->datawidth  = 16;
  priv->channels   = 2;
  nxmutex_init(&priv->lock);

  return &priv->dev;
}

int rk3576_sai_probe(void)
{
  uint32_t version;
  int ret;

  /* 1) 电源域。SAI 在 PD_AUDIO 里，U-Boot 不会开它 ——
   *    引导阶段用不到音频。
   */

  ret = rk3576_power_on(RK3576_PD_AUDIO);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SAI: PD_AUDIO 上电失败: %d\n", ret);
      return ret;
    }

  /* 2) 时钟。三路都要开：源时钟、MCLK、以及供寄存器访问的 HCLK。
   *
   *    ★ 只开 HCLK 时寄存器能正常读写，但音频时钟不动 —— 这与 I2C
   *      当初"PCLK 通而功能时钟没开"是同一类陷阱，自检读得到版本号
   *      也不能说明 MCLK 已就绪。
   */

  rk3576_clk_gate(SAI1_GATE_CON, SAI1_GATE_HCLK, true);
  rk3576_clk_gate(SAI1_GATE_CON, SAI1_GATE_MCLK_SRC, true);
  rk3576_clk_gate(SAI1_GATE_CON, SAI1_GATE_MCLK, true);

  /* 3) 引脚复用 */

  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SCLK, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_LRCK, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDO0, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDI0, SAI1_PIN_FUNC);

  /* 4) 自检：读版本寄存器。0 或 0xffffffff 说明电源域、HCLK 或
   *    基址三者之一有问题。
   */

  version = sai_getreg(RK3576_SAI_VERSION);
  if (version == 0 || version == 0xffffffff)
    {
      syslog(LOG_ERR,
             "SAI: VERSION=0x%08" PRIx32 " —— 电源域/HCLK/基址有问题\n",
             version);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "SAI1: VERSION=0x%08" PRIx32 " XFER=0x%08" PRIx32
         " 复用 sclk=gpio%d-%d→%d lrck→%d sdo0→%d sdi0→%d\n",
         version, sai_getreg(RK3576_SAI_XFER),
         SAI1_PIN_BANK, SAI1_PIN_SCLK,
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SCLK),
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_LRCK),
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SDO0),
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SDI0));
  return OK;
}

#endif /* CONFIG_RK3576_SAI */
