# MIPI 面板参考资料

## 来源

厂商的 RK3576 SDK 未公开（`Rk3576-SDK/README.txt`：「RK3576 SDK暂不开放」），
但 Armbian 的板级配置指明了内核仓库：

    config/sources/families/rk35xx.conf
        KERNELSOURCE='https://github.com/armbian/linux-rockchip.git'
        KERNELBRANCH='branch:rk-6.1-rkr5.1'

该仓库公开，两份文件由此取得。

## rk3576-evb.dtsi

RK3576 官方 EVB 的 MIPI DSI 面板定义（`dsi_panel: panel@0`）。

**面板本身是 1080x1920，与本项目的 F050008M01（720x1280）不同**，
但它是一份完整可用的 **RK3576 平台** DSI 配置范例：节点结构、
`dsi,flags` / `dsi,format` / `dsi,lanes` 的取值、
`panel-init-sequence` 的编码格式（每条 `<类型> <延时> <长度> <数据…>`）。
写 DSI 主机与 VOP2 驱动时以它为结构参照。

## rk3308b-mipi-display-v11.dtsi

一块 **720x1280 的 5 寸 MIPI 屏**，驱动 IC 为 `sitronix,st7703`。

    clock-frequency = 65000000     像素时钟 65MHz
    hactive = 720   vactive = 1280
    hfront-porch = 48   hsync-len = 8   hback-porch = 52
    vfront-porch = 16   vsync-len = 6   vback-porch = 15
    dsi,lanes = 4       dsi,format = MIPI_DSI_FMT_RGB888
    dsi,flags = VIDEO | VIDEO_BURST | LPM | NO_EOT_PACKET
    width-mm = 68   height-mm = 121     （对角线约 5.0 寸）
    panel-init-sequence 共 208 行

★ **这不是 F050008M01 的数据，是同规格另一块屏的数据。**

  分辨率、尺寸、通道数、像素格式都对得上，因此 **display-timings 与
  DSI 参数可以作为起点**；但 `panel-init-sequence` 是写给屏上那颗
  驱动 IC 的私有命令，**只有当 F050008M01 也用 ST7703 时才通用**。

  用它点不亮时，不要怀疑 VOP/DSI 的配置 —— 先确认面板 IC 型号。
  可行的确认手段：DSI 链路起来后发 DCS 命令 0x04（read_ddb）或
  0xDA/0xDB/0xDC（read ID1/2/3）读回面板 ID。

  真正对口的数据仍需向 KICKPI 技术支持索取
  `rk3576-kickpi-k7-android-mipi-5-720-1280-F050008M01.dtsi`。

## 顺带：触摸地址

`rk3308b-mipi-display-v11.dtsi` 里的触摸是 `goodix,gt1x` @ **0x14**，
而 KICKPI 文档的示例是 `goodix,gt9xx` @ **0x5d**。

两者都是 GT9xx 系列的合法地址 —— 复位释放瞬间 INT 脚为低选 0x5d、
为高选 0x14。这印证了板级代码里「两个地址都要扫」的写法。
