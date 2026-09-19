# 1.3.16 失败记录：stdio 流数上限 16

`nist_sts` 在打开第 6 项 Rank 的 results.txt 时失败（stats.txt 同目录已打开成功，排除目录不存在）：
`ERROR: LOG FILES COULD NOT BE OPENED. MAX # OF OPENED FILES HAS BEEN REACHED = 11`。
其后脚本送出的 `10`、`1` 落到 NSH（`command not found`），报告为空。修复与复测见 `../2026-09-19-nist-sts/`。
