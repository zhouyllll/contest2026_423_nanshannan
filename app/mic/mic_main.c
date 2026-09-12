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

#include <malloc.h>
#include <nuttx/i2c/i2c_master.h>
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

/****************************************************************************
 * Name: mic_dump_codec
 *
 * Description:
 *   采集进行时读一遍 ES8388 的关键寄存器。
 *
 *   ★ 为什么必须在 START 之后读。
 *
 *     之前读到 ADCPOWER=0x09（MICBIAS 关）是在 mic **跑完之后** —— 那是
 *     es8388_stop() 写下的关断状态，不代表采集时的状态。我据此判断过
 *     "MICBIAS 没开"，又因为读驱动代码看到 es8388_start() 会开而撤回。
 *     两次都是在推断，没有一次是测量。
 *
 *     **取样时机本身就是判据的一部分。** 采集时的状态只能在采集时读。
 *
 ****************************************************************************/

static void mic_dump_codec(void)
{
  /* ★ 全量 0x00-0x34，和原厂固件读到的那份逐条对照。
   *
   *   原厂（录音进行时，/sys/kernel/debug/regmap/3-0010）：
   *     00:36 01:60 02:00 03:09 04:c0 05:00 06:00 07:7c
   *     08:00 09:00 0a:00 0b:02 0c:4c 0d:02 0e:30 0f:30
   *     10:00 11:00 12:ea 13:c0 14:05 15:06 16:53 17:18
   *     18:02 19:02 ... 2b:80 ... 33:aa 34:aa
   *
   *   放音已证明数字链路在我们的配置下是通的（喇叭出声），所以差异只会
   *   在 ADC 侧的寄存器里。挑着看已经挑了太多轮，全量对一次更省。
   */

  static uint8_t regs[0x35];

  struct i2c_msg_s msg[2];
  struct i2c_transfer_s xfer;
  uint8_t regaddr;
  uint8_t val;
  int fd;
  int i;

  fd = open("/dev/i2c3", O_RDONLY);
  if (fd < 0)
    {
      printf("[codec] 打不开 /dev/i2c3: %d\n", errno);
      return;
    }

  for (i = 0; i <= 0x34; i++)
    {
      regs[i] = (uint8_t)i;
    }

  printf("[codec] ES8388@0x10 采集中：");

  for (i = 0; i <= 0x34; i++)
    {
      regaddr = regs[i];

      msg[0].frequency = 100000;
      msg[0].addr      = 0x10;
      msg[0].flags     = 0;
      msg[0].buffer    = &regaddr;
      msg[0].length    = 1;

      msg[1].frequency = 100000;
      msg[1].addr      = 0x10;
      msg[1].flags     = I2C_M_READ;
      msg[1].buffer    = &val;
      msg[1].length    = 1;

      xfer.msgv = msg;
      xfer.msgc = 2;

      if (ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer) < 0)
        {
          printf(" %02x=??", regaddr);
          continue;
        }

      printf("%s%02x:%02x", (i % 8) == 0 ? "\n  " : " ", regaddr, val);
    }

  printf("\n");
  close(fd);
  fflush(stdout);
}

/* 1kHz 正弦，16 位立体声 48kHz：每 48 个样点一个周期 */

static void mic_fill_sine(FAR struct ap_buffer_s *apb, int nbytes,
                          FAR int *phase)
{
  static const int16_t wave[48] =
  {
        0,  4276,  8480, 12539, 16383, 19947, 23169, 25995,
    28377, 30272, 31650, 32486, 32767, 32486, 31650, 30272,
    28377, 25995, 23169, 19947, 16383, 12539,  8480,  4276,
        0, -4276, -8480,-12539,-16383,-19947,-23169,-25995,
   -28377,-30272,-31650,-32486,-32767,-32486,-31650,-30272,
   -28377,-25995,-23169,-19947,-16383,-12539, -8480, -4276
  };

  FAR int16_t *p = (FAR int16_t *)apb->samp;
  int n = nbytes / 2;
  int i;

  for (i = 0; i < n; i += 2)
    {
      int16_t v = wave[*phase];

      p[i]     = v;          /* 左 */
      p[i + 1] = v;          /* 右 */
      *phase   = (*phase + 1) % 48;
    }

  apb->nbytes  = nbytes;
  apb->curbyte = 0;
  apb->flags   = 0;
}

/****************************************************************************
 * Name: mic_tone
 *
 * Description:
 *   往编解码器送一段正弦波。`mic tone [秒数]`
 *
 *   ★ 为什么把它做进这个命令
 *
 *     录音方向查到现在，SoC 侧（SAI 环回原样回读）与编解码器侧（寄存器
 *     逐项在采集中读过）都已验证正确，采到的却全是 0。剩下的分歧是：
 *     我们的固件到底有没有真的把 MCLK/SCLK/LRCK 送到编解码器？
 *
 *     原厂固件在同一块板上能出声，说明**硬件通路没问题**；但那不能证明
 *     我们的配置也让它跑起来了。放音是唯一一个**人耳可判、不依赖任何
 *     片上观测手段**的证据 —— 而这条链上我已经连续用坏了四个片上探针。
 *
 *     有声  → 时钟链在我们的配置下也通，问题只在 ASDOUT/ADC 方向
 *     无声  → 收发两个方向是同一个根因，优先查时钟输出
 *
 *   用 /dev/audio/pcm1（裸编解码器）。pcm0 外面套着 pcm_decode，
 *   那是 WAV 解码器，喂裸 PCM 会被它拒掉。
 *
 ****************************************************************************/

static int mic_tone(int seconds)
{
  struct audio_caps_desc_s cap_desc;
  struct audio_buf_desc_s  buf_desc;
  struct ap_buffer_s      *bufs[MIC_NBUFFERS];
  struct mq_attr           attr;
  struct audio_msg_s       msg;
  char     mqname[24];
  mqd_t    mq = (mqd_t)-1;
  int      fd = -1;
  int      nbuf = MIC_NBUFFERS;
  int      bufbytes = 4096;
  int      sent = 0;
  int      want;
  unsigned int prio;
  int      phase = 0;
  int      i;
  int      ret = 1;

  printf("[tone] open...\n"); fflush(stdout);
  fd = open("/dev/audio/pcm1", O_RDWR);
  if (fd < 0)
    {
      printf("打不开 /dev/audio/pcm1: %d\n", errno);
      return 1;
    }

  printf("[tone] RESERVE...\n"); fflush(stdout);
  if (ioctl(fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("RESERVE 失败: %d\n", errno);
      goto out;
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels       = 2;
  cap_desc.caps.ac_controls.hw[0] = 48000;
  cap_desc.caps.ac_controls.b[2]  = 16;

  printf("[tone] CONFIGURE...\n"); fflush(stdout);
  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      printf("CONFIGURE 失败: %d\n", errno);
      goto release;
    }
  printf("[tone] CONFIGURE ok\n"); fflush(stdout);

  snprintf(mqname, sizeof(mqname), "/tone%d", (int)getpid());
  attr.mq_maxmsg  = 16;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;
  mq = mq_open(mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (mq == (mqd_t)-1)
    {
      printf("mq_open 失败: %d\n", errno);
      goto release;
    }

  ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq);

  {
    struct ap_buffer_info_s info;

    if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&info) == OK)
      {
        nbuf     = info.nbuffers < MIC_NBUFFERS ? info.nbuffers : MIC_NBUFFERS;
        bufbytes = info.buffer_size;
      }
  }

  printf("[tone] ALLOC %d x %d...\n", nbuf, bufbytes); fflush(stdout);
  for (i = 0; i < nbuf; i++)
    {
      memset(&buf_desc, 0, sizeof(buf_desc));
      buf_desc.numbytes  = bufbytes;
      buf_desc.u.pbuffer = &bufs[i];

      if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc) < 0 ||
          bufs[i] == NULL)
        {
          printf("ALLOCBUFFER %d 失败: %d\n", i, errno);
          goto unreg;
        }
    }

  want = seconds * 48000 * 2 * 2;
  printf("发正弦波 1kHz，%d 秒，共 %d 字节 -> /dev/audio/pcm1\n",
         seconds, want);

  /* 先把所有缓冲区填满并递进去 */

  for (i = 0; i < nbuf; i++)
    {
      mic_fill_sine(bufs[i], bufbytes, &phase);
      memset(&buf_desc, 0, sizeof(buf_desc));
      buf_desc.u.buffer = bufs[i];
      ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
      sent += bufbytes;
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("START 失败: %d\n", errno);
      goto unreg;
    }

  mic_dump_codec();

  while (sent < want)
    {
      struct timespec ts;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;

      if (mq_timedreceive(mq, (FAR char *)&msg, sizeof(msg), &prio, &ts)
          != sizeof(msg))
        {
          printf("等缓冲区超时（已送 %d 字节）\n", sent);
          break;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
          FAR struct ap_buffer_s *apb = msg.u.ptr;

          mic_fill_sine(apb, bufbytes, &phase);
          memset(&buf_desc, 0, sizeof(buf_desc));
          buf_desc.u.buffer = apb;
          ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
          sent += bufbytes;
        }
      else if (msg.msg_id == AUDIO_MSG_STOP)
        {
          break;
        }
    }

  ioctl(fd, AUDIOIOC_STOP, 0);
  printf("放音结束，共送 %d 字节。听到声音了吗？\n", sent);
  ret = 0;

unreg:
  ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)mq);

  for (i = 0; i < nbuf; i++)
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
  if (mq != (mqd_t)-1)
    {
      mq_close(mq);
      mq_unlink(mqname);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return ret;
}

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

  if (argc > 1 && strcmp(argv[1], "tone") == 0)
    {
      return mic_tone(argc > 2 ? atoi(argv[2]) : 2);
    }

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

  mic_dump_codec();

  /* 6) 收缓冲区。总量按采样率算，收够就停 —— **有上界**，
   *    麦克风没信号也不会卡在这里。
   */

  {
    int want = seconds * samprate * nchan * 2;
    struct timespec deadline;

    /* ★ 整件事也要有上界，不只是每次等待。
     *
     *   前一版给 mq_timedreceive 设了 2 秒超时，看着是有界的。但只要驱动
     *   每次都还回**一点点**数据，循环就一直在"有进展"，凑够 want 字节
     *   可以走上几个小时 —— 而这是前台命令，控制台跟着一起没了。
     *
     *   逐次有界 != 总量有界。上界必须设在整件事上：录 N 秒最多花 3N+5 秒，
     *   到点就带着已有的数据如实收摊。
     */

    /* ★ 上界用单调时钟，不用墙钟。
     *
     *   墙钟会被对时改动（本项目案例 20 就栽在这里：agent 用墙钟量耗时，
     *   撞上 TLS 握手改钟，把正常返回误判成超时）。这块板现在有 RTC，
     *   启动时会对一次时间 —— 墙钟往前跳，deadline 就永远到不了；
     *   往后跳则立刻超时。**deadline 要的是"过了多久"，那是单调时钟。**
     */

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds * 3 + 5;

    while (total < want)
      {
        struct timespec ts;
        struct timespec now;
        ssize_t n;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec))
          {
            printf("总时限到（已收 %d 个缓冲区 %d 字节）—— 采集跟不上速率\n",
                   got, total);
            break;
          }

        /* ★ 等待也要有上界。
         *
         *   我原来只用字节数设了上界，但那只有在缓冲区**会回来**时才成立。
         *   驱动若一个都不还（采集没真正跑起来就是这样），mq_receive 会
         *   一直阻塞 —— 而这条命令是前台任务，控制台跟着一起没了。
         *   这正是我做这个命令要避开的东西，第一版却没做到。
         *
         *   两秒收不到一个缓冲区就认定采集没起来，如实报告并退出。
         */

        /* mq_timedreceive 的绝对超时按 POSIX 走 CLOCK_REALTIME，
         * 这里不能换成单调钟 —— 两者用途不同，别一起改。
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

            /* ★ 前 5 轮留痕。循环里只有 fwrite 和 ENQUEUEBUFFER 可能
             *   无限阻塞，而 ENQUEUEBUFFER 会进驱动去抢两把**没有超时**
             *   的互斥锁（pendlock 与 SAI 的 priv->lock）。哪一步停住，
             *   这两行直接分开。
             */

            /* ★ 别再给打印开窗口了。
             *
             *   第一版开 `got <= 5`，1-5 全正常、6 之后死掉 —— 证据正好
             *   落在窗口外。第二版改成"每 4 个"，于是卡点落在 16 和 20
             *   之间，还是看不见。**窗口位置本身就是一个假设**，而我连着
             *   两次押错。一共才 24 个缓冲区，全打出来的代价是零。
             *
             *   同时带上耗时：每个缓冲区 8192 字节，按 48kHz 立体声 16 位
             *   应是 43ms，按实测的 12kHz 是 171ms。**实际耗时直接判定
             *   数据是真的还是 DMA 在按总线速度搬垃圾** —— 如果是个位数
             *   毫秒，那就不是音频数据。
             */

            {
              struct timespec tn;
              static uint32_t t_prev;
              uint32_t t_now;

              clock_gettime(CLOCK_MONOTONIC, &tn);
              t_now = (uint32_t)(tn.tv_sec * 1000 + tn.tv_nsec / 1000000);

              printf("[%d] %dB total=%d 用时=%ums peak=%d\n",
                     got, (int)apb->nbytes, total,
                     (unsigned)(got > 1 ? t_now - t_prev : 0), p);
              fflush(stdout);
              t_prev = t_now;
            }

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

  /* ★ 每个收尾步骤都留一行痕迹。
   *
   *   上一版跑完 mic 后控制台就没了，而且**一条日志都没有** —— 驱动那边
   *   已经停了，所以卡的是本命令自己。但"卡在收尾的哪一步"完全看不出来：
   *   STOP、UNREGISTERMQ、FREEBUFFER、RELEASE、close 每一个都可能阻塞。
   *   与其逐个猜、每猜一次烧一块板，不如一次把五步都标出来。
   */

  printf("[收尾] STOP...\n"); fflush(stdout);
  ioctl(fd, AUDIOIOC_STOP, 0);
  printf("[收尾] STOP 返回\n"); fflush(stdout);

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
  printf("[收尾] UNREGISTERMQ...\n"); fflush(stdout);
  ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)mq);
  printf("[收尾] FREEBUFFER...\n"); fflush(stdout);

  for (i = 0; i < MIC_NBUFFERS; i++)
    {
      if (bufs[i])
        {
          buf_desc.u.buffer = bufs[i];
          ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
        }
    }

release:
  printf("[收尾] RELEASE...\n"); fflush(stdout);
  ioctl(fd, AUDIOIOC_RELEASE, 0);
  printf("[收尾] RELEASE 返回\n"); fflush(stdout);

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
