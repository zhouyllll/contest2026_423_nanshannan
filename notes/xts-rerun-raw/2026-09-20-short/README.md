# xTS 补测：1.1.5 / 1.1.12 / 1.2.4 / 1.3.4（2026-09-20）

镜像：ecc7cba 正常镜像（`ed8f69ad-dirty Sep 20 2026 07:35:35`），经 telnet NSH 执行；
cmocka 与 md5_test 的输出走 syslog，所以另存串口原始字节。

## 1.1.5 getprime：PASS

    thread #0 finished, found 1230 primes, last one was 9973
    Done
    getprime took 149 msec

## 1.1.12 md5：PASS（100/100 一致）

`echo xts-md5-test-file > /tmp/1.txt; md5_test -f /tmp/1.txt -c 100`
→ 串口 100 行，全部 `e8561e8afce7de09c0d527db5df887ca`（`serial-md5.raw`），
与主机 `printf 'xts-md5-test-file\n' | md5sum` 逐字相同。

偏差：原文用 `/etc/1.txt`。本配置没有 ROMFS，板上没有 /etc（`ls /etc` → stat failed: 2），改用 /tmp/1.txt。
旧记录"100 次只收到 99 行"已确认是串口丢行，不是测试问题。

## 1.2.4 Flash 占用

openvela 侧 `df -h` 只有内存文件系统（本核不在 Flash 上挂任何文件系统，镜像由 U-Boot 从 eMMC 读进 RAM 执行）：

    tmpfs 19K /data   procfs /proc   tmpfs 512B /tests   tmpfs 512B /tmp

Linux 侧（A72，`ampctl exec df`）：根文件系统是 initramfs，运行时同样不占 Flash。

硬件 Flash：eMMC 29824 MB（SAMSUNG，`rkdeveloptool rfi`）。本项目实际写入的区域：

| 分区 | LBA | 容量 | 我们写入 | 占该分区 |
|---|---|---|---|---|
| uboot | 16384 | 4 MB | uboot.img 3,145,728 B | 75% |
| trust | 24576 | 4 MB | AMP FIT（openvela）3,572,224 B | 85% |
| dtbo | 40960 | 4 MB | 开机 logo resource 82,432 B | 2% |
| vbmeta+boot 起 | 49152 | 65 MB | Linux Image-amp-rootfs 7,358,976 B | 11% |
| security | 8192 | 4 MB | dtb（LBA 14336）+ OP-TEE 安全存储 | — |

合计约 14.2 MB / 29824 MB（0.05%）。其余分区（super、userdata 等）本项目未使用。

## 1.3.4 RAM 随机读写：PASS

原文命令：`mkrd -m 10 -s 1000 1024` → `/dev/ram10`（堆 used +1 MB），
`cmocka_driver_block -m /dev/ram10` → `serial-driver-block-ram10.raw`：

    drivertest_block_stress OK / drivertest_block_single_write OK / drivertest_block_cache_write OK
    [  PASSED  ] 3 test(s).
