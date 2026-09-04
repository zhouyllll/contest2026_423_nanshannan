# 参考资料索引

本目录放的是大赛与 openvela 官方文档。**芯片侧最重要的两份不在这里**，
体积太大，只记位置。

## ★ Rockchip 官方 RK3576 Linux SDK

```
~/openvela-rk3576/docs/Rk3576-SDK/1-SDK软件源码/Linux/sdk/20260320/
  rk3576-linux-20260320/rk3576-linux/       # 只有 .git，工作树未检出
```

一个 git 仓库里装下了整套：`u-boot/`、`kernel-6.1/`、`buildroot/`、
`device/`、`rkbin/`。

### 不要检出，直接从 git 读

工作树没检出（5.1GB 的 `.git`），检出一次要十几 GB。要看哪个文件就：

```sh
cd .../rk3576-linux
git show HEAD:u-boot/drivers/video/drm/rockchip_vop2.c > /tmp/x.c
git ls-tree -r --name-only HEAD | grep -i vop2      # 找路径
```

### 芯片侧最该抄的几份

| 路径 | 用来回答 |
|---|---|
| `u-boot/drivers/video/drm/rockchip_vop2.c` | U-Boot 到底怎么配的显示，寄存器写入顺序与取值 |
| `kernel-6.1/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c` | 同一件事的第二份独立实现，用来交叉验证 |
| `kernel-6.1/drivers/gpu/drm/rockchip/rockchip_vop2_reg.c` | 各 SoC 的窗口能力表、寄存器差异 |
| `device/rockchip/` | 板级 dts、分区表、打包配置 |

## ★ RK3576 TRM / 数据手册 / 原理图

```
docs/vendor/RK3576-Datasheet/          TRM Part1、Part2、Datasheet
docs/vendor/KICKPI-K7（规格书+原理图+机械图）/
```

TRM 是文本层完好的 PDF，直接用 `pdfgrep` / `pdftotext -layout` 查：

```sh
pdfgrep -n "ESMART0_REGION0_VIR" Rockchip_RK3576_TRM_Part2_*.pdf   # 给页码
pdftotext -f 1560 -l 1575 -layout Rockchip_RK3576_TRM_Part2_*.pdf out.txt
```

## ★ 两者的分工 —— 先读哪个

**TRM 说的是寄存器"是什么"，SDK 说的是它"该怎么用"。写驱动时要抄的是
后者。**

这一条是拿教训换来的（`notes/DEBUG-CASES.md` 案例 15）：VOP2 的图层显示
起点 `DSP_ST`，TRM 只写「offset in panel」五个字，从 RK3568 照搬过来的
"加回消隐段"的写法在 RK3576 上是错的，屏上出斜纹。查 TRM 查了半天、
还设计了一套七步二分实验，而 SDK 里 U-Boot 和内核两处实现摆在那里，
三分钟就能对出来。

顺序应当是：**SDK 实现 → TRM 字段表核对 → 上板验证**。
只有 SDK 里没有的（比如 openvela 特有的框架接线）才回头啃 TRM。
