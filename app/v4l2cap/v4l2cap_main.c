/****************************************************************************
 * app/v4l2cap/v4l2cap_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 从 /dev/video0 抓一帧 JPEG 存成文件。
 *
 * ★ 存在的理由：证明 V4L2 这条路真的通，而不是"设备注册上了"。
 *
 *   ai_agent 的视觉工具（packages/ai_agent/src/tools/tool_camera.c）走的
 *   就是这套调用：S_FMT(JPEG) -> REQBUFS(MMAP) -> QUERYBUF -> mmap ->
 *   QBUF -> STREAMON -> DQBUF。本程序**逐字复刻**这个顺序和参数，所以
 *   它跑通就等于那条路跑通；它失败的地方也就是 agent 会失败的地方。
 *
 *   照着自己的私有接口写一个"能出图"的测试，证明不了 agent 能用 ——
 *   那只会把问题推迟到 agent 真正跑起来的时候，那时候更难查。
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/videoio.h>

#define DEV        "/dev/video0"
#define DEF_WIDTH  1280
#define DEF_HEIGHT 720
#define DEF_PATH   "/tmp/v4l2.jpg"

/* 只要一个缓冲区。
 *
 * ★ MMAP 模式下 REQBUFS 会按 (格式算出的帧大小 x 缓冲区个数) 一次性
 *   从堆里切走一整块。板上堆总共才 ~6MB，多要几个就直接 ENOMEM。
 *   抓单帧不需要流水线，一个就够。
 */

#define NBUFFERS   1

int main(int argc, char *argv[])
{
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  FAR void *addr = MAP_FAILED;
  size_t length = 0;
  FAR const char *path = DEF_PATH;
  int width  = DEF_WIDTH;
  int height = DEF_HEIGHT;
  int status = 1;
  int fd = -1;
  FAR FILE *fp;
  struct pollfd pfd;
  int ret;

  if (argc >= 3)
    {
      width  = atoi(argv[1]);
      height = atoi(argv[2]);
    }

  if (argc >= 4)
    {
      path = argv[3];
    }

  printf("v4l2cap: %dx%d -> %s\n", width, height, path);

  fd = open(DEV, O_RDWR);
  if (fd < 0)
    {
      printf("打不开 %s: %d\n", DEV, errno);
      return 1;
    }

  memset(&cap, 0, sizeof(cap));
  if (ioctl(fd, VIDIOC_QUERYCAP, (unsigned long)&cap) < 0)
    {
      printf("QUERYCAP 失败: %d\n", errno);
      goto out;
    }

  printf("  driver=%s\n", cap.driver);

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;

  if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      printf("S_FMT 失败: %d —— 这个尺寸传感器/转换器不接受\n", errno);
      goto out;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = NBUFFERS;

  if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      printf("REQBUFS 失败: %d\n", errno);
      goto out;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index  = 0;

  if (ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
    {
      printf("QUERYBUF 失败: %d\n", errno);
      goto out;
    }

  length = buf.length;
  addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
              fd, buf.m.offset);
  if (addr == MAP_FAILED)
    {
      printf("mmap %zu 字节失败: %d\n", length, errno);
      goto out;
    }

  printf("  缓冲区 %zu 字节\n", length);

  if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
    {
      printf("QBUF 失败: %d\n", errno);
      goto out;
    }

  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)&type) < 0)
    {
      printf("STREAMON 失败: %d\n", errno);
      goto out;
    }

  /* ★ 先 poll 再 DQBUF。
   *
   *   直接 DQBUF 会一直阻塞到有帧为止，出了问题分不清是"还没到"还是
   *   "永远不会到"。poll 给一个明确的超时，超时就是超时。
   *
   *   5 秒：软件 JPEG 编码一帧 1932x1096 在 lpwork 上要一两秒。
   */

  pfd.fd     = fd;
  pfd.events = POLLIN;
  ret = poll(&pfd, 1, 5000);
  if (ret <= 0)
    {
      printf("等帧超时/出错: ret=%d errno=%d\n", ret, errno);
      ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      goto out;
    }

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
    {
      printf("DQBUF 失败: %d\n", errno);
      ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      goto out;
    }

  printf("  取到 %" PRIu32 " 字节\n", (uint32_t)buf.bytesused);

  if (buf.bytesused == 0)
    {
      printf("  长度为 0 —— 编码失败了，不写文件\n");
      ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      goto out;
    }

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      printf("打不开 %s: %d\n", path, errno);
      ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      goto out;
    }

  if (fwrite(addr, 1, buf.bytesused, fp) != buf.bytesused)
    {
      printf("写盘不完整: %d\n", errno);
      fclose(fp);
      ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
      goto out;
    }

  fclose(fp);
  ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);

  printf("v4l2cap: 成功，%s\n", path);
  status = 0;

out:
  if (addr != MAP_FAILED)
    {
      munmap(addr, length);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return status;
}
