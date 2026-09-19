/****************************************************************************
 * app/kickpi_ui/k7_vision.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * 桌面问答：拍一张照片，连同问题一次发给 mimo-v2.5，拿回答。
 *
 * ★ 为什么不走 ai_agent 的循环
 *
 *   走 agent 是三次模型调用：LLM 决定调相机 → 相机工具里视觉模型描述 →
 *   LLM 组织回答。mimo-v2.5 默认开"深度思考"，板上实测一次 196s。关掉
 *   思考后降到 23s，但又碰到两件事：
 *     - agent 自带的 camera_capture 拿到 0 字节（v4l2cap 同一时间能拍到
 *       262KB），失败之后 agent 凭会话记忆一直回"摄像头不可用"；
 *     - 界面每次发同样的问题，agent 在 0ms 内直接复用上一次的回答。
 *   桌面问答本身不需要工具编排：拍照 + 一次视觉请求就够了，快、可控。
 *
 * ★ 请求
 *
 *   POST /v1/chat/completions，model mimo-v2.5，"thinking":{"type":"disabled"}，
 *   user content = [image_url(data:image/jpeg;base64,...), text]。
 *   回答在 choices[0].message.content。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cJSON.h>

#include "k7_net.h"
#include "k7_vision.h"

#ifdef __has_include
#  if __has_include("k7_agent_key.h")
#    include "k7_agent_key.h"
#  endif
#endif

#define VISION_JPEG     "/tmp/k7-vision.jpg"
#define VISION_PATH     "/v1/chat/completions"
#define VISION_RESP_CAP (64 * 1024)
#define VISION_JPEG_MAX (1024 * 1024)

/* 拍照：跑 v4l2cap，最多等 10 秒。
 *
 * ★ 判断"拍完了"看文件，不看 waitpid。
 *
 *   在工作线程（pthread）里 waitpid(WNOHANG) 等 v4l2cap：串口上 v4l2cap
 *   早就打印了"成功"，waitpid 却 10 秒都不返回它的 pid —— 界面主线程里
 *   同样的写法是好的。与其追 NuttX 在 pthread 里等子进程的语义，不如看
 *   结果：照片存在、大于 1KB、连续两次检查大小不变，就算拍完。waitpid
 *   仍然顺手调，能收就收。
 */

static int vision_snapshot(void)
{
  char *argv[] =
  {
    "v4l2cap", "1280", "720", VISION_JPEG, NULL
  };

  struct stat st;
  off_t last = -1;
  pid_t pid;
  int status = 0;
  int waited;
  int ret;

  unlink(VISION_JPEG);
  ret = posix_spawn(&pid, "v4l2cap", NULL, NULL, argv, NULL);
  if (ret != 0)
    {
      return -ret;
    }

  for (waited = 0; waited < 10000; waited += 100)
    {
      usleep(100 * 1000);
      waitpid(pid, &status, WNOHANG);

      if (stat(VISION_JPEG, &st) == 0 && st.st_size >= 1024)
        {
          if (st.st_size == last)
            {
              return (int)st.st_size;
            }

          last = st.st_size;
        }
    }

  syslog(LOG_ERR, "视觉: 10 秒内没拿到照片（%s）\n", VISION_JPEG);
  return -ETIMEDOUT;
}

static const char g_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *in, size_t len)
{
  size_t olen = (len + 2) / 3 * 4;
  char *out = malloc(olen + 1);
  char *w = out;
  size_t i;

  if (out == NULL)
    {
      return NULL;
    }

  for (i = 0; i + 2 < len; i += 3)
    {
      uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];

      *w++ = g_b64[(v >> 18) & 63];
      *w++ = g_b64[(v >> 12) & 63];
      *w++ = g_b64[(v >> 6) & 63];
      *w++ = g_b64[v & 63];
    }

  if (i < len)
    {
      uint32_t v = in[i] << 16;

      if (i + 1 < len)
        {
          v |= in[i + 1] << 8;
        }

      *w++ = g_b64[(v >> 18) & 63];
      *w++ = g_b64[(v >> 12) & 63];
      *w++ = i + 1 < len ? g_b64[(v >> 6) & 63] : '=';
      *w++ = '=';
    }

  *w = '\0';
  return out;
}

static char *vision_load_datauri(void)
{
  static const char prefix[] = "data:image/jpeg;base64,";
  unsigned char *jpeg = NULL;
  char *b64 = NULL;
  char *uri = NULL;
  struct stat st;
  FILE *fp;
  size_t n;

  if (stat(VISION_JPEG, &st) < 0 || st.st_size > VISION_JPEG_MAX)
    {
      return NULL;
    }

  jpeg = malloc(st.st_size);
  fp = fopen(VISION_JPEG, "rb");
  if (jpeg == NULL || fp == NULL)
    {
      goto out;
    }

  n = fread(jpeg, 1, st.st_size, fp);
  b64 = b64_encode(jpeg, n);
  if (b64 == NULL)
    {
      goto out;
    }

  uri = malloc(sizeof(prefix) + strlen(b64));
  if (uri != NULL)
    {
      strcpy(uri, prefix);
      strcat(uri, b64);
    }

out:
  if (fp != NULL)
    {
      fclose(fp);
    }

  free(jpeg);
  free(b64);
  return uri;
}

int k7_vision_ask(const char *question, char *answer, size_t cap)
{
#ifndef K7_AGENT_LLM_KEY
  (void)question;
  snprintf(answer, cap, "没有配置模型 Key（k7_agent_key.h）。");
  return -ENOTSUP;
#else
  static const char auth[] = "Bearer " K7_AGENT_LLM_KEY;
  const vela_header_t hdrs[] =
  {
    { "Authorization", auth },
    { NULL, NULL }
  };

  cJSON *root = NULL;
  cJSON *content;
  cJSON *part;
  cJSON *msgs;
  cJSON *m;
  char *uri = NULL;
  char *body = NULL;
  char *resp = NULL;
  struct timespec t0;
  struct timespec t1;
  int status;
  int ret;

  clock_gettime(CLOCK_MONOTONIC, &t0);

  ret = vision_snapshot();
  if (ret < 0)
    {
      snprintf(answer, cap, "拍照失败（%d），请稍后再试。", ret);
      return ret;
    }

  uri = vision_load_datauri();
  if (uri == NULL)
    {
      snprintf(answer, cap, "照片读取失败。");
      return -ENOMEM;
    }

  root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", K7_AGENT_VISION_MODEL);
  cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "thinking"),
                          "type", "disabled");
  cJSON_AddNumberToObject(root, "max_completion_tokens", 512);
  msgs = cJSON_AddArrayToObject(root, "messages");
  m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  content = cJSON_AddArrayToObject(m, "content");

  part = cJSON_CreateObject();
  cJSON_AddStringToObject(part, "type", "image_url");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(part, "image_url"),
                          "url", uri);
  cJSON_AddItemToArray(content, part);

  part = cJSON_CreateObject();
  cJSON_AddStringToObject(part, "type", "text");
  cJSON_AddStringToObject(part, "text", question);
  cJSON_AddItemToArray(content, part);

  cJSON_AddItemToArray(msgs, m);
  free(uri);
  uri = NULL;

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  root = NULL;

  resp = malloc(VISION_RESP_CAP);
  if (body == NULL || resp == NULL)
    {
      snprintf(answer, cap, "内存不够。");
      ret = -ENOMEM;
      goto out;
    }

  status = vela_https_post_json(K7_AGENT_LLM_HOST, "443", VISION_PATH, hdrs,
                                body, resp, VISION_RESP_CAP);
  if (status != 200)
    {
      resp[status > 0 ? 200 : 0] = '\0';
      syslog(LOG_ERR, "视觉: HTTP %d %s\n", status, resp);
      snprintf(answer, cap, "请求模型失败（HTTP %d）。", status);
      ret = status < 0 ? status : -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  m = cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
        "message"), "content");
  if (!cJSON_IsString(m))
    {
      snprintf(answer, cap, "模型没有给出回答。");
      ret = -EPROTO;
      goto out;
    }

  snprintf(answer, cap, "%s", m->valuestring);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  syslog(LOG_INFO, "视觉: 照片 %d 字节，用时 %ld ms\n", ret,
         (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                (t1.tv_nsec - t0.tv_nsec) / 1000000));
  ret = 0;

out:
  cJSON_Delete(root);
  cJSON_free(body);
  free(resp);
  free(uri);
  return ret;
#endif
}
