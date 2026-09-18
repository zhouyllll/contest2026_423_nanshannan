# 交接说明（给下一个 Claude Code 会话）

更新：2026-09-18。上一段会话太长，这里是**接手时必须知道的全部事实**。
细节以 git log 和各文件注释为准；本文只讲现状、规矩和坑。

---

## 1. 项目一句话

openvela（NuttX）移植到 **KICKPI-K7（RK3576）**，大赛 BSP 作品，截止 2026-09-20。
仓库：`~/openvela-amlogic/src/contest2026_423_nanshannan`（公开 GitHub，分支 `rk3576-bsp`）。
主要形态是 **AMP 双系统**：openvela 跑 A53 簇（4 核 SMP），Linux 跑 A72 簇（只做计算，无控制台）。
同一份 openvela 二进制可打成**双系统**（`amp/fit/amp.its`）或**单系统**（`amp-solo.its`，不带 Linux）。

## 2. 铁律（违反过，代价很大）

1. **API Key 绝不进 git，也不要在对话/命令行里明文出现。** 仓库公开，`logs/` 会导出会话。
   Key 在本地 `app/kickpi_ui/k7_agent_key.h`（已 gitignore，模板 `.example`）。用的时候从文件读：
   `K=$(sed -n 's/.*K7_AGENT_LLM_KEY *"\(.*\)".*/\1/p' app/kickpi_ui/k7_agent_key.h)`。
   `backup/images/` 里 09-17 之后的镜像都内嵌 Key，只能留在本地。
2. **动板子前先确认用户没开 minicom。** 两个程序读同一个 `/dev/ttyUSB0` 会各丢一半字节，
   看起来和死机一模一样（pyserial 报 "device reports readiness to read but returned no data"）。
   上段会话因此误判"挂死"好几次。判断死机要用 telnet 2323 / ping 交叉验证。
3. **不许写 LBA 49152–65535**（Linux 内核）。旧 `flash.sh` 写 51200 覆盖过内核，
   导致"双系统全挂、单系统正常"查了一整天。现在所有写盘经 `amp/layout.sh` 的 `amp_check_write`。
4. `cmocka_driver_block` 不许对 eMMC（`/dev/mmcsd0`）跑；xTS 1.3.7 的 spidev/i2cdev master 用例会挂板，不要跑。
5. 不许 force-push；不许未经确认合并 PR #1；`logs/` 只收白名单会话（`scripts/collect-logs.sh`）。
6. 未验证的硬件启动流程（如蓝牙固件加载）**不放进启动路径**，用手动命令触发，否则卡住就进不了 nsh。

## 3. eMMC 布局（`amp/layout.sh` 是唯一来源）

| LBA | 内容 | 说明 |
|---:|---|---|
| 14336 | Linux AMP DTB | `rk3576-kickpi-k7-amp.dtb`，上限 544 扇区 |
| 16384 | U-Boot（自编，带 `bootamp`） | `bootcmd=bootamp`，`bootdelay=1` |
| **24576** | **AMP FIT（openvela）** | trust 分区，上限 8192 扇区（09-17 从 8192 挪过来） |
| 49152 | Linux `Image-amp`（7.2MB） | `~/rk3576-amp/out/Image-amp` sha dfb0842a |
| ≥65536 | rkdeveloptool **写不进去**（静默丢弃） | |

U-Boot 改动在 `amp/uboot/0004..0009`，U-Boot 源码 `~/rk3576-amp/u-boot`（不是 git 仓库）。
Linux 内核树 `~/rk3576-amp/kernel-6.1`；编 DTB：
`make ARCH=arm64 CROSS_COMPILE=<openvela aarch64-none-elf-> rockchip/rk3576-kickpi-k7-amp.dtb </dev/null`，
**编完 `cp .config.old .config`**（syncconfig 会改 .config）。

## 4. 编译 / 刷写 / 串口

```bash
cd ~/openvela-amlogic/src/contest2026_423_nanshannan
source scripts/env.sh
cd ../nuttx && make distclean; ./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp-dual && make -j8
cd ../contest2026_423_nanshannan && bash scripts/flash.sh          # 双系统；--solo 单系统；--fit X.itb；--stay
```

- `flash.sh`：打 FIT → 进下载模式（先发 `loader`，落到 Maskrom 就 `db`；再退到 `reboot`+抓 U-Boot）
  → 备份/写/回读/失败回滚 → `rd` 复位。进不去时通过 telnet 2323 发 `reboot`，或请用户按 RESET（**不要按 RECOVERY 键**）。
- 改了 ai_agent 头文件（如 `agent_config.h`）后要 `touch` 用到它的 .c，Makefile 不追踪头文件依赖。
- 串口：`/dev/ttyUSB0`，**1500000**（U-Boot 和 NuttX 都是）。usbipd 在 WSL 里：`/mnt/c/Program Files/usbipd-win/usbipd.exe`。
- 网络：板子 **192.168.1.50**，网关/DNS 192.168.1.1，**网线插 GMAC1 口**（`k7diag eth` 可查两个口链路）。
  电脑是 192.168.1.100。telnet NSH：`192.168.1.50:2323`。
- 串口调试入口：`echo 1 > /tmp/k7-open-camera`（开相机页）、`echo 1 > /tmp/k7-ask`（agent 看桌面，回复写 syslog）。

## 5. 现在能用的（已上板验证）

最后一版干净验证镜像：`~/rk3576-amp/backup/images/20260918_dual_agent-e2e_verified.itb`（提交 27f497a）。

- 双系统启动、rpmsg 握手、屏幕/触摸（LVGL PARTIAL）、音频
- 摄像头：`/dev/video0`（IMX415），`v4l2cap`；界面相机页实时画面约 30fps
- 网络：ping、DNS、telnet NSH
- **桌面助手端到端**：开机自启 `ai_agent` → 拍照 → mimo-v2.5 看图 → 作答，界面收到回答（实测 193s）
  - Token Plan：地址 `https://token-plan-cn.xiaomimimo.com/v1`，模型 **mimo-v2.5**（不支持 mimo-v2-flash/omni）

## 6. 上段会话修过的关键缺陷（别再踩）

| 现象 | 根因 | 位置 |
|---|---|---|
| 双系统全挂、单系统好 | flash.sh 覆盖 LBA 49152 内核 | layout.sh / flash.sh |
| 双系统摄像头/音频 I2C 无应答 | Linux `I2C_RK3X=y` 接管了 i2c2/3/4/5/7/8 | amp dts 里 disable |
| ping 几下整机挂 | 收包回复不检查发送环空位 → freeframe 死循环 | `rk3576_eth_reply` |
| HTTPS 下 lpwork panic | `txdone` 里 DEBUGASSERT(txtail) | rk3576_eth.c |
| 相机页 5fps | `cif_wait_frame` 用 up_mdelay 空转 + RR 200ms | 改 nxsig_usleep |
| 点"打开相机"没反应 | LVGL 内存池 256KB，921KB 缓冲分配失败 | `ui_draw_buf_create` 走系统堆 |
| agent 拍照不返回 | V4L2 每帧都排 JPEG 编码，LPWORK 永远忙 | rk3576_video.c 按需编码 |
| 拍两次照后堆损坏 | **CIF DMA 每帧越界写约 2920 字节** | RAW 缓冲尾部哨兵 + 余量 |
| agent 回答被丢 | 60s 看门狗 < mimo-v2.5 推理 86~99s | 180s（`bsp/upstream/ai_agent.patch`） |
| 界面卡在"让我查一下..." | 过渡消息用掉 SDK 一次性回调 | local_client 不发过渡消息 |

## 7. 未解决（按用户要的顺序）

### 7.1 蓝牙（用户正在做，先接着做）
- K7 实焊 **SKW6621S（SDIO 0x2a320000）**，不是 dtsi 里的 AP6256/BCM UART。设计/需求/任务在
  `chip/rk3576/skw6621s/{requirements,design,tasks}.md`（**用户的未跟踪文件，别动别提交**）。
- 现状：`bt probe` → CMD5 响应无效（`RINTSTS=0x2`，RESP0=0）。用户 09-18 已提交多轮 A/B 试验
  （commit 22239d0..b946208，记录在 `notes/bt-*-ab-2026-09-18/`），先读这些再动手。
- 我本地**未提交**：`amp-dual/defconfig` 打开 `RK3576_SKW6621S/RK3576_DWMMC/MMCSD_SDIO`、去掉 BCM4343X。
  与用户的配置对齐前别提交。
- 原厂依据：`~/rk3576-amp/kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-wifi.dtsi`
  （pwrseq：GPIO1_C6 active-low reset，post-power-on 200ms；BT_REG_ON GPIO1_C7）。
  32K 时钟来自 HYM8563 CLKOUT（RTC 须先于 SDIO 初始化）。Linux 内核无 MMC 驱动，不碰 SDIO。

### 7.2 CPU1（双系统下不接任务）
- 事实：CPU1 启动阶段走完（进 IDLE），但之后绑到 CPU1 的线程永远不跑；单系统四核都正常；
  没有收到 Linux 发来的 SGI（计数为 0）。
- 现为**绕行**：`board_late_initialize` → `kickpi_avoid_cpu1()` 让所有任务避开 CPU1（affinity 0x0d）。
- 诊断补丁（各核启动阶段 / 中断计数 / 外簇 SGI 计数 / `k7diag cpus [test]`）在
  `/tmp/claude-1000/.../scratchpad/diag/` —— 已从源码撤出。下一步：看 CPU1 的中断计数是否在涨
  （不涨 = 关中断卡住；疯涨 = 陷在某个中断里）。**`k7diag cpus test` 会把 nsh 卡住**，用 telnet 跑。
- 怀疑方向：共享 GIC 模式（`CONFIG_ARM64_GICV2_SHARED_DIST`）下的 banked 寄存器 / SGI 配置；
  U-Boot `smc_cpu_on` 的 `sip_smc_amp_cfg(AMP_PE_STATE…)` 是否把配置记到了 A53 core1。

### 7.3 本地语音唤醒（未开始）
- `docs/k7-ai-agent-design.md` 有设计：本地匹配固定唤醒词"你好，openvela"→ ASR → agent → TTS。
- Token Plan 里有 `mimo-v2.5-asr` / `mimo-v2.5-tts`；麦克风先用 `mic` 命令验证信号（MIC 走 LINE2）。

### 7.4 其他已知问题
- CIF DMA 为什么多写一行未查（传感器实际行数 > 配置？）。现在靠余量挡着。
- agent 一次回答约 3 分钟（模型端推理）。
- 带诊断代码的镜像从 Maskrom 起来时，用户在 **115200** 看到过 nsh —— 原因未查。
- xTS：1.3.15 看门狗 api 子项（GLB_RST_ST 被上游清零）未过；stash@{0} 的内容已提交（e84061b），stash 可删。
- NuttX 仓库（`../nuttx`）有未提交修改：`arm64_gicv2.c`（SHARED_DIST 支持，**必需**）、ft5x06、es8388、bt_uart 等，归属待核对，别随手 checkout。

## 8. 相关记忆文件

`~/.claude/projects/-home-dministrator-linux/memory/`：
`rk3576-amp-emmc-layout.md`、`serial-port-shared-check.md`、`rk3576-official-sdk.md`（原厂 SDK 只读 `git show` 读法）。
