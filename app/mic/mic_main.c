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
#include <time.h>

#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>

/* ★ 录音走 pcm1（裸编解码器），不是 pcm0。
 *
 *   pcm0 外面套着 pcm_decode，那是放音用的 WAV 解码器 —— 它对递进来的
 *   缓冲区调 pcm_parsewav()，而录音递的是空缓冲区，解析必然失败，
 *   ENQUEUEBUFFER 直接返回 ENOENT。板级为此把裸设备另注册成 pcm1。
 */

#define MIC_DEV       "/dev/audio/pcm1"
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
  int      nbuf     = MIC_NBUFFERS;
  int      bufbytes = 4096;
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
  /* ★ 字段位置要按**这个驱动实际读的**来，不能照搬别处的用法。
   *
   *   drivers/audio/es8388.c 的 AUDIO_TYPE_INPUT 分支读的是：
   *     ac_controls.hw[0] -> 采样率
   *     ac_controls.b[2]  -> 位深
   *   ac_controls 是联合体，hw[0] 就是 b[0..1]，48000 放得下。
   *
   *   我原来把位深写进了 b[3]、拿 b[2] 放采样率高位，于是驱动读到位深
   *   为 0，直接 -ERANGE。这类错误不会崩、只会拒绝，而拒绝的理由
   *   （"参数超范围"）离真正的原因（字段填错位置）还有一步。
   */

  cap_desc.caps.ac_controls.hw[0] = samprate;
  cap_desc.caps.ac_controls.b[2]  = 16;            /* 位深 */

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

  /* 4) 先告诉驱动要几个缓冲区、每个多大，再申请。
   *
   * ★ 这一步不能省。audio.c 的 audio_allocbuffer() 开头是：
   *
   *     if (upper->periods >= upper->nbuffers)
   *       return 0;          <- 返回**成功**，但什么都没分配
   *
   *   nbuffers 默认为 0，所以不设 BUFFERINFO 时第一次申请就走这条路：
   *   ioctl 返回 0（看起来成功）、pbuffer 却没被填。调用方若直接使用，
   *   就是解引用 NULL —— 表现为 data abort 而不是一条错误信息。
   *   我正是这样崩过一次，之后才加的指针校验。
   */

  {
    struct ap_buffer_info_s binfo;

    /* ★ 必须先 **查询** 缓冲区信息，这一步有副作用。
     *
     *   audio.c 的 AUDIOIOC_GETBUFFERINFO 分支里：
     *       ret = lower->ops->ioctl(...);
     *       if (ret >= 0) upper->nbuffers = arg->nbuffers;
     *
     *   也就是说上半部的 nbuffers 是被这个"查询"操作赋值的。不查的话
     *   nbuffers 恒为 0，而 audio_allocbuffer() 开头就是
     *       if (upper->periods >= upper->nbuffers) return 0;
     *   于是每次申请都"成功"却不分配。
     *
     *   一个 GET 操作承担初始化职责，这个耦合从接口名字上完全看不出来；
     *   SETBUFFERINFO 在本设备上还返回 ENOTTY，更容易让人以为这条路走
     *   不通。这里按驱动实际报的数量来用，不自己拍。
     */

    memset(&binfo, 0, sizeof(binfo));
    if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&binfo) < 0)
      {
        /* 驱动没实现查询：退回自己的默认值，nbuffers 仍是 0，
         * 后面的指针校验会挡住，不会崩。
         */

        printf("GETBUFFERINFO 不支持 (%d)，用默认 %d 个缓冲区\n",
               errno, MIC_NBUFFERS);
        nbuf = MIC_NBUFFERS;
      }
    else
      {
        nbuf = binfo.nbuffers < MIC_NBUFFERS ? binfo.nbuffers : MIC_NBUFFERS;
        bufbytes = binfo.buffer_size ? binfo.buffer_size : 4096;
        printf("缓冲区: 驱动报 %u 个 x %u 字节，使用 %d 个\n",
               binfo.nbuffers, binfo.buffer_size, nbuf);
      }
  }

  memset(bufs, 0, sizeof(bufs));
  for (i = 0; i < nbuf; i++)
    {
      memset(&buf_desc, 0, sizeof(buf_desc));
      buf_desc.numbytes = bufbytes;
      buf_desc.u.pbuffer = &bufs[i];

      /* ★ 只判负值。AUDIOIOC_ALLOCBUFFER 的返回值约定在不同下半部实现里
       *   不一致（有的返回结构体大小、有的返回 0），拿 == sizeof 去比
       *   会把成功当成失败 —— 而且 errno 是 0，报出来的"失败: 0"自相矛盾。
       */

      if (ioctl(fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)&buf_desc) < 0)
        {
          printf("ALLOCBUFFER %d 失败: %d\n", i, errno);
          goto unreg;
        }

      /* ★ 返回成功不等于指针填回来了。
       *
       *   把返回值判断放宽成"只判负值"之后，必须另外确认 pbuffer 真的被
       *   写了 —— 否则后面直接解引用 NULL，表现是 data abort 而不是一条
       *   错误信息，排查成本天差地别。我已经这样崩过一次。
       */

      if (bufs[i] == NULL)
        {
          printf("ALLOCBUFFER %d 返回成功但没给出缓冲区\n", i);
          goto unreg;
        }
    }

  for (i = 0; i < nbuf; i++)
    {
      bufs[i]->nbytes   = 0;
      bufs[i]->curbyte  = 0;
      bufs[i]->flags    = 0;
      memset(&buf_desc, 0, sizeof(buf_desc));
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
        struct timespec ts;
        ssize_t n;

        /* ★ 等待也要有上界。
         *
         *   我原来只用字节数设了上界，但那只有在缓冲区**会回来**时才成立。
         *   驱动若一个都不还（采集没真正跑起来就是这样），mq_receive 会
         *   一直阻塞 —— 而这条命令是前台任务，控制台跟着一起没了。
         *   这正是我做这个命令要避开的东西，第一版却没做到。
         *
         *   两秒收不到一个缓冲区就认定采集没起来，如实报告并退出。
         */

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        n = mq_timedreceive(mq, (FAR char *)&msg, sizeof(msg), &prio, &ts);

        if (n != sizeof(msg))
          {
            printf("等缓冲区超时（已收 %d 个）—— 采集没有产出数据\n", got);
            break;
          }

        if (msg.msg_id == AUDIO_MSG_DEQUEUE)
          {
            FAR struct ap_buffer_s *apb = msg.u.ptr;
            int p;
            int a;

            if (apb == NULL)
              {
                continue;
              }

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
            memset(&buf_desc, 0, sizeof(buf_desc));
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
