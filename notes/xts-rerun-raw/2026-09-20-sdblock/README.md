# xTS 1.3.5 Flash 功能：按原文命令在 SD 卡上执行，规模不可行（2026-09-20）

原文：在 Flash 设备上跑 `cmocka_driver_block -m <设备>`。

- 板载唯一 Flash 是 eMMC，其上就是启动镜像，跑破坏性块测试会毁掉系统 —— 不执行。
- 改在 SD 卡（`/dev/mmcsd1`，16 GB，卡上只有先前格式化的空 FAT32）上按原文命令执行：
  `cmocka_driver_block -m /dev/mmcsd1`。

## 结果：进入 `drivertest_block_stress` 后无法在可用时间内完成

三次尝试（19:36 后台、19:39 前台 160 s、19:56 后台 + 连续抓串口 41 min）现象一致：
串口只打出

    [==========] tests: Running 3 test(s).
    [ RUN      ] drivertest_block_stress

之后没有任何输出；板子本身正常（`free`、`ls /dev` 均正常，`/dev/mmcsd1` 在）。

## 原因：测试规模按介质容量线性展开

`apps/testing/drivers/drivertest/drivertest_block.c` 的 stress 子项：

    nsectors = pre->cfg.geo_nsectors * SECTORS_RANGE;   /* 约 95% 容量 */
    for (i = 0; i < nsectors; i++) {
        lseek → write(512B) → fsync → lseek → read(512B) → CRC 比对
    }

这张卡 30,723,072 个扇区，95% 即约 **2,920 万次**"写+fsync+读+CRC"。
即便每次 1 ms 也要 8 小时以上，实际更久，且会把整卡写一遍。
原文用例面向的是几 MB 的 NAND/NOR，那个容量下规模才合理。

## 结论

本项按**未达成**提交：命令与介质都按原文执行了，但在 16 GB 介质上这个用例的
规模不可行；板载 eMMC 不能作为测试对象（其上即启动镜像）。
替代验证（同一张卡）：`mkfatfs -F 32` + 挂载 + 2 MB 写入 + 重挂读回、`fstest` 20/20，
见 `../2026-09-20-btier/`（1.3.2）与 `notes/DEBUG-CASES.md` 案例 13。
