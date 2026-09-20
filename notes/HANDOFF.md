# 交接说明（给下一个 Claude Code 会话）

更新：2026-09-20（第三版，补 09-19 夜间与 09-20 的 xTS 收尾）。上一段会话太长，这里是**接手时必须知道的全部事实**。
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
| 16384 | U-Boot（自编，带 `bootamp`） | `bootcmd=bootamp`；倒计时每档 200ms（`amp/uboot/0010`）。烧写用 `scripts/flash-uboot.sh`（备份/回读/回滚） |
| **24576** | **AMP FIT（openvela）** | trust 分区，上限 8192 扇区（09-17 从 8192 挪过来） |
| 40960 | 开机 logo 的 resource 镜像（dtbo 分区） | `scripts/gen-boot-logo.py` 生成，`scripts/flash-logo.sh` 烧写；U-Boot 侧 `amp/uboot/0011` 让 resource 回退到 dtbo。原出厂 DTBO 备份在 `~/rk3576-amp/backup/dtbo-lba40960-factory-20260919.bin` |
| 49152 | Linux `Image-amp-rootfs`（7.36MB，内嵌 initramfs） | `~/rk3576-amp/out/Image-amp-rootfs` sha 8f64e120；旧的无用户态版 `Image-amp` sha dfb0842a |
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
- 界面的调试入口（telnet 里 `echo … > 文件`，界面 250ms 轮询一次）：
  | 文件 | 作用 |
  |---|---|
  | `/tmp/k7-open-camera` | 开相机页 |
  | `/tmp/k7-ask` | 等同点"桌上有什么？"（拍照 + 视觉，回复写 syslog） |
  | `/tmp/k7-rec` / `/tmp/k7-play` / `/tmp/k7-beep` | 录音机录 5 秒 / 放音 / 1kHz 测试音 |
  | `/tmp/k7-vol`（写数字 0~100） | 音量 |
  | `/tmp/k7-say`（写文字） | 直接朗读 |
  | `/tmp/k7-wake`（写 1/0） | 开/关语音唤醒 |
- 唤醒识别留底：`/tmp/k7-wake.log`（每句时长、峰值、增益、识别文字）、`/tmp/k7-wake-{0,1,2}.wav`。
- 板上文件取回主机：nsh 的 `hexdump` 经 telnet 导出，按"at 基址 + 行偏移"拼（示例见会话脚本 pullhex.py 的做法），
  TFTP `put` 用不了（固定 69 端口，WSL 绑不了）。
- 界面文字改完跑 `python3 scripts/check-ui-glyphs.py`：查字库缺字、以及中文字库标签上误用 `LV_SYMBOL_*` 图标（显示成方框）。
  字库由 `scripts/gen-cjk-font.sh` 生成（ASCII + GB2312 一级 + 常用符号，3888 字形）。
- Linux 侧：`bash amp/linux/rootfs/build-rootfs.sh` 出 `~/rk3576-amp/out/Image-amp-rootfs`；
  `flash.sh --stay` 后 `scripts/flash-kernel.sh <Image>` 写 LBA 49152（整区备份 / 回读 / 回滚）。

## 5. 现在能用的（已上板验证）

- 双系统启动、rpmsg、屏幕/触摸、摄像头（`/dev/video0`、`v4l2cap`、相机页约 30fps）、网络（ping/DNS/telnet）。
- **Linux 用户态**：Image 内嵌 initramfs（静态 busybox + `k7d`），开机约 3.1s 进用户态。
  `ampctl exec <命令>` 在 A72 上执行并回显（`rpmsg-raw` → `/dev/rpmsgN`）。k7d 每个通道 fork 一个子进程。
- **界面（kickpi_ui）**，页签 AMP / DEV / LIVE / DESK / AGENT / VOICE / ABOUT：
  - AGENT：中文气泡问答、等待计秒、三个提问按钮、桌面守护、**语音播报**（回答用 mimo-v2.5-tts 念出来）、**语音唤醒**开关与电平。
  - VOICE：**喇叭音量**滑块（ES8388 DAC 硬件音量）、**录音机**（录 5 秒、波形、放音）。
  - DEV：16 项设备节点，✓ / 灰色"未启用"原因 / ✗ 三态。
- **桌面问答**（`k7_vision.c`）：`v4l2cap` 拍 1280x720 → 一次 mimo-v2.5 视觉请求（关深度思考）→ 回答。
  板上实测 4.5~27s（网络波动）。**不再走 ai_agent 循环**（原因见第 6 节）。
- **录音 / 放音**：`k7_audio.c`（pcm1，48k 立体声）。录音开 ALC + 噪声门、差分输入 LIN2-RIN2；
  放音一条流只配置一次；喇叭功放 GPIO2_B1 随放音开关。
- **语音唤醒**（`k7_wake.c` + k7d）："你好 openvela，桌上有什么？" → 识别 → 拍照看图作答 → 朗读。
  openvela 采集 48k→FIR→16k、环形缓冲 12s、经 rpmsg 送 PCM；**A72 的 k7d 做端点检测（VAD）**，回发整句区间；
  openvela 取段 → mimo-v2.5-asr（约 0.9s）→ 匹配唤醒词。识别器会把 openvela 写成"OpenAI / Open Wheel / OPPO VELA / 薇拉"，
  匹配规则靠"你好 + open/vela/维拉…"兜住（说"你好 OpenAI"也会唤醒）。用户 09-19 实测"都识别到了"。
- Token Plan：`https://token-plan-cn.xiaomimimo.com/v1`；文本/视觉 `mimo-v2.5`，TTS `mimo-v2.5-tts`（voice 冰糖，pcm16 24k），
  ASR `mimo-v2.5-asr`（input_audio base64 WAV，asr_options.language=zh）。深度思考用 `"thinking":{"type":"disabled"}` 关。

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
| **（09-19）** 录音全是 0 夹 0x7fxx，8KB 缓冲 1ms 就"收完" | PL330 接收用了 SINGLE 条件，没等请求线 | `rk3576_pl330.c` 改 BURST（照原厂 `_bursts()`） |
| 改 BURST 后 DMA 永远等不到请求 | ①把 RXFIFOLR bit23 当"满"标志（其实是 rfl3 最高位）反复清 RXC ②先开 RXS 后起 DMA，FIFO 0.33ms 溢出 ③es8388 录音超时算成 0 → 20ms，而一个缓冲要 21.3ms | `rk3576_sai.c` |
| 样本全是真值右移一位（0xFFC0 读成 0x7FE0） | 接收早采一位 | `RX_SHIFT` RIGHT(2)→RIGHT(4) |
| 放音"卡一卡的电音"、5 秒放 30 秒 | `sai_send` 每个缓冲都 sai_configure（含复位）+ 启停 | 一条流只配一次，AUDIO_APB_FINAL 才停 |
| 只有耳机响、喇叭不响 | 喇叭功放使能 GPIO2_B1 没人拉高 | 板级 + SAI txhook 随放音开关 |
| 录音回放电流声比人声大 | 麦克风是差分对，驱动按单端 LIN2 采；另外 PGA +24dB、无 ALC/噪声门 | k7_audio 写 0x0A=0xF0 0x0B=0x82 与原厂 ALC 值 |
| 问一次 196 秒 | mimo-v2.5 默认深度思考，agent 三轮 | 关思考（`bsp/upstream/agent-mimo-no-thinking.patch`）+ 直连视觉 |
| agent 一直答"摄像头不可用"、0ms 复用旧回答 | agent 的 camera_capture 拿到 0 字节后靠会话记忆；另有未查清的复用 | 桌面问答绕开 agent |
| pthread 里 waitpid(WNOHANG) 收不到 v4l2cap | 未深究 | 看输出文件"存在且大小稳定" |
| 界面方框 | 字库缺 → 「」；DEV 页用中文字库却放 LV_SYMBOL 图标 | 补字形 + 检查脚本 |

## 7. 未解决（按用户要的顺序）

### 7.1 蓝牙 —— **截止前已停止**（用户决定）
- K7 实焊 SKW6621S（SDIO 0x2a320000）。`bt probe` 卡在枚举：CMD52/CMD5 均 RE、空闲 DAT0 恒低。
- 09-18 用户六轮 A/B（`notes/bt-*-ab-2026-09-18/`）+ 09-19 整体核对（`notes/bt-final-2026-09-19/README.md`）：
  引脚、时钟、电源域、复位时序、Linux 干扰、GMAC1 引脚、IO 电压域在软件侧均与原厂一致；剩余可能在硬件，需要仪器。
- `amp-dual/defconfig` 的 SKW/DWMMC 改动仍是**用户未提交的本地修改**；`chip/rk3576/skw6621s/*.md` 是用户的未跟踪文件，别动。
- 附带：`rk3576_gmac.c` 把 GMAC1 25M 也从 GPIO1_D5（模组 HOST_WAKE）输出，与原厂不一致，后续应去掉（与蓝牙失败无关，已实验排除）。

### 7.2 CPU1（双系统下不接任务）
- 事实：CPU1 启动阶段走完（进 IDLE），但之后绑到 CPU1 的线程永远不跑；单系统四核都正常；
  没有收到 Linux 发来的 SGI（计数为 0）。
- 现为**绕行**：`board_late_initialize` → `kickpi_avoid_cpu1()` 让所有任务避开 CPU1（affinity 0x0d）。
- 诊断补丁（各核启动阶段 / 中断计数 / 外簇 SGI 计数 / `k7diag cpus [test]`）在
  `/tmp/claude-1000/.../scratchpad/diag/` —— 已从源码撤出。下一步：看 CPU1 的中断计数是否在涨
  （不涨 = 关中断卡住；疯涨 = 陷在某个中断里）。**`k7diag cpus test` 会把 nsh 卡住**，用 telnet 跑。
- 怀疑方向：共享 GIC 模式（`CONFIG_ARM64_GICV2_SHARED_DIST`）下的 banked 寄存器 / SGI 配置；
  U-Boot `smc_cpu_on` 的 `sip_smc_amp_cfg(AMP_PE_STATE…)` 是否把配置记到了 A53 core1。

### 7.3 语音 —— 已完成（见第 5 节），可改进
- 麦克风电平偏低：20cm 处说话峰值多在 1000~3800（满量程 32767），送识别前放大到上限 8 倍。
- 唤醒词匹配偏宽（"你好 open…"就算）；若要更严，可换更好识别的唤醒词，或在 Linux 侧做本地模板匹配（MFCC+DTW）。
- 朗读与唤醒互斥：朗读 / 录音机期间唤醒采集暂停，每次恢复丢开头 500ms（ALC 起步冲击）。

### 7.4 其他已知问题
- CIF DMA 为什么多写一行未查（传感器实际行数 > 配置？）。现在靠余量挡着。
- 摄像头画面很暗、噪点多：视觉模型两次都说"像雪花屏"。没有 ISP，RAW 直接转 JPEG；也可能光线 / 镜头朝向。
- eMMC 在 amp-dual 下没开（83b8f3c 打通阶段关掉的，未加回）：`/data` 是 tmpfs，断电即丢。要加回需在 Linux DTB 声明 SDHCI 中断归 openvela。
- ai_agent 进程仍开机自启（飞书等通道），但界面的桌面问答已不经过它。
- 带诊断代码的镜像从 Maskrom 起来时，用户在 **115200** 看到过 nsh —— 原因未查。
- telnet：连上立即 RST 且连发十几次时，个别 `Telnet_session` 卡住不退，占住 8 个预分配 TCP 连接之一（正常断开/间隔 0.5s 的 RST 不漏）。
- xTS：1.3.15 看门狗 4 项过 2 项。根因已坐实：CRU_GLB_RST_ST 被 ATF（闭源 BL31，打印 `reset status: 0x1050` 后）清零，
  openvela 里 `xd 0x27200c04` 读到全 0，所以复位原因报 CHIPPOR。修法（未做）见 `notes/xts-rerun-raw/2026-09-20-watchdog/README.md`。
- NuttX 仓库（`../nuttx`）有未提交修改：`arm64_gicv2.c`（SHARED_DIST 支持，**必需**）、ft5x06、es8388、bt_uart 等，归属待核对，别随手 checkout。

## 7.5 xTS 最终状态（2026-09-20）

**汇总看 `notes/xts-final-2026-09-20.md`**（35 项分 A 严格通过 13 / B 功能通过 18 / C 未达成 4），
逐项原始日志在 `notes/xts-rerun-raw/2026-09-1x…`、`2026-09-20-*`；审计表 `notes/xts-strict-audit.md`。

09-19/20 为通过 xTS 做的改动（都已上板验证）：

- 启动时间 4.47s → **2.76s**（reboot）/ **2.84s**（冷启动）：U-Boot 倒计时 0.2s（`amp/uboot/0010`）、
  触摸/TF/摄像头/界面挪到后台线程 `devinit`（`kickpi_k7_appinit.c`）。
- 开机 logo：`amp/uboot/0011` + `scripts/gen-boot-logo.py`（8 位 RLE8，**最后一行不写 EOL**，
  否则 U-Boot 的 libnsbmp 判 DATA_ERROR）。
- TF 卡按厂商设备树 `no-mmc` 跳过 CMD1（`rk3576_dwmmc.c`），并把 SD / SDIO 的驱动状态拆成两份。
- HYM8563 用 wdog 实现闹钟与周期唤醒（nuttx `ed8f69ad`，已在 `bsp/nuttx-drivers.patch`）。
- `/dev/urandom` 指向硬件 RNG（`rk3576_rng.c` + `DEV_URANDOM_ARCH`）。
- 两个上游补丁：`posixspawn-enoent-not-error.patch`（每条 NSH 命令都多一行 ERROR）、
  `stdio-stream-limit-open-max.patch`（fopen 流数上限 16，NIST 要 32）。
- 测试镜像用叠加配置：`board/kickpi-k7/configs/xts-driver-tests.config`（RTC/NIST）、
  `xts-kasan-longrun.config`（KASAN + showinfo，为塞进 FIT 去掉 ostest/scanftest/cxxtest）。

判读时的坑（踩过）：串口 1.5M 有主机丢字；NSH 横幅与另一核 syslog 逐字节交错，必须子序列匹配；
冷启动计时起点要取上电后第一行 DDR 日志（拔电会产生一个 0x00）。

## 8. 相关记忆文件

`~/.claude/projects/-home-dministrator-linux/memory/`：
`rk3576-amp-emmc-layout.md`、`serial-port-shared-check.md`、`rk3576-official-sdk.md`（原厂 SDK 只读 `git show` 读法）。
