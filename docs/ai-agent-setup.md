# ai_agent 上板配置

## 前提

- 网口已通（`ifconfig eth0 <ip> netmask 255.255.255.0 dr <网关>` + `ifup eth0`）
- DNS 可用（`CONFIG_NETDB_DNSCLIENT=y`，已在 defconfig）
- `/data` 已挂载（板级初始化里挂的 tmpfs）

## 步骤

```
nsh> ifconfig eth0 192.168.1.100 netmask 255.255.255.0 dr 192.168.1.1
nsh> ifup eth0
nsh> ai_agent
vela> set_llm mimo <你的-API-Key>
vela> 你好
```

后端预设见 `packages/ai_agent/README.md`：`kimi` / `qwen` / `deepseek` /
`glm` / `mimo` / `openai` / `claude` / `openrouter`。大赛发的是小米
MiMo Token Plan，用 `mimo` 预设。

## ★ Key 不要写进仓库

本仓默认 public。`set_llm` 会把 Key 存到 `/data/ai_agent/` 下，**在板子上，
不在仓库里** —— 这是对的，不要为了图省事把它硬编码进 defconfig 或源码。
`.gitignore` 里已加了几条防线，但那只挡得住手滑。

## 已知限制

`/data` 现在是 tmpfs，**掉电即失**，每次重启都要重新 `set_llm`。
要持久化得改挂 SD 卡（`/dev/mmcsd1`）—— 接 Skills 时一并做，
现在不做是为了不把「agent 能不能跑」和「SD 卡在不在」绑在一起。
