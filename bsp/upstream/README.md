# 上游侧改动

这些改动**不在** nuttx 核心仓，也不在本仓的板级/芯片层，而是散落在
openvela 工程的其它仓里。按《新平台适配指南》「不得修改核心代码」的精神，
它们不该跟板级适配混在一起，各自独立提 PR。

归档在这里是为了让别人能复现我们的构建。

| 补丁 | 仓 | 内容 |
|---|---|---|
| `ai_agent.patch` | `packages/ai_agent` | ① `feishu_http.c` 用了 `cJSON_int` —— 这个类型在上游 cJSON 里不存在，全仓只出现这一处、无任何定义，是笔误；顺带 `valueint` 是 `int` 却配 `%lld`，一并改正。② Makefile 加 `-Wno-format-truncation`：`agent_loop.c` 拼路径的 `snprintf` 会被判可能截断，该检查随优化级别变化，参考平台（goldfish，`-O3`）不触发。 |
| `frameworks-uv.patch` | `frameworks/system/utils/uv` | `uv_aes.c` / `uv_hkdf.c` / `uv_ecdh.c` 照 mbedtls 2.x 写的，直接访问 `mbedtls_cipher_context_t` 的 `cipher_info` / `add_padding` / `get_padding`。树里是 mbedtls 3.4.0，这些成员已被 `MBEDTLS_PRIVATE()` 包起来。加 `-DMBEDTLS_ALLOW_PRIVATE_ACCESS`。 |
| `0002-nuttx-include-tls-task-header.patch` | `apps/system/libuv` | libuv 的 nuttx 移植只 include 了 `nuttx/tls.h`，但用的是 `task_tls_*`，那些声明在 `nuttx/tls_task.h`。**注意这是给下载源码打的补丁**：libuv 源码是构建时从 GitHub 拉的，直接改工作副本会在干净构建时丢失，所以必须以 `000*.patch` 的形式放进 `apps/system/libuv/`（Makefile 会 `sort $(wildcard 000*.patch)` 全部应用）。 |
| `apps.patch` | `apps` | `drivertest_spidev_master.c` 一处格式符 `%d` -> `%zu`。 |
| `tftpc-null-blockno.patch` | `apps` | **空指针解引用**：`netutils/tftpc/tftpc_put.c` 等 WRQ 首个 ACK 时传 `blockno = NULL`（那个块号必然为 0，调用方不关心），但 `tftp_rcvack()` 无条件写 `*blockno = rblockno`。**任何一次握手成功的 TFTP put 都会 panic**。单独成补丁而不并进 `apps.patch`，因为这是个可独立提 PR 的真实缺陷，跟那边的格式符清理不是一回事。 |

## 另外两个必须记住的配置坑

**cJSON 版本**：`CONFIG_NETUTILS_CJSON_VERSION` 默认 1.7.12，缺
`cJSON_GetNumberValue` / `cJSON_SetValuestring`，ai_agent 编不过。已改 1.7.18。

**`CONFIG_ARCH_CHIP_CUSTOM` 必须原样写进 defconfig**，光靠
`ARCH_CHIP_ARM64_CUSTOM` 去 `select` 它不行 —— `tools/Config.mk` 是 make
在 kconfig 解析 select 之前读的。详见根目录 README。
