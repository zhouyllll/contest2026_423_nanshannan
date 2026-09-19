/****************************************************************************
 * app/kickpi_ui/k7_audio.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * ES8388 录音 / 放音。流程和坑都照 app/mic/mic_main.c（那边每一步为什么
 * 这么写都有注释），这里只保留能用的最短路径：
 *
 *   open pcm1 → RESERVE → CONFIGURE(方向/48k/16bit) → REGISTERMQ →
 *   GETBUFFERINFO（有副作用，必须调）→ ALLOCBUFFER → ENQUEUE → START →
 *   按 mq 收发缓冲区 → STOP → 逆序释放
 *
 * ★ 每一次等待都有上界，整件事也有上界。这是界面线程之外的工作线程，
 *   但卡死一样会把设备占住，之后录音、放音、唤醒全用不了。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>
#include <nuttx/i2c/i2c_master.h>

#include "k7_audio.h"

#define K7A_DEV       "/dev/audio/pcm1"
#define K7A_MAXBUF    4

struct k7a_dev_s
{
  int                 fd;
  mqd_t               mq;
  char                mqname[24];
  int                 nbuf;
  int                 bufbytes;
  struct ap_buffer_s *bufs[K7A_MAXBUF];
};

/* ES8388 在 I2C3:0x10（板级 kickpi_k7_audio.c 已实测） */

#define K7A_CODEC_BUS   "/dev/i2c3"
#define K7A_CODEC_ADDR  0x10

static pthread_mutex_t g_k7a_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   g_k7a_busy;
static int             g_k7a_volume = 70;    /* % */
static int             g_k7a_playfd = -1;    /* 正在放音的设备，调音量用 */

static uint32_t k7a_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void k7a_close(struct k7a_dev_s *d)
{
  struct audio_buf_desc_s desc;
  int i;

  if (d->fd < 0)
    {
      return;
    }

  ioctl(d->fd, AUDIOIOC_STOP, 0);
  if (d->mq != (mqd_t)-1)
    {
      ioctl(d->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)d->mq);
    }

  for (i = 0; i < K7A_MAXBUF; i++)
    {
      if (d->bufs[i] != NULL)
        {
          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = d->bufs[i];
          ioctl(d->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
          d->bufs[i] = NULL;
        }
    }

  ioctl(d->fd, AUDIOIOC_RELEASE, 0);
  if (d->mq != (mqd_t)-1)
    {
      mq_close(d->mq);
      mq_unlink(d->mqname);
    }

  close(d->fd);
  d->fd = -1;
}

static int k7a_open(struct k7a_dev_s *d, uint8_t type)
{
  struct audio_caps_desc_s caps;
  struct audio_buf_desc_s  desc;
  struct ap_buffer_info_s  info;
  struct mq_attr           attr;
  int ret;
  int i;

  memset(d, 0, sizeof(*d));
  d->mq = (mqd_t)-1;
  d->fd = open(K7A_DEV, O_RDWR | O_CLOEXEC);
  if (d->fd < 0)
    {
      return -errno;
    }

  if (ioctl(d->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      ret = -errno;
      close(d->fd);
      d->fd = -1;
      return ret;
    }

  /* ES8388 驱动读 hw[0] 作采样率、b[2] 作位深（见 mic_main.c） */

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len            = sizeof(struct audio_caps_s);
  caps.caps.ac_type           = type;
  caps.caps.ac_channels       = 2;
  caps.caps.ac_controls.hw[0] = K7A_RATE;
  caps.caps.ac_controls.b[2]  = 16;
  if (ioctl(d->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0)
    {
      goto errout;
    }

  snprintf(d->mqname, sizeof(d->mqname), "/tmp/k7a%d",
           (int)(uintptr_t)pthread_self());
  attr.mq_maxmsg  = K7A_MAXBUF + 4;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;
  d->mq = mq_open(d->mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (d->mq == (mqd_t)-1 ||
      ioctl(d->fd, AUDIOIOC_REGISTERMQ, (unsigned long)d->mq) < 0)
    {
      goto errout;
    }

  /* GETBUFFERINFO 会顺带设置上半部的 nbuffers，不调就分不到缓冲区 */

  d->nbuf = K7A_MAXBUF;
  d->bufbytes = 8192;
  memset(&info, 0, sizeof(info));
  if (ioctl(d->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&info) == 0)
    {
      d->nbuf = info.nbuffers < K7A_MAXBUF ? info.nbuffers : K7A_MAXBUF;
      if (info.buffer_size > 0)
        {
          d->bufbytes = info.buffer_size;
        }
    }

  for (i = 0; i < d->nbuf; i++)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes  = d->bufbytes;
      desc.u.pbuffer = &d->bufs[i];
      if (ioctl(d->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc) < 0 ||
          d->bufs[i] == NULL)
        {
          goto errout;
        }
    }

  return 0;

errout:
  ret = errno ? -errno : -EIO;
  k7a_close(d);
  return ret;
}

static int k7a_enqueue(struct k7a_dev_s *d, struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.u.buffer = apb;
  return ioctl(d->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) < 0 ?
         -errno : 0;
}

/* 等驱动还回一个缓冲区，最多 2s。返回 NULL 表示超时或流结束。 */

static struct ap_buffer_s *k7a_dequeue(struct k7a_dev_s *d)
{
  struct audio_msg_s msg;
  struct timespec ts;
  unsigned int prio;

  for (; ; )
    {
      clock_gettime(CLOCK_REALTIME, &ts);   /* mq 的超时按 POSIX 走墙钟 */
      ts.tv_sec += 2;
      if (mq_timedreceive(d->mq, (char *)&msg, sizeof(msg), &prio, &ts)
          != sizeof(msg))
        {
          return NULL;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
          return msg.u.ptr;
        }

      if (msg.msg_id == AUDIO_MSG_STOP || msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          return NULL;
        }
    }
}

static bool k7a_acquire(void)
{
  pthread_mutex_lock(&g_k7a_lock);
  if (g_k7a_busy)
    {
      pthread_mutex_unlock(&g_k7a_lock);
      return false;
    }

  g_k7a_busy = true;
  pthread_mutex_unlock(&g_k7a_lock);
  return true;
}

static void k7a_release(void)
{
  pthread_mutex_lock(&g_k7a_lock);
  g_k7a_busy = false;
  pthread_mutex_unlock(&g_k7a_lock);
}

/* ES8388 的 AUDIO_FU_VOLUME 取 0~1000，驱动内部按 20log10(v/1000) 换成
 * DAC 数字衰减（DACCONTROL4/5，每级 0.5dB）：50% ≈ -6dB，10% = -20dB。
 */

static void k7a_apply_volume(int fd, int percent)
{
  struct audio_caps_desc_s caps;

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len            = sizeof(struct audio_caps_s);
  caps.caps.ac_type           = AUDIO_TYPE_FEATURE;
  caps.caps.ac_format.hw      = AUDIO_FU_VOLUME;
  caps.caps.ac_controls.hw[0] = (uint16_t)(percent * 10);
  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0)
    {
      syslog(LOG_WARNING, "k7a: 设音量失败 %d\n", errno);
    }
}

void k7a_set_volume(int percent)
{
  if (percent < 0)
    {
      percent = 0;
    }
  else if (percent > 100)
    {
      percent = 100;
    }

  pthread_mutex_lock(&g_k7a_lock);
  g_k7a_volume = percent;
  if (g_k7a_playfd >= 0)
    {
      k7a_apply_volume(g_k7a_playfd, percent);
    }

  pthread_mutex_unlock(&g_k7a_lock);
}

int k7a_get_volume(void)
{
  return g_k7a_volume;
}

bool k7a_busy(void)
{
  return g_k7a_busy;
}

static void k7a_codec_write(int fd, uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s xfer;
  uint8_t buf[2];

  buf[0] = reg;
  buf[1] = val;
  msg.frequency = 100000;
  msg.addr      = K7A_CODEC_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;
  xfer.msgv     = &msg;
  xfer.msgc     = 1;
  if (ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer) < 0)
    {
      syslog(LOG_WARNING, "k7a: 写 ES8388 %02x 失败 %d\n", reg, errno);
    }
}

/* ★ 录音增益照原厂：ALC 自动增益 + 噪声门。
 *
 *   上游 es8388 驱动录音时：麦克风 PGA +24dB（0x09=0x88，最大），ALC 关
 *   （0x12=0x38），噪声门关（0x16=0x00）。安静环境底噪平均约 130/32767，
 *   录音机再放大 8~16 倍，用户听到的就是"录音回放噪声很大"。
 *
 *   原厂 Android 录音进行时读回（见 app/mic/mic_main.c 的对照表）：
 *     12:ea 13:c0 14:05 15:06 16:53
 *   ALC 双声道、按目标电平自动调 PGA；噪声门阈值约 -61.5dBFS，低于它
 *   把 ADC 输出静音。原厂 0x09=0x00 是交给 ALC 管的起始值。
 *
 *   输入通道（0x0A = LIN2/RIN2）不动：板上麦克风座接的是 LIN2，已实测。
 *   必须在 START 之后写：es8388_start() 会重写 ADC 相关寄存器。
 */

static void k7a_codec_tune_capture(void)
{
  static const uint8_t regs[][2] =
  {
    { 0x09, 0x00 },

    /* ★ 差分输入：板上麦克风是一对差分线 MIC2P/MIC2N，接 LIN2/RIN2。
     *   上游驱动的 LINE2 是单端（0x0A=0x50），只取 MIC2P 对地，50Hz
     *   工频这类共模干扰原样进来 —— 实测一句话里 100Hz 以下占能量的
     *   99.8%、最强频点 50Hz，用户听到"电流声比说话声还大"。驱动自带的
     *   DIFFERENTIAL 选项又把差分对写死成 LIN1-RIN1（耳机座），不能用。
     *     0x0A = LINSEL=差分 | RINSEL=差分
     *     0x0B = DS(bit7)=LIN2-RIN2 | bit1 默认值
     */

    { 0x0a, 0xf0 },
    { 0x0b, 0x82 },
    { 0x12, 0xea },
    { 0x13, 0xc0 },
    { 0x14, 0x05 },
    { 0x15, 0x06 },
    { 0x16, 0x53 },
  };

  int fd = open(K7A_CODEC_BUS, O_RDWR | O_CLOEXEC);
  size_t i;

  if (fd < 0)
    {
      syslog(LOG_WARNING, "k7a: 打不开 %s，录音增益保持驱动默认\n",
             K7A_CODEC_BUS);
      return;
    }

  for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
    {
      k7a_codec_write(fd, regs[i][0], regs[i][1]);
    }

  close(fd);
}

int k7a_capture(k7a_capture_cb_t cb, void *priv, int max_ms)
{
  struct k7a_dev_s d;
  uint32_t deadline;
  size_t skipped = 0;
  int ret;
  int i;

  if (!k7a_acquire())
    {
      return -EBUSY;
    }

  ret = k7a_open(&d, AUDIO_TYPE_INPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "k7a: 打开录音失败 %d\n", ret);
      k7a_release();
      return ret;
    }

  for (i = 0; i < d.nbuf; i++)
    {
      d.bufs[i]->nbytes = 0;
      d.bufs[i]->curbyte = 0;
      d.bufs[i]->flags = 0;
      k7a_enqueue(&d, d.bufs[i]);
    }

  if (ioctl(d.fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      goto out;
    }

  k7a_codec_tune_capture();

  deadline = k7a_now_ms() + (uint32_t)max_ms;
  ret = 0;
  while ((int32_t)(deadline - k7a_now_ms()) > 0)
    {
      struct ap_buffer_s *apb = k7a_dequeue(&d);
      bool more;

      if (apb == NULL)
        {
          syslog(LOG_ERR, "k7a: 2s 没收到录音数据\n");
          ret = -ETIMEDOUT;
          break;
        }

      /* ★ 开头 500ms 不交给调用方。
       *
       *   START 之后第一块是满幅爆音（`mic 1` 实测 peak=32767）；写完 ALC
       *   寄存器后 ALC 起步还有一段增益收敛的冲击：丢 150ms 时实测每 100ms
       *   峰值 9934 → 3184 → 1216 → 509 → 229，约 400ms 才落到安静时的
       *   25~65。交出去的话录音机开头"砰"一声、归一化也被它
       *   带偏，唤醒的端点检测也会被它触发。
       */

      if (skipped < K7A_RATE * 500 / 1000)
        {
          skipped += apb->nbytes / 4;
          more = true;
        }
      else
        {
          more = cb((const int16_t *)apb->samp, apb->nbytes / 4, priv);
        }

      apb->nbytes = 0;
      apb->curbyte = 0;
      apb->flags = 0;
      k7a_enqueue(&d, apb);

      if (!more)
        {
          break;
        }
    }

out:
  k7a_close(&d);
  k7a_release();
  return ret;
}

/* 从 mono[*pos] 起填一个缓冲区（复制到两个声道），不够的补零 */

static void k7a_fill(struct ap_buffer_s *apb, int bufbytes,
                     const int16_t *mono, size_t frames, size_t *pos)
{
  int16_t *st = (int16_t *)apb->samp;
  size_t n = (size_t)bufbytes / 4;
  size_t i;

  for (i = 0; i < n; i++)
    {
      int16_t v = *pos < frames ? mono[(*pos)++] : 0;

      st[2 * i]     = v;
      st[2 * i + 1] = v;
    }

  apb->nbytes  = n * 4;
  apb->curbyte = 0;

  /* 数据到头的这一块标成最后一块：SAI 发送据此排空后停下，缓冲之间
   * 则一直不停（见 rk3576_sai_send）。
   */

  apb->flags   = *pos >= frames ? AUDIO_APB_FINAL : 0;
}

int k7a_play_mono(const int16_t *mono, size_t frames)
{
  struct k7a_dev_s d;
  size_t pos = 0;
  int inflight = 0;
  int ret;
  int i;

  if (!k7a_acquire())
    {
      return -EBUSY;
    }

  ret = k7a_open(&d, AUDIO_TYPE_OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "k7a: 打开放音失败 %d\n", ret);
      k7a_release();
      return ret;
    }

  pthread_mutex_lock(&g_k7a_lock);
  k7a_apply_volume(d.fd, g_k7a_volume);
  g_k7a_playfd = d.fd;
  pthread_mutex_unlock(&g_k7a_lock);

  for (i = 0; i < d.nbuf && pos < frames; i++)
    {
      k7a_fill(d.bufs[i], d.bufbytes, mono, frames, &pos);
      if (k7a_enqueue(&d, d.bufs[i]) == 0)
        {
          inflight++;
        }
    }

  if (ioctl(d.fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      goto out;
    }

  /* 数据送完之后还要等在途的缓冲区放完，不然最后几十毫秒会被 STOP 截掉 */

  while (inflight > 0)
    {
      struct ap_buffer_s *apb = k7a_dequeue(&d);

      if (apb == NULL)
        {
          ret = -ETIMEDOUT;
          break;
        }

      inflight--;
      if (pos < frames)
        {
          k7a_fill(apb, d.bufbytes, mono, frames, &pos);
          if (k7a_enqueue(&d, apb) == 0)
            {
              inflight++;
            }
        }
    }

out:
  pthread_mutex_lock(&g_k7a_lock);
  g_k7a_playfd = -1;
  pthread_mutex_unlock(&g_k7a_lock);
  k7a_close(&d);
  k7a_release();
  return ret;
}
