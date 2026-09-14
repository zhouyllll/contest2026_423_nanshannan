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
#include <nuttx/wqueue.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/irq.h>
#include <nuttx/clock.h>

#include "arm64_internal.h"
#include "rk3576_sai.h"
#include "rk3576_cru.h"
#include "rk3576_pinmux.h"
#include "rk3576_gpio.h"
#include "rk3576_power.h"
#include "hardware/rk3576_sai.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_pl330.h"
#include "rk3576_pl330.h"

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

/* ★ 送给编解码器的 MCLK 是**另一路时钟、另一个门控**。
 *
 *   上面三个门控让 SAI 控制器自己动起来（寄存器访问、内部主时钟），
 *   但编解码器芯片需要的 MCLK 是从 SoC **输出到引脚**的，走的是
 *   CLK_SAI1_MCLKOUT 这个独立的门控：
 *     GATE(CLK_SAI1_MCLKOUT, "clk_sai1_mclkout", "mclk_sai1_8ch",
 *          RK3576_CLKGATE_CON(9), 13)
 *
 *   漏掉它的现象极具迷惑性：SAI 作为主机照常产生 SCLK/LRCK，FIFO 照常
 *   按采样率填充 —— 但从模式的编解码器没有 MCLK 就不工作，ASDOUT 恒低，
 *   于是**采进来的每一个样本都是 0**。所有寄存器配置看上去都对。
 */

#define SAI1_MCLKOUT_CON     9
#define SAI1_MCLKOUT_BIT     13

/* ★ 复位。出处：原厂 dts 的 resets = <&cru SRST_M_SAI1_8CH>,
 *   <&cru SRST_H_SAI1_8CH>，编号取自 rockchip,rk3576-cru.h。
 *
 *   rockchip_register_softrst() 用的是线性映射（id -> CON(id/16) 的
 *   bit id%16），与本 BSP 的 rk3576_reset() 一致 —— 这个换算已由 SDIO
 *   的 SRST_H_SDIO=684（CON42 bit12）在板上验证过。
 *
 *   本驱动此前**完全没有复位处理**。表现是：寄存器读写正常、使能位也能
 *   回读，但控制器不动 —— SCLK 不输出、RX_DATA_CNT 恒为 0、CLR 的三个
 *   清除位一个都自清不掉（清除操作需要 mclk 域在跑）。
 */

#define SAI1_SRST_M          133
#define SAI1_SRST_H          134

/* 引脚复用 sai1m0，出处：原厂 dtb 的 pinctrl 节点，功能号均为 1 */

#define SAI1_PIN_BANK        4
#define SAI1_PIN_MCLK        2    /* sai1m0_mclk = <4 RK_PA2 1> */
#define SAI1_PIN_SCLK        3
#define SAI1_PIN_LRCK        5
#define SAI1_PIN_SDO0        7
#define SAI1_PIN_SDI0       11
#define SAI1_PIN_FUNC        1

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 接收侧一次性诊断的标记。不用每次都打 —— 见 receive 里的说明。 */

/* ★ 传输完成的回调必须换一个上下文再调。
 *
 *   上层 es8388_processbegin() 持着 pendlock 调 I2S_RECEIVE()，我们若在
 *   同一个栈里回调 es8388_processdone()，它会往驱动自己的消息队列里
 *   file_mq_send() —— 而那个队列**唯一的消费者就是当前这个线程**。
 *   发满 16 条（mq_maxmsg）之后线程永久阻塞在 MQ full，同时还攥着
 *   pendlock，应用线程跟着一起卡死。
 *
 *   实测 ps 的输出把这条锁死了：
 *     PID 8  mic       Waiting Mutex:9
 *     PID 9  es8388    Waiting MQ full
 *
 *   上层的假设是"回调来自中断/DMA 完成上下文"，不是从提交调用里重入。
 *   所以这里把回调投到高优先级工作队列，让 I2S_RECEIVE 先返回、
 *   pendlock 先释放，工作线程才有机会把自己的队列消费掉。
 *
 *   这解释了本会话里几乎所有的"跑到第 17~19 个缓冲区就挂" —— 队列深 16。
 */

#define SAI_NDONE 8

struct sai_done_s
{
  struct work_s     work;
  struct i2s_dev_s *dev;
  struct ap_buffer_s *apb;
  i2s_callback_t    cb;
  void             *arg;
  int               result;
  bool              busy;
};

static struct sai_done_s g_done[SAI_NDONE];

static void sai_done_worker(void *arg)
{
  struct sai_done_s *d = arg;

  if (d->cb != NULL)
    {
      d->cb(d->dev, d->apb, d->arg, d->result);
    }

  d->busy = false;
}

/* 找一个空槽把回调排进工作队列；排不下就退回同步调用（总比丢掉强）。 */

static void sai_post_done(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                          i2s_callback_t cb, void *arg, int result)
{
  irqstate_t flags;
  int i;

  if (cb == NULL)
    {
      return;
    }

  flags = up_irq_save();

  for (i = 0; i < SAI_NDONE; i++)
    {
      if (!g_done[i].busy)
        {
          g_done[i].busy = true;
          break;
        }
    }

  up_irq_restore(flags);

  if (i >= SAI_NDONE)
    {
      syslog(LOG_WARNING, "SAI: 完成槽用尽，退回同步回调\n");
      cb(dev, apb, arg, result);
      return;
    }

  g_done[i].dev    = dev;
  g_done[i].apb    = apb;
  g_done[i].cb     = cb;
  g_done[i].arg    = arg;
  g_done[i].result = result;

  work_queue(HPWORK, &g_done[i].work, sai_done_worker, &g_done[i], 0);
}

static bool g_rx_logged     = false;
static bool g_rx_datalogged = false;
static bool g_rx_nohs_done  = false;
static bool g_rx_cpuprobe   = false;
static bool g_rx_pinprobe   = false;

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
  const uint32_t clr = SAI_CLR_TXC | SAI_CLR_RXC | SAI_CLR_FSC;
  int us;

  /* ★ 三条通道都要清。
   *
   *   初版只写了 TXC|FSC —— 这个驱动最早只有发送侧，接收侧是后补的，
   *   复位这里漏了 RXC，于是接收通道带着上一次的残留状态继续跑。
   *
   *   清位是硬件自清的：写 1 之后要等它变 0。原厂在超时后会退一步做
   *   CRU 级复位（rst_h 再 rst_m），我们这里先如实报告是哪几位没清掉 ——
   *   "复位超时"本身不说明问题在哪，卡住的是哪条通道才说明。
   */

  sai_putreg(RK3576_SAI_CLR, clr);

  for (us = 0; us < 10000; us++)
    {
      if ((sai_getreg(RK3576_SAI_CLR) & clr) == 0)
        {
          return;
        }

      up_udelay(1);
    }

  syslog(LOG_WARNING,
         "SAI: CLR 自清超时 CLR=0x%08" PRIx32 " (%s%s%s)，退到 CRU 复位\n",
         sai_getreg(RK3576_SAI_CLR),
         (sai_getreg(RK3576_SAI_CLR) & SAI_CLR_TXC) ? "TXC " : "",
         (sai_getreg(RK3576_SAI_CLR) & SAI_CLR_RXC) ? "RXC " : "",
         (sai_getreg(RK3576_SAI_CLR) & SAI_CLR_FSC) ? "FSC" : "");

  /* ★ CLR 清不掉就退到 CRU 级复位 —— 这是原厂的做法，不是兜底。
   *
   *   rockchip_sai.c 的 rockchip_sai_clear()：轮询超时后直接调
   *   rockchip_sai_reset() 并**返回成功**。也就是说厂商预期这条路会走到，
   *   CLR 自清本来就依赖 mclk 域在跑，而从模式下外部没有时钟输入时它清
   *   不掉。
   *
   *   之前这里只打一条 LOG_ERR 就往下走，控制器停在没复位完的状态：
   *   RXFIFOLR 恒为 0，DMA 照常搬运，搬的全是 0 —— 表现就是"麦克风采到
   *   的全是静音"，而每一层都不报错。
   *
   *   顺序照 rockchip_sai_reset()：先 hclk 域再 mclk 域，各留 10us。
   *   原厂注释说明了原因：从模式且外部无时钟时，单独复位 mclk 域会失败，
   *   先复位 hclk 域把控制器拉回主机状态，再复位 mclk 域。
   *
   *   复位会清掉寄存器配置 —— 这没问题，sai_reset() 只在 sai_configure()
   *   的开头调用，后面紧接着就把所有寄存器重写一遍。原厂那边对应的是
   *   regcache_sync()。
   */

  rk3576_reset(SAI1_SRST_H, true);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_H, false);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_M, true);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_M, false);
  up_udelay(10);

  /* ★ 复位之后把送给编解码器的 MCLK 门控**重新开一遍**。
   *
   *   实测（复用不动、只设方向的正确量法）：
   *     SAI RX: MCLK(b4-2) 高=0 跳变=0 | SCLK 高=89 跳变=19
   *   同一段代码里 SCLK 在跳、MCLK 恒低 —— 仪器是好的，MCLK 确实没出来。
   *   而回读 CLKGATE_CON(9) = 0x2000，bit13 正是 CLK_SAI1_MCLKOUT，
   *   置 1 在 Rockchip 的语义里就是**关断**。
   *
   *   门控是在初始化里开过的，但那是在 CRU 复位**之前**。这里复位之后
   *   补开一次，并把回读打出来 —— 如果补开之后 MCLK 就跳了，说明复位
   *   （或复位之后的某一步）把它又关上了；如果仍然是 0x2000，那就是
   *   写没落到这个位上，得回去查寄存器地址。
   *
   *   没有 MCLK 的后果很隐蔽：SAI 作为主机照常产生 SCLK/LRCK、FIFO 照常
   *   按采样率填充，但从模式的编解码器不工作，ASDOUT 恒低 —— 采到的每
   *   一个样本都是 0，而每一层都不报错。
   */

  rk3576_clk_gate(SAI1_MCLKOUT_CON, SAI1_MCLKOUT_BIT, true);

  syslog(LOG_INFO, "SAI: CRU 复位后 CLR=0x%08" PRIx32 " XFER=0x%08" PRIx32
         " CLKGATE9=0x%08" PRIx32 "\n",
         sai_getreg(RK3576_SAI_CLR), sai_getreg(RK3576_SAI_XFER),
         getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKGATE_CON(9)));
}


/****************************************************************************
 * i2s_dev_s 实现
 ****************************************************************************/

struct rk3576_sai_dev_s
{
  struct i2s_dev_s dev;          /* 必须是第一个成员 */
  mutex_t          lock;

  /* ★ 收发参数必须分开存。
   *
   *   原先 TX/RX 共用一组字段，结果是：录音时 CONFIGURE 把采样率设成
   *   48000，之后编解码器在别处按默认值 44100 配了发送方向，同一组字段
   *   被覆盖，接收就跟着跑在 44100 上 —— 实测 SAI 拿到的正是 44100。
   *   这类"被另一个方向悄悄改掉"的错误不会报错，只会让时钟对不上。
   */

  uint32_t         txsamplerate;
  int              txdatawidth;
  int              txchannels;

  uint32_t         rxsamplerate;
  int              rxdatawidth;
  int              rxchannels;

  bool             rx_running;   /* 已配置并启动接收，勿重复复位 */

  /* ★ DMA 接收侧（PL330 dmac0，SAI1 RX 请求号 3）。
   *   rxdma 在 appinit 里已 rk3576_pl330_initialize(0)，这里只取句柄；
   *   rxchan 仅在一次 receive 期间占用；rx_result 由中断回调写、等待
   *   线程读，>0 是收到的字节数，<0 是负 errno。
   */

  struct dma_dev_s *rxdma;
  struct dma_chan_s *rxchan;
  sem_t             rx_sem;       /* DMA 完成信号量              */
  bool              rx_seminit;   /* 信号量已初始化              */
  volatile int      rx_result;    /* DMA 完成结果（字节数/负 errno） */
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

/****************************************************************************
 * Name: sai_rx_dma_enable
 *
 * Description:
 *   开/关 SAI1 接收侧 DMA 请求。水位 RDL=8：FIFO 攒满 8 个 32 位 entry
 *   （16 字节）就拉一次请求线，与 PL330 侧 burst=4 x 4B=16B 对齐。
 *   ★ 用 4 而不是 8：Linux 的 rockchip_sai 用 src_maxburst=4，且实测
 *     burst=8 时 LDP 突发读 SAI 端口被总线拒绝（FTC=DATA_READ_ERR）；
 *     4 时内层 8192/16=512 超 PL330 8 位循环上限，由 build_program
 *     自动拆外层循环解决（nper x2 / nburst /2）。
 *   ★ 顺序照 Linux：先 DMACR 后 XFER（先 dma_ctrl(en) 再 xfer_start）。
 ****************************************************************************/

static void sai_rx_dma_enable(bool en)
{
  uint32_t dmacr = sai_getreg(RK3576_SAI_DMACR);

  if (en)
    {
      /* ★ 水位必须 >= 一次突发的量，否则 DMA 会去取 FIFO 里还没有的数据。
       *
       *   突发是 8 个 entry（8 x 4B = 32B，见 receive 里的 OPT_BURST(8)），
       *   而这里原来写的是 RDL(4) —— FIFO 攒到 4 个就拉请求线，DMA 却要
       *   拿 8 个。原厂 rockchip_sai.c 用的是 RDL(16) 配 maxburst 8，
       *   水位是突发的两倍，留足余量。照它来。
       *
       *   顺带：这一行上方的注释本来写着"RDL=8"，和代码里的 4 对不上。
       *   注释描述的是意图，代码才是事实 —— 两者不一致时先信代码。
       */

      dmacr |= SAI_DMACR_RDE | SAI_DMACR_RDL(16) | SAI_DMACR_TDL(16);
    }
  else
    {
      dmacr &= ~SAI_DMACR_RDE;
    }

  sai_putreg(RK3576_SAI_DMACR, dmacr);
}

/****************************************************************************
 * Name: sai_rx_dma_cb
 *
 * Description:
 *   PL330 完成中断回调 —— 运行在 IRQ 上下文。
 *   ★ 只做两件最小的事：记结果、唤醒等待线程。绝不能在这里调上层
 *     （es8388 的回调要发消息、还缓冲区，必须在进程上下文做）。
 ****************************************************************************/

static void sai_rx_dma_cb(FAR struct dma_chan_s *chan, FAR void *arg,
                          ssize_t len)
{
  FAR struct rk3576_sai_dev_s *priv = (FAR struct rk3576_sai_dev_s *)arg;
  static int ncb = 0;

  priv->rx_result = (int)len;

  if (ncb < 3)
    {
      ncb++;

    }

  nxsem_post(&priv->rx_sem);
}

static void sai_configure(struct rk3576_sai_dev_s *priv, bool rx)
{
  uint32_t slots = rx ? priv->rxchannels   : priv->txchannels;
  uint32_t sbw   = rx ? priv->rxdatawidth  : priv->txdatawidth;
  uint32_t rate  = rx ? priv->rxsamplerate : priv->txsamplerate;
  uint32_t mdiv;
  uint32_t slotw;
  uint32_t txcr;

  sai_reset();

  /* I2S 标准格式：MSB 在先、右对齐、帧同步为低表示左声道 */

  /* ★ 标准 I2S 的位对齐，逐项照原厂 rockchip_sai.c 的
   *   SND_SOC_DAIFMT_I2S 分支：
   *
   *     XCR   : VDJ_L + EDGE_SHIFT_1
   *     XSHIFT: RIGHT(2)
   *     FSCR  : EDGE_DUAL，帧脉冲宽 = 半帧
   *
   *   这四项一起表达的是同一件事：**I2S 的数据比帧同步边沿延后一个
   *   BCLK**。我们此前用的是 VDJ_R + EDGE_RISING，且 XSHIFT 寄存器
   *   压根没写 —— 于是控制器在错误的位位置上采样。
   *
   *   这类错误的特征是：时钟对、帧率对、位宽对、所有寄存器回读都正常，
   *   唯独数据不对。它不会报任何错。
   */

  /* ★ 槽宽固定 32 位，不等于有效数据宽度。
   *
   *   原厂录音时读到的是 TXCR/RXCR=0x00400fef、FSCR=0x0101f03f，解出来是
   *   **VDW=16（有效数据 16 位）、SBW=32（槽宽 32 位）、帧宽 64 位**。
   *   我们原来把两者都填成 16，帧只有 32 位 —— 有效数据宽度对、帧结构不对。
   *
   *   ES8388 作为从机跟随 BCLK/LRCK，帧结构不符时 ADC 不输出数据，
   *   而 SAI 照常按自己的帧率把零采进 FIFO。所有寄存器回读都"正确"。
   *
   *   这组值是**从能工作的原厂固件上读回来的**，不是我推导的 —— 这条链
   *   上我推导出的配置已经错过太多次。
   */

  slotw = 32;

  txcr = SAI_XCR_VDW(sbw) |
         SAI_XCR_SBW(slotw) |
         SAI_XCR_SNB(slots) |
         SAI_XCR_CSR(1) |          /* 单条数据线 sdo0/sdi0 */
         SAI_XCR_FBM_MSB |
         SAI_XCR_VDJ_L |
         SAI_XCR_EDGE_SHIFT_1;
  sai_putreg(RK3576_SAI_TXCR, txcr);

  /* ★ 接收侧用同一组参数。
   *
   *   TXCR 与 RXCR 位定义相同，但**必须分别写** —— 只配 TXCR 的话录音
   *   方向的位宽/声道数全是复位值，SAI 收不到有意义的数据，而且不报错：
   *   表现就是缓冲区一个都不回来。
   */

  sai_putreg(RK3576_SAI_RXCR, txcr);

  /* 数据相对帧同步右移 2 拍 —— I2S 的那一拍延迟 */

  sai_putreg(RK3576_SAI_TX_SHIFT, SAI_XSHIFT_RIGHT(2));
  sai_putreg(RK3576_SAI_RX_SHIFT, SAI_XSHIFT_RIGHT(2));

  /* 帧宽 = 每帧总位数；脉冲宽取一半，即标准 I2S 的 50% 占空 */

  sai_putreg(RK3576_SAI_FSCR,
             SAI_FSCR_EDGE_DUAL |
             SAI_FSCR_FW(slots * slotw) |
             SAI_FSCR_FPW(slots * slotw / 2));

  /* ★ 参数非法就大声失败，别默默算下去。
   *
   *   这三个值任一为 0 时，SAI_XCR_VDW(0) 这类 `(x)-1` 编码会算出全 1
   *   的垃圾字段，下面的除法还会除零（ARM64 上不陷入，直接得 0）。
   *   结果是控制器被配成一个无效状态、却没有任何报错 —— 我的环回自检
   *   就是这样跑在一组全 0 的参数上，然后给出了"RX 通路收不到数据"
   *   这个**确定的错误结论**。
   *
   *   仪器必须能报告"我没法工作"，否则它给出的就不是观测而是噪声。
   */

  if (rate == 0 || slots == 0 || sbw == 0)
    {
      syslog(LOG_ERR,
             "SAI: 配置参数非法 rate=%" PRIu32 " slots=%" PRIu32
             " sbw=%" PRIu32 " —— 拒绝配置\n", rate, slots, sbw);
      return;
    }

  /* 分频按**帧的实际位数**算：MCLK / (帧率 x 每帧位数) */

  mdiv = RK3576_SAI_BASECLK_HZ / (rate * slots * slotw);
  if (mdiv < 1)
    {
      mdiv = 1;
    }

  sai_putreg(RK3576_SAI_CKR,
             SAI_CKR_MDIV(mdiv) |
             SAI_CKR_MSS_MASTER |   /* SoC 出时钟，codec 作从机 */
             SAI_CKR_CKP_NORMAL |
             SAI_CKR_FSP_NORMAL);

  audinfo("SAI: %s %" PRIu32 "Hz %" PRIu32 "bit %" PRIu32 "ch mdiv=%" PRIu32 "\n",
          rx ? "RX" : "TX", rate, sbw, slots, mdiv);
}

static uint32_t rk3576_sai_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->txsamplerate = rate;
  return rate;
}

static uint32_t rk3576_sai_txdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->txdatawidth = bits;
  return bits;
}

static int rk3576_sai_txchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->txchannels = channels;
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

  priv->rxsamplerate = rate;
  priv->rx_running   = false;   /* 新的一次配置 = 新的一条流 */
  return rate;
}

static uint32_t rk3576_sai_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->rxdatawidth = bits;
  return bits;
}

static int rk3576_sai_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_dev_s *priv = (struct rk3576_sai_dev_s *)dev;

  priv->rxchannels = channels;
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
  static int nprint = 0;
  struct dma_config_s cfg;
  size_t nwords;
  size_t nbytes;
  irqstate_t flags;
  int ret;
  clock_t t_start;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* ★ 只在第一个缓冲区时配置，之后连续收。
   *   实测每缓冲全 configure 会让 FSC 复位超时（CLR=0x4 清不掉）、
   *   全部缓冲失败；仅首配置时第一个缓冲能稳定 8192/8192。
   */

  if (!priv->rx_running)
    {
      sai_configure(priv, true);
      sai_rx_dma_enable(true);
      /* ★ 打开帧计数器。
       *
       *   RX_DATA_CNT 只有在 XFER 的 RX_CNT_EN(bit5) 使能后才计数。
       *   我之前把"计数器恒 0"当成"SAI 没在收帧"的判据 —— 而计数器根本
       *   没开。**用一个没使能的观测手段下结论，比没有观测更糟**：它给出
       *   的是确定的错误答案，而不是"不知道"。
       *
       *   顺带留意 XFER 的 bit6/7/8 是只读的 FS/TX/RX IDLE 状态位，
       *   读回 0 表示对应状态机**不空闲**，即正在跑。
       */

      sai_putreg(RK3576_SAI_XFER, SAI_XFER_CLK_EN | SAI_XFER_FSS_EN);
      sai_putreg(RK3576_SAI_XFER,
                 SAI_XFER_CLK_EN | SAI_XFER_FSS_EN | SAI_XFER_RXS_EN |
                 SAI_XFER_RX_CNT_EN | SAI_XFER_TX_CNT_EN);
      priv->rx_running = true;
    }

  /* FIFO 满（缓冲间隙攒满）：停 RXS、只清 RXC 通道，再重开。
   * 不清的话满 FIFO 会挡住后续的水位请求边沿。
   */

  if (sai_getreg(RK3576_SAI_RXFIFOLR) & SAI_RXFIFOLR_FULL)
    {
      int us;

      sai_putreg(RK3576_SAI_XFER, SAI_XFER_CLK_EN | SAI_XFER_FSS_EN);
      sai_putreg(RK3576_SAI_CLR, SAI_CLR_RXC);

      for (us = 0; us < 10000; us++)
        {
          if ((sai_getreg(RK3576_SAI_CLR) & SAI_CLR_RXC) == 0)
            {
              break;
            }

          up_udelay(1);
        }

      sai_rx_dma_enable(true);
      sai_putreg(RK3576_SAI_XFER,
                 SAI_XFER_CLK_EN | SAI_XFER_FSS_EN | SAI_XFER_RXS_EN);
    }

  /* DMA 设备：appinit 里已 rk3576_pl330_initialize(0)，这里只取句柄。
   * 接收侧不再忙等 —— PL330 用 DMAWFP 等 SAI1 的 DMA 请求线，
   * FIFO 水位由 DMACR RDL 决定（4 entry = 16 字节 = 一次突发）。
   */

  if (priv->rxdma == NULL)
    {
      priv->rxdma = rk3576_pl330_initialize(0);
      if (priv->rxdma == NULL)
        {
          ret = -ENODEV;
          goto err_unlock;
        }
    }

  if (!priv->rx_seminit)
    {
      nxsem_init(&priv->rx_sem, 0, 0);
      priv->rx_seminit = true;
    }

  nwords = apb->nmaxbytes / 4;
  nbytes = nwords * 4;

  /* ★ 一次性诊断。
   *
   *   定位 DMA 那几处缺陷时，这里每个缓冲区打 8 行 —— 结果串口被灌满，
   *   控制台再也回不来，"看起来又挂了"。**诊断本身成了新的故障源。**
   *   问题解决后脚手架就该拆掉：每条音频缓冲区打日志的驱动不能交付。
   *   留一行足够回答"这条路走通了没有"。
   */

  if (!g_rx_logged)
    {
      g_rx_logged = true;
      syslog(LOG_INFO,
             "SAI RX DMA: %" PRIu32 "Hz %dbit %dch nwords=%zu "
             "XFER=%08" PRIx32 " DMACR=%08" PRIx32 " RXFIFOLR=%08" PRIx32
             "\n",
             priv->rxsamplerate, priv->rxdatawidth, priv->rxchannels, nwords,
             sai_getreg(RK3576_SAI_XFER), sai_getreg(RK3576_SAI_DMACR),
             sai_getreg(RK3576_SAI_RXFIFOLR));
    }

  /* 取通道 + 配置。
   *   方向 DEV_TO_MEM：build_ccr 会自动 置 DSTINC / 清 SRCINC
   *     （内存侧递增、外设 FIFO 端口固定）。
   *   突发 8 x 4B = 32B，DMACR RDL=16（16 个 entry 拉请求线）：
   *     水位取突发的两倍，PL330 每次请求搬走 8 个，FIFO 始终有余量。
   *     8192B / 32B = 256 个突发，恰好不超 PL330 的 8 位循环计数上限。
   */

  priv->rxchan = priv->rxdma->get_chan(priv->rxdma, 0);
  if (priv->rxchan == NULL)
    {
      ret = -ENODEV;
      goto err_unlock;
    }

  /* ★ 首个缓冲区用"不带握手"的方式搬一次，用来把两件事分开：
   *
   *     DMAC 能不能读到 0x2a610034（SAI 的 RXDR）？
   *     外设的 DMA 请求线通不通？
   *
   *   两者失败的现象完全一样 —— 搬回来一片零。用 MEM_TO_MEM + 源地址
   *   固定，微码里就是 LD/ST 而没有 WFP，不依赖任何请求线。
   *     搬回真实数据 → 地址通，问题在握手；
   *     还是零        → DMAC 根本读不到这个地址，握手层面查下去是白费。
   */

  memset(&cfg, 0, sizeof(cfg));

  cfg.direction = g_rx_nohs_done ? DMA_DEV_TO_MEM : DMA_MEM_TO_MEM;
  cfg.src_width = 4;                  /* FIFO entry 32 位 */
  cfg.dst_width = 4;
  cfg.src_drq   = RK3576_DMA_REQ_SAI1_RX;
  cfg.src_step  = 0;                  /* 源是 RXDR 端口，固定 */
  cfg.dst_step  = 4;                  /* 目的是内存，递增 */
  cfg.option    = RK3576_DMA_OPT_BURST(8);

  ret = priv->rxchan->ops->config(priv->rxchan, &cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SAI RX DMA: config 失败 %d\n", ret);
      goto err_chan;
    }

  /* 复位完成标记、清信号量计数，然后启动 DMA。
   * 临界区包住 reset：防止上一次残留计数和这次的完成中断错位。
   */

  /* ★ 同一地址、同一时刻，CPU 读一次做对照。
   *
   *   现在的矛盾是：DMA 的读**弹出了 FIFO 条目**（RXFIFOLR 被排空），
   *   说明访问确实到达了 SAI，但取回的值是 0。是"总线上读不到数据"还是
   *   "FIFO 本来就是空的"，事后完全分不开 —— 必须在同一时刻用 CPU 读
   *   同一个寄存器做对照：
   *     CPU 读到非零、DMA 读到零 → 访问属性/总线视角问题
   *     CPU 也读到零             → FIFO 本来就没数据，方向全错
   */

  /* ★ 先确认编解码器到底有没有在 SDI0 上驱动数据。
   *
   *   到这一步，引脚复用已回读确认、编解码器寄存器已在采集中读过、
   *   RXCR 解码也对 —— 全都指向"配置没问题"，可采样就是零。剩下唯一
   *   没验过的是**那根线上到底有没有信号**。
   *
   *   手法与当初查 GMAC 时钟一致：把引脚临时切回 GPIO 输入，采样若干次，
   *   看高电平比例与跳变次数，然后把复用切回去。
   *     有跳变 → 编解码器在发，问题在 SAI 的采样位置/格式
   *     恒定   → 编解码器根本没输出，回头查它的主从模式与 ADC 输出使能
   *
   *   ★ 注意这会短暂打断接收，只做一次。
   */

  if (!g_rx_pinprobe)
    {
      int hi = 0;
      int tr = 0;
      int prev = -1;
      int k;

      g_rx_pinprobe = true;

      rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDI0, 0);   /* 切回 GPIO */
      rk3576_gpio_setdir(SAI1_PIN_BANK, SAI1_PIN_SDI0, false);

      for (k = 0; k < 200; k++)
        {
          int v = rk3576_gpio_read(SAI1_PIN_BANK, SAI1_PIN_SDI0);

          if (v > 0)
            {
              hi++;
            }

          if (prev >= 0 && v != prev)
            {
              tr++;
            }

          prev = v;
          up_udelay(2);
        }

      rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDI0, SAI1_PIN_FUNC);

      /* ★ 同样量一下 MCLK 与 SCLK 这两根**输出**引脚。
       *
       *   内部有时钟不等于引脚上有时钟：SAI 的 FIFO 按帧率在填，只证明
       *   控制器内部的主时钟在跑；送到编解码器的 MCLK 走的是另一路门控、
       *   另一根引脚，中间任一环节没通，编解码器就没有时钟。
       *
       *   三根一起量才能定位到底断在哪一级：
       *     MCLK 有、SCLK 有、SDI0 无 → 时钟都到了，问题在编解码器内部
       *     MCLK 无                   → 输出这一路没通（门控/选源/分频）
       */

      {
        int mk_hi = 0, mk_tr = 0, mk_prev = -1;
        int sk_hi = 0, sk_tr = 0, sk_prev = -1;
        int q;

        /* ★ 量输出脚**不能先切复用**。
         *
         *   MCLK/SCLK 是 SoC 往外送的。把复用切成 GPIO，SAI 的输出驱动
         *   就从焊盘上断开了 —— 此时读到的必然是恒定电平，跟"时钟有没有
         *   出来"毫无关系。据此得出的"MCLK 跳变=0，时钟没到引脚"是个
         *   **自己造出来的结论**。
         *
         *   本项目 GMAC 那边早就写过正确做法：复用态下 GPIO 输入缓冲仍能
         *   读到焊盘电平。所以保持功能复用不动，只把方向设成输入再采样。
         *
         *   （SDI0 是输入脚，由编解码器驱动，切不切复用都读得到，所以
         *     上面那段不受影响。）
         */

        rk3576_gpio_setdir(SAI1_PIN_BANK, SAI1_PIN_MCLK, false);
        rk3576_gpio_setdir(SAI1_PIN_BANK, SAI1_PIN_SCLK, false);

        for (q = 0; q < 200; q++)
          {
            int mv = rk3576_gpio_read(SAI1_PIN_BANK, SAI1_PIN_MCLK);
            int sv = rk3576_gpio_read(SAI1_PIN_BANK, SAI1_PIN_SCLK);

            if (mv > 0)
              {
                mk_hi++;
              }

            if (sv > 0)
              {
                sk_hi++;
              }

            if (mk_prev >= 0 && mv != mk_prev)
              {
                mk_tr++;
              }

            if (sk_prev >= 0 && sv != sk_prev)
              {
                sk_tr++;
              }

            mk_prev = mv;
            sk_prev = sv;
          }

        /* ★ 先证明这根脚的寄存器通路本身是通的，再谈"时钟没出来"。
         *
         *   到这一步，门控、选源、父时钟（SCLK 在跳即可证明）、引脚功能号
         *   （<4 RK_PA2 1>，与原厂 pinctrl 逐字一致）全部核对过，MCLK 却
         *   恒低。那就有两种可能，必须先分开：
         *     a) 时钟路由的问题 —— 通路是好的，只是没信号
         *     b) 我们对这个引脚的寄存器访问压根没生效 —— 回读一致只是因为
         *        读写用的是同一个错地址，自己和自己对上了
         *
         *   把它切成 GPIO 输出自己推高再推低：电平跟得上就排除 (b)。
         *   测完必须还原成功能复用，否则后面的现象都是自己造出来的。
         */

        {
          int padhi;
          int padlo;

          rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_MCLK,
                            RK3576_PINMUX_GPIO);
          rk3576_gpio_setdir(SAI1_PIN_BANK, SAI1_PIN_MCLK, true);
          rk3576_gpio_write(SAI1_PIN_BANK, SAI1_PIN_MCLK, true);
          up_udelay(50);
          padhi = rk3576_gpio_read(SAI1_PIN_BANK, SAI1_PIN_MCLK);
          rk3576_gpio_write(SAI1_PIN_BANK, SAI1_PIN_MCLK, false);
          up_udelay(50);
          padlo = rk3576_gpio_read(SAI1_PIN_BANK, SAI1_PIN_MCLK);

          rk3576_gpio_setdir(SAI1_PIN_BANK, SAI1_PIN_MCLK, false);
          rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_MCLK, SAI1_PIN_FUNC);

          syslog(LOG_INFO,
                 "SAI RX: MCLK 焊盘自检 推高读到=%d 推低读到=%d —— %s\n",
                 padhi, padlo,
                 (padhi == 1 && padlo == 0) ? "通路正常，恒低是时钟没来" :
                                        "★寄存器通路不通，之前的读数全部作废");
        }

        syslog(LOG_INFO,
               "SAI RX: MCLK(b%d-%d) 高=%d 跳变=%d | SCLK(-%d) 高=%d 跳变=%d\n",
               SAI1_PIN_BANK, SAI1_PIN_MCLK, mk_hi, mk_tr,
               SAI1_PIN_SCLK, sk_hi, sk_tr);
      }

      /* ★ 第三个独立证据源：SAI 自己的帧计数器。
       *
       *   前两个证据互相矛盾 —— FIFO 水位说"有数据在进来"，GPIO 采样说
       *   "三根线全是恒低"。两者必有一个是假的：水位寄存器可能被我解错，
       *   GPIO 输入通路也可能根本没使能。
       *
       *   RX_DATA_CNT 由硬件按收到的帧递增，既不依赖我对水位字段的解码，
       *   也不依赖 GPIO。隔一段时间读两次：
       *     递增 → SAI 真的在收，问题在数据内容
       *     不变 → SAI 根本没在跑，"FIFO 在填"是假象
       */

      /* ★ 先确认这个寄存器块到底是不是活的。
       *
       *   RXFIFOLR 连着读出 0x00555555 / 0x00820820 / 0x00001000 这种
       *   规则位图案 —— 那不是水位值。一个只读的 VERSION 寄存器能不能读出
       *   合理常量，是判断"寄存器块真的在工作"最直接的证据；XFER 能回读
       *   只说明写进去的值被锁存了，不代表控制器活着。
       *
       *   同时把门控、复位、电源域的**寄存器原值**读回来 —— 之前每一步都
       *   只是"调用了设置函数"，从没验证过硬件真的接受了。
       */

      /* ★ 全量 dump，和原厂固件读到的那一份逐条对照。
       *
       *   原厂（录音进行时，/sys/kernel/debug/regmap/2a610000.sai）：
       *     00 TXCR=00400fef  04 FSCR=0101f03f  08 RXCR=00400fef
       *     24 DMACR=010f0010 28 INTCR=00020000 38 PATH_SEL=0000e4e4
       *     64 TX_SHIFT=00000002
       *
       *   逐条比对比"再想一个假设"可靠得多 —— 这条链上我推导出来的配置
       *   已经错过太多次，而这份是从能工作的固件上读回来的。
       */

      {
        static const uint16_t offs[] =
        {
          0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
          0x20, 0x24, 0x28, 0x2c, 0x38, 0x64, 0x68
        };
        char line[256];
        int  pos = 0;
        int  ri;

        for (ri = 0; ri < (int)(sizeof(offs) / sizeof(offs[0])); ri++)
          {
            pos += snprintf(&line[pos], sizeof(line) - pos,
                            "%02x=%08lx ", offs[ri],
                            (unsigned long)sai_getreg(offs[ri]));
          }

        syslog(LOG_INFO, "SAI 全量: %s\n", line);
      }

      syslog(LOG_INFO,
             "SAI RX: VERSION=%08" PRIx32 " | CLKGATE8=%08" PRIx32
             " CLKGATE9=%08" PRIx32 " SOFTRST8=%08" PRIx32 "\n",
             sai_getreg(RK3576_SAI_VERSION),
             getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKGATE_CON(8)),
             getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKGATE_CON(9)),
             getreg32(RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(8)));

      {
        uint32_t c1 = sai_getreg(RK3576_SAI_RX_DATA_CNT);
        uint32_t f1 = sai_getreg(RK3576_SAI_RXFIFOLR);
        uint32_t c2;
        uint32_t f2;

        up_mdelay(5);
        c2 = sai_getreg(RK3576_SAI_RX_DATA_CNT);
        f2 = sai_getreg(RK3576_SAI_RXFIFOLR);

        syslog(LOG_INFO,
               "SAI RX: 5ms 内 RX_DATA_CNT %08" PRIx32 " -> %08" PRIx32
               " | RXFIFOLR %08" PRIx32 " -> %08" PRIx32
               " | XFER=%08" PRIx32 " STATUS=%08" PRIx32 "\n",
               c1, c2, f1, f2,
               sai_getreg(RK3576_SAI_XFER),
               sai_getreg(RK3576_SAI_STATUS));
      }

      syslog(LOG_INFO,
             "SAI RX: SDI0(b%d-%d) 采样 200 次 高=%d 跳变=%d —— %s\n",
             SAI1_PIN_BANK, SAI1_PIN_SDI0, hi, tr,
             tr > 0 ? "有信号" : "恒定，编解码器没在发");
    }

  if (!g_rx_cpuprobe)
    {
      uint32_t before = sai_getreg(RK3576_SAI_RXFIFOLR);
      uint32_t w0 = sai_getreg(RK3576_SAI_RXDR);
      uint32_t w1 = sai_getreg(RK3576_SAI_RXDR);
      uint32_t after = sai_getreg(RK3576_SAI_RXFIFOLR);

      g_rx_cpuprobe = true;
      syslog(LOG_INFO,
             "SAI RX: CPU 直读 RXDR=%08" PRIx32 " %08" PRIx32
             " 水位 %08" PRIx32 " -> %08" PRIx32 "\n",
             w0, w1, before, after);
    }

  /* ★ 启动 DMA 前先 clean 目的缓冲区。
   *
   *   apb->samp 是上层反复复用的缓冲区，CPU 刚读过/写过它，缓存里可能还
   *   留着脏行。DMA 写完 DRAM 之后这些脏行一旦回写，就会盖掉刚搬回来的
   *   数据 —— 表现是"DMA 成功但采到的全是静音"。
   *   实测 PL330 的 mem2mem 自检就是栽在这里（dst 全零）。
   */

  up_clean_dcache((uintptr_t)apb->samp, (uintptr_t)apb->samp + nbytes);

  t_start = clock_systime_ticks();
  priv->rx_result = 0;
  flags = up_irq_save();
  nxsem_reset(&priv->rx_sem, 0);
  up_irq_restore(flags);

  /* ★ 源地址必须是 SAI 的 RXDR 端口，不能是 0。
   *
   *   DMALDP 是"带外设握手的载入"：外设的请求线只决定**什么时候**搬，
   *   **从哪儿搬**仍然取自 SAR。这里一开始传了 0，等于让 DMAC 去读物理
   *   地址 0 —— 而且因为请求线本身也没来，现象是"等待超时"，完全指不到
   *   地址这一层。握手信号和地址是两件独立的事，别因为有握手就以为
   *   地址会自动来。
   */

  ret = priv->rxchan->ops->start(priv->rxchan, sai_rx_dma_cb, priv,
                                 (uintptr_t)apb->samp,
                                 SAI1_BASE + RK3576_SAI_RXDR, nbytes);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SAI RX DMA: start 失败 %d\n", ret);
      goto err_chan;
    }

  /* ★ 等 DMA 完成，超时贴着"这批数据本来需要多久"。
   *   es8388 按 buffer 时间 x2 传下来的 timeout 就是这个量级；若为 0
   *   （异常），给 20ms 保底。等待期间本线程让出 CPU —— 不再忙等，
   *   也就不会再饿死控制台。
   */

  if (timeout == 0)
    {
      timeout = MSEC2TICK(20);
    }

  ret = nxsem_tickwait(&priv->rx_sem, timeout);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SAI RX DMA: 等待超时 ret=%d XFER=%08" PRIx32
                      " DMACR=%08" PRIx32 " RXFIFOLR=%08" PRIx32
                      " INTSR=%08" PRIx32 "\n",
             ret, sai_getreg(RK3576_SAI_XFER), sai_getreg(RK3576_SAI_DMACR),
             sai_getreg(RK3576_SAI_RXFIFOLR), sai_getreg(RK3576_SAI_INTSR));
      syslog(LOG_ERR, "SAI RX DMA: dmac0 CS=%08" PRIx32 " INTEN=%08" PRIx32
                      " INTST=%08" PRIx32 " FSC=%08" PRIx32 "\n"
                      "            CPC0=%08" PRIx32 " SA0=%08" PRIx32
                      " DA0=%08" PRIx32 "\n",
             getreg32(RK3576_DMAC0_BASE + PL330_CS(0)),
             getreg32(RK3576_DMAC0_BASE + PL330_INTEN),
             getreg32(RK3576_DMAC0_BASE + PL330_INTSTATUS),
             getreg32(RK3576_DMAC0_BASE + PL330_FSC),
             getreg32(RK3576_DMAC0_BASE + PL330_CPC(0)),
             getreg32(RK3576_DMAC0_BASE + PL330_SA(0)),
             getreg32(RK3576_DMAC0_BASE + PL330_DA(0)));

      /* 超时：DMA 没等来数据（外设没在跑 / 请求线没到）。如实返回。 */

      priv->rxchan->ops->stop(priv->rxchan);
      priv->rxdma->put_chan(priv->rxdma, priv->rxchan);
      priv->rxchan = NULL;

      apb->nbytes  = 0;
      apb->curbyte = 0;

      nxmutex_unlock(&priv->lock);

      sai_post_done(dev, apb, callback, arg, -ETIMEDOUT);

      return -ETIMEDOUT;
    }

  /* ★ DMA 写内存不走 CPU 的 D-cache —— 收完必须先失效缓存行，
   *   否则 CPU 读到的可能是旧数据（pl330 驱动注释里的硬性要求）。
   */


  if (priv->rx_result > 0)
    {
      up_invalidate_dcache((uintptr_t)apb->samp,
                           (uintptr_t)apb->samp + (size_t)priv->rx_result);
      apb->nbytes  = (size_t)priv->rx_result;
      apb->curbyte = 0;
      /* 一次性：确认搬回来的到底是不是音频数据。 */

      if (!g_rx_datalogged)
        {
          const uint32_t *w = (const uint32_t *)apb->samp;

          g_rx_datalogged = true;
          g_rx_nohs_done  = true;
          syslog(LOG_INFO,
                 "SAI RX DMA: 首缓冲(无握手) %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " %08" PRIx32 " | RXFIFOLR=%08" PRIx32 " INTSR=%08" PRIx32
                 " 用时=%luus\n",
                 w[0], w[1], w[2], w[3],
                 sai_getreg(RK3576_SAI_RXFIFOLR),
                 sai_getreg(RK3576_SAI_INTSR),
                 (unsigned long)(TICK2USEC(clock_systime_ticks() - t_start)));
        }

      ret = OK;
    }
  else
    {
      apb->nbytes  = 0;
      apb->curbyte = 0;
      ret = (priv->rx_result < 0) ? priv->rx_result : -EIO;
    }

  priv->rxdma->put_chan(priv->rxdma, priv->rxchan);
  priv->rxchan = NULL;

  /* 不在这里关 XFER —— 关掉就等于停流，下一个缓冲区又要重来一遍。
   * 停止由 stop 路径负责（后续接上 i2s_stop 时再清 RXC + RDE）。
   */

  nxmutex_unlock(&priv->lock);

  sai_post_done(dev, apb, callback, arg, ret);

  /* 首三个缓冲的收数确认 —— 用 syslog 而不是 audinfo：defconfig 没开
   * CONFIG_DEBUG_AUDIO*，aud 系列打印编译出来全是空的。
   */

  if (nprint < 3)
    {
      nprint++;
      syslog(LOG_INFO, "SAI RX DMA: 第%d个缓冲 %zu/%zu 字节 ret=%d\n",
             nprint, (size_t)apb->nbytes, nbytes, ret);
    }

  return ret;

err_chan:
  priv->rxdma->put_chan(priv->rxdma, priv->rxchan);
  priv->rxchan = NULL;

err_unlock:
  nxmutex_unlock(&priv->lock);

  sai_post_done(dev, apb, callback, arg, ret);

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

  /* 发送方向的 sai_configure() 里含 sai_reset()，控制器一复位接收侧的
   * 配置就没了 —— 必须让下一次 receive 重新配一遍。
   */

  priv->rx_running = false;

  sai_configure(priv, false);

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

  sai_post_done(dev, apb, callback, arg, ret);

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

  priv->dev.ops      = &g_sai_ops;
  priv->txsamplerate = 48000;
  priv->txdatawidth  = 16;
  priv->txchannels   = 2;
  priv->rxsamplerate = 48000;
  priv->rxdatawidth  = 16;
  priv->rxchannels   = 2;
  priv->rx_running   = false;
  priv->rxdma        = NULL;
  priv->rxchan       = NULL;
  priv->rx_seminit   = false;
  nxmutex_init(&priv->lock);

  return &priv->dev;
}

/****************************************************************************
 * Name: sai_loopback_selftest
 *
 * Description:
 *   片内环回自检：把 TX 的数据线在 SAI 内部接到 RX，发一段已知图案再读回。
 *
 *   ★ 为什么需要它
 *
 *     排到这一步，时钟、电源、复位、门控、引脚复用、XCR/FSCR/CKR、
 *     编解码器寄存器全部逐项回读确认无误，帧计数器也证明 SAI 在以
 *     46875Hz 正常跑 —— 唯独收到的数据全是 0。剩下两种可能，从外部
 *     完全分不开：
 *
 *       a) RX 数据通路本身有问题（配置/微码/FIFO 读法）
 *       b) 通路没问题，是外面没人往 SDI0 上送数据
 *
 *     环回把 (b) 摘掉：数据不出芯片，不经过引脚，也不经过编解码器。
 *     读回图案 → RX 通路是好的，去查外部；仍是 0 → 问题在片内。
 *
 *     **这是这条链上第一个我自己能完全控制两端的测试。**
 *
 ****************************************************************************/

static void sai_loopback_selftest(struct rk3576_sai_dev_s *priv)
{
  static const uint32_t pattern[8] =
  {
    0x11112222, 0x33334444, 0x55556666, 0x77778888,
    0x9999aaaa, 0xbbbbcccc, 0xddddeeee, 0x0f0ff0f0
  };

  uint32_t pathsel;
  uint32_t got[8];
  int i;
  int n = 0;

  sai_configure(priv, false);

  /* 开 lp0：PATH_SEL bit18 = 使能，bits[23:22] = 0 表示来源 SDO0 */

  pathsel = sai_getreg(RK3576_SAI_PATH_SEL);
  sai_putreg(RK3576_SAI_PATH_SEL,
             (pathsel & ~(3u << 22)) | (1u << 18));

  /* 先把图案灌进发送 FIFO，再一起使能收发 */

  for (i = 0; i < 8; i++)
    {
      sai_putreg(RK3576_SAI_TXDR, pattern[i]);
    }

  sai_putreg(RK3576_SAI_XFER, SAI_XFER_CLK_EN | SAI_XFER_FSS_EN);
  sai_putreg(RK3576_SAI_XFER,
             SAI_XFER_CLK_EN | SAI_XFER_FSS_EN |
             SAI_XFER_TXS_EN | SAI_XFER_RXS_EN |
             SAI_XFER_TX_CNT_EN | SAI_XFER_RX_CNT_EN);

  /* 8 帧 @46875Hz 约 170us，给 2ms 足够宽裕 */

  up_mdelay(2);

  for (i = 0; i < 8; i++)
    {
      got[i] = sai_getreg(RK3576_SAI_RXDR);

      if (got[i] != 0)
        {
          n++;
        }
    }

  syslog(LOG_INFO,
         "SAI 环回: %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
         " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
         got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
  syslog(LOG_INFO,
         "SAI 环回: 非零 %d/8 TX_CNT=%08" PRIx32 " RX_CNT=%08" PRIx32
         " —— %s\n",
         n, sai_getreg(RK3576_SAI_TX_DATA_CNT),
         sai_getreg(RK3576_SAI_RX_DATA_CNT),
         n > 0 ? "RX 通路正常，问题在片外" : "RX 通路本身收不到数据");

  /* 收摊：关传输、恢复路由 */

  sai_putreg(RK3576_SAI_XFER, 0);
  sai_putreg(RK3576_SAI_PATH_SEL, pathsel);
  priv->rx_running = false;
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

  /* 送到编解码器引脚的那一路 —— 不开它，从模式的 codec 永远不工作 */

  rk3576_clk_gate(SAI1_MCLKOUT_CON, SAI1_MCLKOUT_BIT, true);

  /* ★ 时钟开了还要解复位，顺序照原厂 rockchip_sai_reset()：
   *   先 hclk 域、再 mclk 域，每步之间留 10us。
   *
   *   原厂注释解释了为什么是这个顺序：从模式且外部没有时钟输入时，
   *   mclk 域单独复位会失败，所以先复位 hclk 域把控制器拉回主机状态，
   *   再复位 mclk 域。
   */

  rk3576_reset(SAI1_SRST_H, true);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_H, false);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_M, true);
  up_udelay(10);
  rk3576_reset(SAI1_SRST_M, false);
  up_udelay(10);

  /* ★ 只开门控不等于频率对。
   *
   *   我们一直假设 MCLK = 12.288MHz（256 x 48kHz），因为 dtb 的
   *   assigned-clock-rates 是这么写的 —— 但 assigned-clock-rates 是
   *   **Linux 时钟框架去设**的目标值，不是硬件上电后的现状。我们只开了
   *   门控、从没设过分频，拿到的是引导器留下的值。
   *
   *   实测帧率是 12kHz，正好是 48kHz 的 1/4，反推 MCLK = 3.072MHz，
   *   恰好是 12.288MHz 的 1/4。而 ES8388 配的是 256fs，MCLK/256 = 12kHz
   *   —— 整条链自洽地跑在 12kHz 上，谁都不报错。
   *
   *   所以先把时钟树的现状打出来：
   *     CLKSEL_CON(46) [10:8]=MCLK_SAI1_8CH_SRC 选择，[7:0]=分频，
   *                    [11]=MCLK_SAI1_8CH 再选一次（0=上面这路，1=外部）
   *     CLKSEL_CON(28) 是 clk_audio_int_{0,1,2} 各自的 5 位分频
   *     CLKSEL_CON(12)/(13) 是 clk_audio_frac_0 的小数分频与源选择
   */

  {
    uint32_t sel46 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(46));
    uint32_t sel28 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(28));
    uint32_t sel12 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(12));
    uint32_t sel13 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(13));

    syslog(LOG_INFO,
           "SAI1 时钟: SEL46=%08" PRIx32 " 源=%" PRIu32 " 分频=%" PRIu32
           " 二级选=%" PRIu32 "\n",
           sel46, (sel46 >> 8) & 0x7, (sel46 & 0xff) + 1,
           (sel46 >> 11) & 0x1);
    syslog(LOG_INFO,
           "SAI1 时钟: SEL28=%08" PRIx32 " (audio_int 分频) "
           "SEL12=%08" PRIx32 " SEL13=%08" PRIx32 " (audio_frac_0)\n",
           sel28, sel12, sel13);
  }

  /* 3) 引脚复用 */

  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_MCLK, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SCLK, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_LRCK, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDO0, SAI1_PIN_FUNC);
  rk3576_pinmux_set(SAI1_PIN_BANK, SAI1_PIN_SDI0, SAI1_PIN_FUNC);

  /* ★ 复用写完要回读。
   *
   *   RK3576 的 IOC 寄存器要在高 16 位给写使能掩码，偏移算错时写入被
   *   硬件**静默忽略** —— 函数返回成功，寄存器纹丝不动。本项目在 I2C 上
   *   已经栽过一次（案例 9），当时也正是靠 rk3576_pinmux_get() 的回读
   *   发现的。
   *
   *   发送方向能工作只证明 sclk/lrck/sdo0 三根对了；**SDI0 是接收独有的
   *   第四根**，它没复用上的表现恰恰是"FIFO 在填、但装的全是零"。
   */

  syslog(LOG_INFO,
         "SAI1 引脚回读: mclk(b%d-%d)=%d sclk(-%d)=%d lrck(-%d)=%d "
         "sdo0(-%d)=%d sdi0(-%d)=%d（期望均为 %d）\n",
         SAI1_PIN_BANK, SAI1_PIN_MCLK,
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_MCLK),
         SAI1_PIN_SCLK,
         rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SCLK),
         SAI1_PIN_LRCK, rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_LRCK),
         SAI1_PIN_SDO0, rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SDO0),
         SAI1_PIN_SDI0, rk3576_pinmux_get(SAI1_PIN_BANK, SAI1_PIN_SDI0),
         SAI1_PIN_FUNC);

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

  /* ★ 环回自检要用有效参数跑。
   *
   *   默认值原本只在 rk3576_sai_initialize() 里设，而那是板级胶水层
   *   后来才调的 —— 探测阶段这些字段还是 0。先在这里填好。
   */

  if (g_sai1.txsamplerate == 0)
    {
      g_sai1.txsamplerate = 48000;
      g_sai1.txdatawidth  = 16;
      g_sai1.txchannels   = 2;
      g_sai1.rxsamplerate = 48000;
      g_sai1.rxdatawidth  = 16;
      g_sai1.rxchannels   = 2;
    }

  sai_loopback_selftest(&g_sai1);
  return OK;
}

#endif /* CONFIG_RK3576_SAI */
