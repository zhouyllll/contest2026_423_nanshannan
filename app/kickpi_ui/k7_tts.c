/****************************************************************************
 * app/kickpi_ui/k7_tts.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音播报：文字 → mimo-v2.5-tts → 24kHz PCM → 插值到 48kHz → ES8388。
 *
 * ★ 接口（Token Plan，OpenAI 兼容的 chat/completions）
 *
 *   请求：model = mimo-v2.5-tts，messages 里 user 放朗读风格、assistant
 *         放要读的文字；audio = {format: "pcm16", voice: "冰糖"}；不流式。
 *   应答：choices[0].message.audio.data 是 base64 的 24kHz 单声道 PCM16LE。
 *
 *   HTTPS 直接用 ai_agent 的 vela_https_post_json（同一个地址空间，连接池
 *   有锁）。它返回 HTTP 状态码，应答体写进调用方给的缓冲区。
 *
 * ★ 缓冲区给多大
 *
 *   120 个汉字大约读 30 秒：24k x 2 字节 x 30s = 1.4MB PCM，base64 后
 *   1.9MB，再加 JSON 外壳。应答缓冲给 4MB，超出的话 vela_tls 会静默截断，
 *   截断的 base64 解出来是半截音频 —— 所以送去合成的文字也限了长度。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <cJSON.h>

#include "k7_audio.h"
#include "k7_net.h"
#include "k7_tts.h"

#ifdef __has_include
#  if __has_include("k7_agent_key.h")
#    include "k7_agent_key.h"
#  endif
#endif

#define TTS_PATH      "/v1/chat/completions"
#define TTS_MODEL     "mimo-v2.5-tts"
#define TTS_VOICE     "冰糖"
#define TTS_RATE      24000
#define TTS_RESP_CAP  (4 * 1024 * 1024)
#define TTS_TEXT_MAX  600               /* 字节，约 200 个汉字 */

static int b64_val(int c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* 就地解码：输出不会比输入长。返回字节数。 */

static size_t b64_decode_inplace(char *s)
{
  const char *r = s;
  uint8_t *w = (uint8_t *)s;
  uint32_t acc = 0;
  int bits = 0;

  for (; *r != '\0'; r++)
    {
      int v = b64_val((unsigned char)*r);

      if (v < 0)
        {
          continue;                 /* '='、换行等一律跳过 */
        }

      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
        {
          bits -= 8;
          *w++ = (uint8_t)(acc >> bits);
        }
    }

  return (size_t)(w - (uint8_t *)s);
}

/* UTF-8 安全截断到 max 字节 */

static void utf8_trunc(char *s, size_t max)
{
  size_t len = strlen(s);

  if (len <= max)
    {
      return;
    }

  len = max;
  while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80)
    {
      len--;
    }

  s[len] = '\0';
}

static char *tts_build_body(const char *text)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *msgs;
  cJSON *m;
  cJSON *audio;
  char *body;

  if (root == NULL)
    {
      return NULL;
    }

  cJSON_AddStringToObject(root, "model", TTS_MODEL);
  msgs = cJSON_AddArrayToObject(root, "messages");

  m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  cJSON_AddStringToObject(m, "content", "用自然、亲切、清晰的普通话朗读，语速适中。");
  cJSON_AddItemToArray(msgs, m);

  m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "assistant");
  cJSON_AddStringToObject(m, "content", text);
  cJSON_AddItemToArray(msgs, m);

  audio = cJSON_AddObjectToObject(root, "audio");
  cJSON_AddStringToObject(audio, "format", "pcm16");
  cJSON_AddStringToObject(audio, "voice", TTS_VOICE);
  cJSON_AddBoolToObject(root, "stream", false);

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return body;
}

int k7_tts_speak(const char *text)
{
#ifndef K7_AGENT_LLM_KEY
  (void)text;
  syslog(LOG_WARNING, "播报: 没有 k7_agent_key.h，不能合成\n");
  return -ENOTSUP;
#else
  static const char auth[] = "Bearer " K7_AGENT_LLM_KEY;
  const vela_header_t hdrs[] =
  {
    { "Authorization", auth },
    { NULL, NULL }
  };

  char *copy = NULL;
  char *body = NULL;
  char *resp = NULL;
  int16_t *pcm48 = NULL;
  cJSON *root = NULL;
  cJSON *data;
  const int16_t *pcm24;
  size_t n24;
  size_t bytes;
  size_t i;
  int status;
  int ret;

  copy = strdup(text);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  utf8_trunc(copy, TTS_TEXT_MAX);
  body = tts_build_body(copy);
  resp = malloc(TTS_RESP_CAP);
  if (body == NULL || resp == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  status = vela_https_post_json(K7_AGENT_LLM_HOST, "443", TTS_PATH, hdrs,
                                body, resp, TTS_RESP_CAP);
  if (status != 200)
    {
      resp[200 < TTS_RESP_CAP ? 200 : TTS_RESP_CAP - 1] = '\0';
      syslog(LOG_ERR, "播报: HTTP %d %s\n", status, status > 0 ? resp : "");
      ret = status < 0 ? status : -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  free(resp);                           /* 解析完就放掉 4MB */
  resp = NULL;

  data = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(
           cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
           "message"), "audio"), "data");
  if (!cJSON_IsString(data))
    {
      syslog(LOG_ERR, "播报: 应答里没有 choices[0].message.audio.data\n");
      ret = -EPROTO;
      goto out;
    }

  bytes = b64_decode_inplace(data->valuestring);
  pcm24 = (const int16_t *)data->valuestring;
  n24 = bytes / 2;
  if (n24 < 2)
    {
      ret = -EPROTO;
      goto out;
    }

  /* 24k → 48k：每个样本后面插一个与下一个样本的中点 */

  pcm48 = malloc(n24 * 2 * sizeof(int16_t));
  if (pcm48 == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  for (i = 0; i < n24; i++)
    {
      int16_t a = pcm24[i];
      int16_t b = i + 1 < n24 ? pcm24[i + 1] : a;

      pcm48[2 * i]     = a;
      pcm48[2 * i + 1] = (int16_t)(((int32_t)a + b) / 2);
    }

  syslog(LOG_INFO, "播报: 合成 %zu 字节文字 → %zu ms 音频\n",
         strlen(copy), n24 * 1000 / TTS_RATE);

  cJSON_Delete(root);                   /* 播放前先放掉 JSON 占的内存 */
  root = NULL;
  ret = k7a_play_mono(pcm48, n24 * 2);

out:
  cJSON_Delete(root);
  free(pcm48);
  free(resp);
  cJSON_free(body);
  free(copy);
  return ret;
#endif
}
