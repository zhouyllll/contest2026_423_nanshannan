/****************************************************************************
 * app/kickpi_ui/k7_net.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 借用 ai_agent 的 HTTPS 客户端（packages/ai_agent/src/infra/vela_tls.h）。
 * NuttX 是平坦地址空间，直接调用即可；连接池内部有锁。这里只声明用到的。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_NET_H
#define __APP_KICKPI_UI_K7_NET_H

#include <stddef.h>

typedef struct
{
  const char *name;
  const char *value;
} vela_header_t;

/* 返回 HTTP 状态码；应答体写进 resp_buf（超出静默截断）。 */

int vela_https_post_json(const char *host, const char *port,
                         const char *path, const vela_header_t *headers,
                         const char *json_body,
                         char *resp_buf, size_t resp_cap);

#endif /* __APP_KICKPI_UI_K7_NET_H */
