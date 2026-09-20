# xTS 1.3.7 I2C 功能：PASS（2026-09-20，MPU-6050 代替 BMI160）

原文用例要求接 BMI160，跑 `cmocka_driver_i2c_spi`。板上没有 BMI160，用手头的
**MPU-6050**（同为 I²C 六轴器件）代替；命令与判据不变。

## 硬件与接线

厂商规格书《40Pin 引脚定义》：40 针扩展口上默认复用为 I²C 的是 I2C7。

| MPU6050 | 排针 | 信号 |
|---|---|---|
| VCC | 1 脚 | VCC_3V3 |
| GND | 6 脚 | GND |
| SCL | 22 脚 | GPIO3_A0 = I2C7_SCL_M1 |
| SDA | 24 脚 | GPIO3_A1 = I2C7_SDA_M1 |
| AD0 | GND | 地址 0x68 |

（i2c4/5/8 也在这一口上，但设备树标注为 MIPI_CSI0/1/3，是三路摄像头在用，不能占。）

## 链路逐层验证

    nsh> i2c dev -b 7 0x20 0x77
    60: -- -- -- -- -- -- -- -- 68 -- ...        ← 扫到 0x68
    nsh> i2c get -b 7 -a 0x68 -r 0x75
    READ Bus: 7 Addr: 68 Subaddr: 75 Value: 68   ← WHO_AM_I，MPU-6050 本人
    nsh> i2c get -b 7 -a 0x68 -r 0x6b
    READ ... Value: 40                           ← 上电默认 SLEEP=1

开机日志：`MPU6050: /dev/accel0 与 /dev/uorb/sensor_accel0|gyro0 就绪（I2C7:0x68，WHO_AM_I=0x68）`

## 用例结果：PASS

    nsh> cmocka_driver_i2c_spi
    [==========] tests: Running 1 test(s).
    [ RUN      ] drivertest_i2c_spi
    [       OK ] drivertest_i2c_spi
    [  PASSED  ] 1 test(s).

100 次读数（`net.txt`，格式：时间戳 陀螺 x,y,z / 加速度 x,y,z，单位为原始计数）：

    [31449] 286, -162, -358 / -1594, -7018, -16316
    [31742] 286, -151, -346 / -1550, -7034, -16362

数据是真实采样而非常量：加速度 Z 轴约 −16,300 计数，量程 ±2g 下 16384 LSB/g，
即约 **−1.0 g**，与板子平放时的重力方向一致；陀螺静止时在 ±350 计数内抖动。

## 为此写的驱动与踩到的两个坑

`board/kickpi-k7/src/kickpi_k7_mpu6050.c`（新增，配置 `KICKPI_K7_MPU6050`）：
初始化时先核对 WHO_AM_I 再注册，清 SLEEP、选陀螺 X 轴时钟、125Hz、DLPF 44Hz、
±2g/±250°/s。注册两类节点：

- `/dev/uorb/sensor_accel0`、`/dev/uorb/sensor_gyro0`：uORB 框架（float，m/s²、rad/s）；
- `/dev/accel0`：字符设备，返回 `struct accel_gyro_st_s`（原始计数）。

**坑一：用例读的不是 uORB 格式。** `drivertest_i2c_spi.c` include 的是
`<nuttx/sensors/bmi160.h>`，按 16 字节的 `struct accel_gyro_st_s` 读，
而 uORB 的 `struct sensor_accel` 是 28 字节的浮点结构。

**坑二：fetch 型 uORB 节点在阻塞模式下读会卡死。** `drivers/sensors/sensor.c`
的 `sensor_read()` 在未带 `O_NONBLOCK` 时先等 `user->buffersem`，而只实现 `fetch`
的底层不会去 post 它；用例正是阻塞 open，于是读第一帧就永远等下去（实测卡住，
既不返回也不报错）。所以按用例期待的布局另开了 `/dev/accel0` 字符节点。

## 顺带修的上游缺陷

`drivers/sensors/bmi160_base.c`：`bmi160_configspi()` 在 `#ifdef CONFIG_SENSORS_BMI160_SPI`
内，而调用它的 `bmi160_transferspi()` 没有同样的条件编译 —— 只开 I²C（不开 SPI）时
该文件编不过（implicit declaration + unused function）。补上条件编译，见
`bsp/upstream/bmi160-spi-only-guard.patch`。

（本用例的程序 `cmocka_driver_i2c_spi` 的编译开关写死在 apps 的 Makefile 里：
`CONFIG_SENSORS_BMI160` 非空才编。板上没有该器件，这里只把驱动编进去让用例存在，
并不注册它。）
