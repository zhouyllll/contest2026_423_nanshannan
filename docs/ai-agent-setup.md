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

## ★ 上板实测踩到的四个坑

**① 串口行编辑器会折断长命令。** `set_llm` 带 Key 时整条命令 65~90 字符，
超过约 60 字符就会被截断成两条，表现为「命令执行了但 Key 是错的」。
**必须逐字符发**（每字符间隔 20ms），或用支持长行的终端。
第一次 401 就是这么来的。

**② `set_llm model <名>` 会被误解析。** 文档写它「仅切换模型」，
实际按 `set_llm <host> <model> <key>` 解析 —— host 变成字面量 `model`，
后端地址直接坏掉（`model:443/v1/chat/completions`）。
**用完整形式**：`set_llm api.xiaomimimo.com <model> <key>`。

**③ `ifconfig eth0 dns <地址>` 会把 IP 清成 0.0.0.0。** ifconfig 把第一个
非选项参数当地址，只给 `dns` 时地址被重置，随后所有出向包都是
ENETUNREACH。**地址和 dns 要在同一条命令里给**：
`ifconfig eth0 192.168.1.100 dns 192.168.1.1`。

**④ 默认 DNS 是 10.0.0.1**（`CONFIG_NETDB_DNSSERVER_IPv4ADDR=0x0a000001`），
对多数网段都不对，必须按上面那条改。改对之后
`nslookup baidu.com` 能解析。

## 已实测通过的部分

```
DNS      nslookup baidu.com -> 111.63.65.103
TLS      Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256
HTTPS    POST api.xiaomimimo.com/v1/chat/completions，服务器返回格式良好的 JSON
```

也就是说**板子这一侧的整条链路是通的**，卡在鉴权：服务器返回
`401 Invalid API Key`。同一个 Key 在主机上用 curl 直接打，返回**完全相同的
401 和响应体** —— 与板子无关，是 Key 本身的问题。

## 已知限制

`/data` 现在是 tmpfs，**掉电即失**，每次重启都要重新 `set_llm`。
要持久化得改挂 SD 卡（`/dev/mmcsd1`）—— 接 Skills 时一并做，
现在不做是为了不把「agent 能不能跑」和「SD 卡在不在」绑在一起。
