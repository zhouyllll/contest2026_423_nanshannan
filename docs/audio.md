# 音频（ES8388 + SAI1）

## 硬件

麦克风与喇叭在**物理上是两个外设**，但**电气上都走同一颗 ES8388**：
喇叭经 DAC 出，麦克风经 ADC 入。原厂 Android 的证据：

```
/proc/asound/cards:  0 [rockchipes8388]
/proc/asound/pcm:    00-00: ... : playback 1 : capture 1
/dev/snd/:           pcmC0D0p（放音） pcmC0D0c（录音）
```

板上**没有**独立的麦克风声卡：两个 PDM 控制器与 `pdm-mic-array` 在原厂
dtb 里均为 `disabled`，`dummy-codec` 也是 disabled。唯一启用的音频卡是
`/es8388-sound`。

★ 这一条推翻了"麦克风是另一个设备、所以要另找驱动"的设想 —— 物理形态与
  电气通路是两回事，判断该走后者。

## 原厂 ES8388 寄存器实测值

从运行中的原厂系统 dump（`/sys/kernel/debug/regmap/3-0010/registers`，
即 I2C3 地址 0x10）：

```
00:36  01:60  02:00  03:f9  04:c0  06:00  07:7c  08:00
09:00  0a:00  0b:02  0c:4c  0d:02  0e:30  0f:30
12:ea  13:c0  14:05  15:06  16:53  17:18  18:02  19:02
1a:00  1b:00  1c:08  1d:06  1e:1f  1f:f7  20:fd  21:ff
22:1f  23:f7  27:b8  28:38  29:38  2a:b8  2b:80  2c:38
```

与原厂驱动 `kernel-6.1/sound/soc/codecs/es8323.c` 的初始化序列逐条对得上
（`00:36 01:60 09:00 0a:00 0b:02` 完全一致）。

关键项：

| 寄存器 | 值 | 含义 |
|---|---|---|
| `0x09` ADCCONTROL1 | `0x00` | 麦克风 PGA 增益 0dB |
| `0x0a` ADCCONTROL2 | `0x00` | 输入选择 = LINPUT1/RINPUT1 |
| `0x03` ADCPOWER | `0x59`（录音时） | AINL+ADCL 上电，右声道关断 —— **单声道左输入** |
| `0x03` ADCPOWER | `0xFF`（空闲） | 全关断，ADC 按需上电 |

★ dump 到的 `03:f9` 是**没有录音流时**的值。原厂驱动只在音频激活时写
  `0x59`，所以静态 dump 看不到真实的录音配置 —— 读驱动源码才拿得到。

## NuttX 侧现状

- `/dev/audio/pcm0` 已注册（ES8388@I2C3:0x10 + SAI1，外套 PCM 解码层）
- 已启用 `nxlooper` / `nxplayer` / `nxrecorder`
- **修掉一个上游缺陷**：`nxlooper` 的设备搜索是 if/else-if 链，
  一个同时支持收放的全双工设备只会被当成放音设备，然后就没有设备可当
  录音 —— 一颗恰好能胜任回环测试的编解码器，正因为它两样都能做而被拒绝。
  见 `bsp/upstream/nxlooper-fullduplex.patch`。
- 修复后 `loopback 2 16 48000 1 0` 正常启动，待人耳确认出声
- `nxrecorder` 仍报"No suitable Audio Device found"，其搜索除能力位外还多
  查一层格式匹配，未定位


## 录音链路：已修好的五层，与仍未通的最后一层

上层问题已逐层修掉（详见 DEBUG-CASES 案例 26）：

| 层 | 问题 | 处置 |
|---|---|---|
| 应用 | nxlooper 拒绝全双工设备 | 已修，`bsp/upstream/nxlooper-fullduplex.patch` |
| 应用 | nxrecorder 自动搜索失败 | 需显式 `device /dev/audio/pcm0` |
| 设备 | `pcm0` 外套 `pcm_decode`（放音用的 WAV 解码器）会解析空的录音缓冲区 | 板级另注册裸编解码器为 `/dev/audio/pcm1` 专供录音 |
| 上半部 | `ALLOCBUFFER` 成功却不分配 | 必须先调 `AUDIOIOC_GETBUFFERINFO`——`upper->nbuffers` 是被这个"查询"操作赋值的 |
| 参数 | `CONFIGURE` 返回 ERANGE | 位深在 `ac_controls.b[2]`，不是 `b[3]` |

**仍未通的是最底层：SAI 的接收侧。**

原本 `i2s_ops_s` 里**只有发送**（`.i2s_send` 等），`I2S_RECEIVE()` 是空指针，
所以录音方向永远产不出数据且不报错。已补上轮询式实现，但上板会**挂住板子**，
需要物理复位。

### 与原厂实现的差距（`kernel-6.1/sound/soc/rockchip/rockchip_sai.c`）

| | 原厂 | 我们 |
|---|---|---|
| 数据搬运 | **DMA**（`rockchip_sai_dma_ctrl`） | 轮询 PIO |
| 启停 | `regmap_update_bits` 只翻方向位，CLK/FSS 在别处配 | 整寄存器覆写 |
| 清除 | 按方向分别清（放音 TXC / 录音 RXC），且在**停止时**清 | 配置前一起清 TXC\|FSC |
| 清除超时 | 容忍（回退整体复位后仍返回 0） | 只告警 |

★ 我们的发送侧用轮询能工作，是因为放音是"我们主动往 FIFO 里灌"；接收是
  "等对方来数据"，轮询在时序上要脆弱得多。要做稳，接收侧应当照原厂走 DMA。

### 下次继续的入手点

1. 先按原厂改启停方式：只用读改写翻 `RXS` 位，不整寄存器覆写；清除按方向分开。
2. 再考虑接收走 DMA（工作量明显更大，但这是原厂的做法）。
3. 每次改动前确认：**上界设在整件事上**，不是每一次等待上（这个错误在本
   项目里已犯三次，见案例 26 与提交记录）。
