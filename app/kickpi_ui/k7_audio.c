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

static pthread_mutex_t g_k7a_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   g_k7a_busy;

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

bool k7a_busy(void)
{
  return g_k7a_busy;
}

int k7a_capture(k7a_capture_cb_t cb, void *priv, int max_ms)
{
  struct k7a_dev_s d;
  uint32_t deadline;
  bool first = true;
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

      /* ★ START 之后第一块是满幅的爆音（`mic 1` 实测第 1 块 peak=32767，
       *   之后都是 100~370 的底噪），不交给调用方 —— 否则录音机的
       *   峰值归一化会被它顶到 1 倍，唤醒的端点检测也会被它触发。
       */

      more = first ? true :
             cb((const int16_t *)apb->samp, apb->nbytes / 4, priv);
      first = false;

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
  k7a_close(&d);
  k7a_release();
  return ret;
}
