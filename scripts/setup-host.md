# 主机环境：三个会让人白花半天的坑

## 一、`configure.sh` 说 `failed to refresh` —— 必须装 kconfiglib

```
$ ./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp-dual
...
make: *** [tools/Unix.mk:726: olddefconfig] Error 1
ERROR: failed to refresh
```

### 为什么

`prebuilts/build-tools/linux-x86_64/bin/kconfig-conf` 那一版不认 `osource`
（optional source，Linux 4.18 引入）。工作区里有五处用到它：

```
apps/graphics/lvgl/Kconfig
external/zblue/Kconfig{,.default,.3_0_1}
frameworks/multimedia/media/Kconfig
frameworks/runtimes/quickapp/Kconfig
frameworks/system/utils/Kconfig
```

碰到 `osource` 是 syntax error，之后整棵树的 `if`/`menu` 配对全乱，报错会
指到毫不相干的文件上 —— 实测报在 `apps/tests/testcases/lvgldemo_test/Kconfig`
和 `drivers/hwtracing/tricoreht/Kconfig`，而这两份文件本身没有任何问题。
**第一条错误信息才是真的，后面几十条都是级联。**

### 后果为什么隐蔽

`olddefconfig` 失败时 `.config` 被原样留成 defconfig 的拷贝 —— **select 和
default 没有展开**。而 `amp` / `amp-dual` 的 defconfig 是 `savedefconfig`
的 minimized 产物（只有 238 行），本来就指望 olddefconfig 补全。补不上，
`.config` 里就缺 `CONFIG_I2C`、`CONFIG_SCHED_WORKQUEUE`、
`CONFIG_16550_REGINCR`、`CONFIG_OPENAMP` 这些**从来没人手写过**的符号，
编译在驱动头文件的 `#error` 上炸开：

```
include/nuttx/input/ft5x06.h:63: #error "Work queue support required. CONFIG_SCHED_WORKQUEUE must be selected."
include/nuttx/serial/uart_16550.h:54: #error "CONFIG_16550_REGINCR not defined"
include/nuttx/rptun/rptun.h:36: fatal error: metal/cache.h: No such file or directory
```

错误信息指向驱动，根因在配置工具 —— 这条链子中间隔了三层，不知道的话
会一个一个去"修驱动"。

### 解法

NuttX 自己就支持：`tools/Unix.mk` 里如果 PATH 上能找到 `menuconfig` 命令，
就整体切到 kconfiglib，而 kconfiglib 支持 `osource`、glob 和宽松的
`---help---`。

```bash
python3 -m pip install --user --break-system-packages kconfiglib
export PATH=$HOME/.local/bin:$PATH
```

装完 `make olddefconfig` 会从 258 行展开到 1088 行。**不要**去改工作区里
那几个 `osource`：`external/zblue/` 下它们指向的是 Zephyr 自己那棵树，
用的是 Zephyr 的 Kconfig 方言，换成 `source` 只会把一个错误变成上百个。

## 二、`apps/graphics/lvgl/lvgl` 是 repo 项目，不是 zip 解出来的

`apps/graphics/lvgl/Makefile` 里有一条下载 + 解压规则：

```make
$(LVGL_UNPACKNAME): $(LVGL_TARBALL)
	$(Q) $(UNPACK) $(LVGL_TARBALL)
	$(Q) mv	lvgl-$(LVGL_VERSION) $(LVGL_UNPACKNAME)

ifeq ($(wildcard $(LVGL_UNPACKNAME)/.git),)     # ← 关键
context:: $(LVGL_UNPACKNAME)
endif
```

只有在 `lvgl/.git` **不存在**时才会去下载。而 openvela 的 manifest 里
写着：

```xml
<project path="apps/graphics/lvgl/lvgl" name="apps_graphics_lvgl"/>
```

也就是说这个目录是 **openvela 自己 fork 的 LVGL**（带他们的改动，例如
`lv_nuttx_entry.h` 里**没有** `lv_nuttx_run()`），由 repo 管理。

如果这个目录被删了（我就干过），`make` 会去 GitHub 拉**原版 v9.2.1** 顶上，
然后你会看到一串看起来毫不相关的错误：

```
lv_conf_internal.h:1324: error: expected identifier or '(' before string constant
font_multilang_large.c:1883: error: 'glyph_bitmap' undeclared here
lv_nuttx_image_cache.c:113: error: implicit declaration of function 'gettid'
```

（原版的 `LV_ATTRIBUTE_LARGE_CONST` 会被 Kconfig 的空字符串展开成 `""`；
fork 里没有这个符号。）

恢复办法 —— 不需要联网：

```bash
cd ~/openvela-amlogic/src/apps/graphics/lvgl
rm -rf lvgl v9.2.1.zip && mkdir lvgl
ln -s ../../../../.repo/projects/apps/graphics/lvgl/lvgl.git lvgl/.git
cd lvgl && git checkout -f HEAD
```

对象都在 `.repo/project-objects/apps_graphics_lvgl.git` 里（67MB），
`git checkout -f HEAD` 就能把工作区拉回来。

顺带一提：`mbedtls`、`libuv` 这些**是**下载来的，`configure.sh` 的
distclean 会连 zip 一起删，换配置后第一次 `make` 需要联网重拉。如果
`.config` 里没有 `CONFIG_MBEDTLS_VERSION`（正是坑一的后果），它会去下载
`v.zip` 然后报 `End-of-central-directory signature not found`。

## 三、U-Boot 那边

`make.sh` 需要主机有 `dtc`（内核树里有一份：`kernel-6.1/scripts/dtc/dtc`），
还会检查 `python2` 存在 —— 但 `PYTHON` 变量在整棵树里没有任何使用者，
给一个转发到 python3 的壳脚本即可。
