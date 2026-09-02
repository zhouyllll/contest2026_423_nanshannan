# LVGL 侧的两处改动

LVGL 不在 manifest 里，是构建时按 `apps/graphics/lvgl/Makefile` 从
GitHub 下载 v9.2.1 的 zip 解包而来（目录 `apps/graphics/lvgl/lvgl/`）。
因为不是 git 仓库，改动无处提交，**干净构建会丢失**，所以在这里存档。

复现步骤：

```sh
cd apps/graphics/lvgl
curl -L -O https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.1.zip
unzip -q v9.2.1.zip && mv lvgl-9.2.1 lvgl && touch lvgl
patch -p1 -d lvgl < ../../../contest2026_423_nanshannan/bsp/lvgl/0001-nuttx-image-cache-include-unistd.patch
```

## 0001 —— lv_nuttx_image_cache.c 缺 unistd.h

`lv_nuttx_image_cache.c` 调用 `gettid()`，但没有包含声明它的
`<unistd.h>`，在开了 `-Werror=implicit-function-declaration` 的
openvela 构建里直接编译失败。属 LVGL 上游缺陷，值得回报。
