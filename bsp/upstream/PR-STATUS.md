# 上游 PR 状态

大赛规则：公共仓（nuttx / nuttx-apps / packages_ai_agent 等）的改动
**不得直接进队伍仓**，须 fork 对应仓、PR 到 `dev-ai-contest-2026` 分支，
由组委会 review 合入（见 `docs/refs/contest_2026/code_submission_guide.md`）。

## 已准备好的分支

三个分支都**基于上游目标分支创建**，diff 里只有本次改动，不含 RK3576 BSP
的历史 —— 这是官方 `submit-pr` 技能 Step 6 特别强调的一点：从当前开发分支
切出去会把整条 BSP 历史带进 PR。

| 仓 | 本地分支 | 内容 |
|---|---|---|
| `open-vela/nuttx` | `pr/hym8563-rtc` | ① 新增 `drivers/timers/hym8563.c`（I2C RTC，316 行）② `ft5x06.c` 的 `%d`→`%zu` |
| `open-vela/nuttx-apps` | `pr/tftpc-null-blockno` | ① tftpc put 路径空指针解引用修复 ② `CONFIG_NETUTILS_TFTP_PORT` 补成 Kconfig 选项 |
| `open-vela/packages_ai_agent` | `pr/monotonic-latency` | LLM 耗时改用 `CLOCK_MONOTONIC`（墙钟被 TLS 改钟后误判超时） |

每个分支内按"一个提交一件事"拆分，提交信息说明**现象、根因、以及为什么
这个缺陷此前没被发现**。

## 推送与开 PR

前置：CLA 已签（`https://openvela.com/#/community/cla`）。若 PR 上
`cla/signature` 检查未过，在 PR 下评论 `/check-cla` 触发复检即可，
不需要重开 PR。

```bash
# 以 nuttx-apps 为例，其余两仓同理
cd <工作区>/apps
gh repo fork open-vela/nuttx-apps --remote-name myfork --clone=false   # 或网页 fork
git push myfork pr/tftpc-null-blockno
gh pr create --repo open-vela/nuttx-apps \
  --base dev-ai-contest-2026 --head <你的用户名>:pr/tftpc-null-blockno \
  --title "netutils/tftpc: fix NULL dereference on the WRQ acknowledgement path"
```

没有 `gh` 时：`git push` 到自己 fork 后，在
`https://github.com/open-vela/<仓>/compare/dev-ai-contest-2026...<你的用户名>:pr/<分支>`
页面开 PR。

## 仍留在 bsp/ 的补丁（暂不提 PR）

| 补丁 | 原因 |
|---|---|
| `apps.patch` | 只是 `drivertest_spidev_master.c` 的格式符，价值低，可并入他人清理 |
| `frameworks-uv.patch` | mbedtls 3.x 的 `MBEDTLS_ALLOW_PRIVATE_ACCESS` 绕过，是权宜之计而非正解，直接提上去会把技术债固化 |
| `0002-nuttx-include-tls-task-header.patch` | 打给构建时下载的 libuv 源码，不是 openvela 仓内文件 |
| `ai_agent.patch` | `cJSON_int` 笔误值得提；`-Wno-format-truncation` 是绕过，两者应拆开，未拆完 |
