# xTS 1.3.16 RNG：nist_sts PASS（2026-09-19）

- 镜像：正常配置 + `board/kickpi-k7/configs/xts-driver-tests.config`；`/dev/urandom` 由
  `CONFIG_DEV_URANDOM_ARCH` 指向本板硬件 RNG（`chip/rk3576/rk3576_rng.c`，自检通过后与 `/dev/random` 一起注册），
  **不是** NuttX 的软件 xorshift128。nuttx 工作区另打 `bsp/upstream/stdio-stream-limit-open-max.patch`。
- 步骤按原文：`cd /tmp`、15 个 `mkdir -p experiments/AlgorithmTesting/*`、`nist_sts 400000`，
  依次输入 `0`、`/dev/urandom/`、`1`、`0`、`10`、`1`，`cat finalAnalysisReport.txt`。
  脚本 `scripts/xts-nist-sts.py`；全程原始字节 `session.raw`，报告 `finalAnalysisReport.txt`。
- 原文未写但必需：NonOverlappingTemplate 读相对路径 `templates/template9`，脚本逐行写入 /tmp/templates 后读回比对（148 行一致）。

## 结果

| 项 | 值 |
|---|---|
| 统计项 | 15 类全部出结果，共 188 行（NonOverlappingTemplate 148、RandomExcursionsVariant 18、RandomExcursions 8 …） |
| 最小 P-VALUE | **0.002042**（> 0.0001） |
| `*` 不合格标记 | **0 处** |
| 通过比例 | 其余均 ≥ 9/10（门槛约 8/10）；RandomExcursions 一行 3/4（该项门槛 3/4） |

判据（原文）：P-Value 大于 0.0001、无 `*` 标记 → **PASS**。

## 两次失败（原始记录保留）

1. 第一版脚本等 `Enter Choice:` 提示：nist_sts 的提示不带换行，NuttX 读 stdin 不刷新 stdout，提示留在板上缓冲里，双方互等。改为输出静默 1.5s 后送答案。
2. `../2026-09-19-nist-sts-stream-limit/`：`LOG FILES COULD NOT BE OPENED. MAX # OF OPENED FILES HAS BEEN REACHED = 11`。
   `lib_fopen.c` 用 `_POSIX_STREAM_MAX`（16）当流数上限，全部 15 项要 32 个日志流。改用 `OPEN_MAX`，见补丁说明。
