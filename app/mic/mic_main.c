/****************************************************************************
 * app/mic/mic_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 麦克风采集自检：录一段固定长度的 PCM，直接给出信号统计。
 *
 * ★ 为什么不用 nxrecorder。
 *
 *   nxrecorder / nxlooper / ai_agent 都是**带自己提示符的前台程序**，
 *   内部一阻塞就把控制台一起带走，连 loader 都发不进去，板子只能复位。
 *   这个项目已经因此复位过三次（见 DEBUG-CASES 案例 21/23）。
 *
 *   而且它们的设备协商还要靠自动搜索与格式匹配 —— 中间任何一环不匹配，
 *   得到的都是同一句 "No suitable Audio Device found"，分不出是设备
 *   不支持、格式不对、还是搜索逻辑本身有毛病。
 *
 * ★ 本命令只做三件事：按原厂实测的参数配好、录固定秒数、算统计量。
 *   有界、不占提示符、**结论是数字而不是人耳**：
 *
 *     峰值接近 0        -> 麦克风没采到（ADC 没上电 / 输入选错 / 没接线）
 *     峰值有值但很小     -> 采到了但增益不足
 *     峰值大且随说话变化 -> 通了
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <mqueue.h>

#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>

#define MIC_DEV       "/dev/audio/pcm0"
#define MIC_NBUFFERS  4

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 统计一段 16 位有符号 PCM 的绝对峰值与平均能量 */

static void mic_stats(FAR const int16_t *s, size_t n,
                      FAR int *peak, FAR int *avg)
{
  int64_t acc = 0;
  int     mx  = 0;
  size_t  i;

  for (i = 0; i < n; i++)
    {
      int v = s[i] < 0 ? -s[i] : s[i];

      if (v > mx)
        {
          mx = v;
        }

      acc += v;
    }

  *peak = mx;
  *avg  = n ? (int)(acc / (int64_t)n) : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct audio_caps_desc_s cap_desc;
  struct audio_buf_desc_s  buf_desc;
  struct ap_buffer_s      *bufs[MIC_NBUFFERS];
  struct mq_attr           attr;
  struct audio_msg_s       msg;
  char                     mqname[24];
  mqd_t                    mq = (mqd_t)-1;
  FAR FILE                *fp = NULL;
  FAR const char          *path = NULL;
  int      seconds  = 5;
  int      samprate = 48000;
  int      nchan    = 2;
  int      fd       = -1;
  int      status   = 1;
  int      got      = 0;
  int      peak     = 0;
  int      avg      = 0;
  int      total    = 0;
  unsigned int prio;
  int      i;

  if (argc > 1)
    {
      seconds = atoi(argv[1]);
      if (seconds <= 0)
        {
          seconds = 5;
        }
    }

  if (argc > 2)
    {
      path = argv[2];
    }

  printf("麦克风: 录 %d 秒 %dch/16bit/%dHz", seconds, nchan, samprate);
  if (path)
    {
      printf("，存到 %s", path);
    }

  printf("\n");

  fd = open(MIC_DEV, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      printf("打不开 %s: %d\n", MIC_DEV, errno);
      return 1;
    }

  /* 1) 占用设备 */

  if (ioctl(fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("RESERVE 失败: %d\n", errno);
      goto out;
    }

  /* 2) 按录音方向配置。
   *
   * ★ ac_type 必须是 AUDIO_TYPE_INPUT —— 这是"录"和"放"的唯一区别，
   *   写成 OUTPUT 的话设备会配成放音，录出来自然全是静音，而且不报错。
   */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels       = nchan;
  cap_desc.caps.ac_chmap          = 0;
  cap_desc.caps.ac_controls.hw[0] = samprate;
  cap_desc.caps.ac_controls.b[2]  = samprate >> 16;
  cap_desc.caps.ac_controls.b[3]  = 16;            /* 位深 */

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      printf("CONFIGURE 失败: %d —— 设备不接受这组录音参数\n", errno);
      goto release;
    }

  /* 3) 消息队列：驱动通过它把装满的缓冲区还给我们 */

  snprintf(mqname, sizeof(mqname), "/tmp/mic%d", (int)getpid());
  attr.mq_maxmsg  = MIC_NBUFFERS + 4;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;

  mq = mq_open(mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (mq == (mqd_t)-1)
    {
      printf("mq_open 失败: %d\n", errno);
      goto release;
    }

  if (ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq) < 0)
    {
      printf("REGISTERMQ 失败: %d\n", errno);
      goto release;
    }

  /* 4) 申请缓冲区并全部入队 */

  memset(bufs, 0, sizeof(bufs));
  for (i = 0; i < MIC_NBUFFERS; i++)
    {
      buf_desc.numbytes = 4096;
      buf_desc.u.pbuffer = &bufs[i];

      if (ioctl(fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)&buf_desc) != sizeof(buf_desc))
        {
          printf("ALLOCBUFFER %d 失败: %d\n", i, errno);
          goto unreg;
        }
    }

  for (i = 0; i < MIC_NBUFFERS; i++)
    {
      bufs[i]->nbytes   = 0;
      bufs[i]->curbyte  = 0;
      bufs[i]->flags    = 0;
      buf_desc.u.buffer = bufs[i];

      if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER,
                (unsigned long)&buf_desc) < 0)
        {
          printf("ENQUEUEBUFFER %d 失败: %d\n", i, errno);
          goto unreg;
        }
    }

  if (path)
    {
      fp = fopen(path, "wb");
    }

  /* 5) 开始采集 */

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("START 失败: %d\n", errno);
      goto unreg;
    }

  /* 6) 收缓冲区。总量按采样率算，收够就停 —— **有上界**，
   *    麦克风没信号也不会卡在这里。
   */

  {
    int want = seconds * samprate * nchan * 2;

    while (total < want)
      {
        ssize_t n = mq_receive(mq, (FAR char *)&msg, sizeof(msg), &prio);

        if (n != sizeof(msg))
          {
            break;
          }

        if (msg.msg_id == AUDIO_MSG_DEQUEUE)
          {
            FAR struct ap_buffer_s *apb = msg.u.ptr;
            int p;
            int a;

            mic_stats((FAR const int16_t *)apb->samp,
                      apb->nbytes / 2, &p, &a);

            if (p > peak)
              {
                peak = p;
              }

            avg   += a;
            got++;
            total += apb->nbytes;

            if (fp)
              {
                fwrite(apb->samp, 1, apb->nbytes, fp);
              }

            apb->nbytes  = 0;
            apb->curbyte = 0;
            apb->flags   = 0;
            buf_desc.u.buffer = apb;
            ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
          }
        else if (msg.msg_id == AUDIO_MSG_STOP ||
                 msg.msg_id == AUDIO_MSG_COMPLETE)
          {
            break;
          }
      }
  }

  ioctl(fd, AUDIOIOC_STOP, 0);

  if (fp)
    {
      fclose(fp);
      fp = NULL;
    }

  /* 7) 判据 */

  if (got == 0)
    {
      printf("一个缓冲区都没收到 —— 采集根本没跑起来\n");
      goto unreg;
    }

  avg /= got;
  printf("收到 %d 个缓冲区 共 %d 字节\n", got, total);
  printf("信号: 峰值=%d 平均=%d（16 位满量程 32767）\n", peak, avg);

  if (peak < 32)
    {
      printf("★ 基本是静音 —— ADC 没上电、输入选错、或麦克风没接\n");
    }
  else if (peak < 512)
    {
      printf("★ 有信号但很弱 —— 通路是通的，增益或偏置不足\n");
    }
  else
    {
      printf("★ 麦克风采集正常\n");
    }

  status = 0;

unreg:
  ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)mq);

  for (i = 0; i < MIC_NBUFFERS; i++)
    {
      if (bufs[i])
        {
          buf_desc.u.buffer = bufs[i];
          ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
        }
    }

release:
  ioctl(fd, AUDIOIOC_RELEASE, 0);

out:
  if (fp)
    {
      fclose(fp);
    }

  if (mq != (mqd_t)-1)
    {
      mq_close(mq);
      mq_unlink(mqname);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  printf("ret=%d\n", status ? -1 : 0);
  return status;
}
