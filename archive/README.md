# 归档

本目录存放**前身项目**的产物，与当前 RK3576 / KICKPI-K7 项目无关，
保留仅供方法论参考，不参与构建。

## a311y2/

openvela × Amlogic A311Y2（逻极派 LogicPi A1）适配的产出。
该轮因板卡未能到位、且上游 `vendor_amlogic` 仓库尚未创建而终止。

| 文件 | 说明 |
|---|---|
| `nuttx-a311y2.patch` | A311Y2 SoC 层端口（13 文件），含从零编写的 Meson UART 驱动 |
| `vendor-amlogic/` | LogicPi A1 板级工程 |
| `meson-uart-spec.md` | Meson UART 寄存器规格（6 个寄存器的位定义、波特率算法、初始化序列） |
| `*.docx` | 厂商开发板资料 |

**可复用的部分已并入当前项目**：openvela ARM64 端口的分层结构、
六处 Kconfig 注册点、地址一致性校验脚本、以及
`notes/DEBUG-CASES.md` 里的排查方法论。
