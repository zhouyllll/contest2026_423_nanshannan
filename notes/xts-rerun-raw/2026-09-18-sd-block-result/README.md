# 1.3.5 原版整卡测试结果及 2 GiB 边界调查

命令 cmocka_driver_block -m /dev/mmcsd1 已完成；单调时钟耗时4824.817秒。
结果 FAIL：stress 的读回 output_crc != input_crc；single_write 和
cache_write 通过，共2通过、1失败。原版测试未打印失败扇区，无法追溯
第一次差异的精确位置。SD 卡部分原数据已覆盖，没有自动恢复或格式化。

结束汇总在 20260918T131751646628Z-186590997785926-dmesg-c.raw。
monitor.log 包含周期采集；主命令 raw/text/events 含正常返回提示符。
提示符不是 PASS 依据，必须检查上述失败汇总。

## 定位证据

原运行配置 CONFIG_FS_LARGEFILE 未启用，include/sys/types.h 使用
int32_t off_t 和 uint32_t blkcnt_t。bch_seek 拒绝负偏移，返回 EINVAL。
原版 stress 两次 lseek(i * sector_size) 都未检查返回值。

只读验证：
- dd if=/dev/mmcsd1 of=/dev/null bs=512 skip=4194303 count=1：正常返回。
- dd if=/dev/mmcsd1 of=/dev/null bs=512 skip=4194304 count=1：
  skip lseek failed: 22。

这证明2 GiB处的定位缺陷，且可解释大范围压力测试CRC差异，但由于未记录
首次失败扇区，不能断言所有问题均由此导致，或排除SD卡/驱动其它问题。

修复方向：独立 xts-storage.config 启用 CONFIG_FS_LARGEFILE=y，
重新构建后先复测边界读取，再运行相同原版整卡命令。完成前不改判 PASS。
完整日志在本目录 run/；原运行镜像和日志配置见此前 ramlog 归档。
