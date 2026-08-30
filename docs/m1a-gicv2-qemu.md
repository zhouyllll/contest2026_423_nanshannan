# M1a：在 QEMU 上跑通 GICv2 路径

> RK3576 用的是 GIC-400（GICv2），与 RK3399/RK3568 的 GIC-500、RK3588 的
> GIC-600（均为 GICv3）不同，是本项目**唯一的新知识点**。
> 本阶段在 QEMU 上先行验证，**完全不依赖开发板**。

## 结果：通过

```
$ cd nuttx && ./tools/configure.sh -e qemu-armv8a:nsh_gicv2 && make -j
$ ../contest2026_423_nanshannan/scripts/run-qemu.sh 2

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 379868bc Aug 30 2026 22:14:12 arm64 qemu-armv8a
nsh> ps
  PID GROUP PRI POLICY   TYPE    NPX STATE    EVENT     ...  COMMAND
    0     0   0 FIFO     Kthread   - Ready              ...  CPU0 IDLE
    1     0 192 RR       Kthread   - Waiting  Semaphore ...  hpwork
    2     2 100 RR       Task      - Running            ...  nsh_main
nsh> free
      total       used       free    maxused    maxfree  nused  nfree name
  130125816      15304  130110512      15984  130110512     25      1 Umem
nsh> getprime
thread #0 started, looking for primes < 10000, doing 10 run(s)
thread #0 finished, found 1230 primes, last one was 9973
getprime took 83 msec
```

三件事同时被证明了：

1. **串口输入能收** —— 命令是从 stdin 喂进去的，NSH 收到并执行了。
   这说明走的是**中断驱动的接收路径**，不只是轮询发送。
   GIC 配错最典型的症状恰好是「能打印但收不到输入」，本项验证直接排除它。
2. **定时器中断与抢占式调度正常** —— `getprime` 以 `SCHED_RR` 跑完 10 轮并计时。
3. **链接进去的确实是 GICv2** —— `libarch.a` 中为 `arm64_gicv2.o`，
   `.config` 中 `CONFIG_ARM64_GIC_VERSION=2`。

> `nxposix_spawn_exec: ERROR: exec failed: 2` 是 NSH 的正常行为：
> 先尝试按文件路径 exec，失败后回退到 builtin。不是故障。

## 读 `arm64_gicv2.c` 得到的三条对本端口有用的结论

### ① GICv2 与 GICv3 的配置面完全不同

| | GICv2 用到的 CONFIG | GICv3 用到的 CONFIG |
|---|---|---|
| 基址 | `CONFIG_GICD_BASE`、`CONFIG_GICR_BASE` | `CONFIG_GICR_BASE`、`CONFIG_GICR_OFFSET` |
| 其它 | `ARM64_GICV2M`、`ARM64_GICV2_LEGACY_IRQ0`、`ARM_GIC_EOIMODE` | `ARCH_ARM64_EXCEPTION_LEVEL`、`ARM64_GICV3_SPI_EDGE`、`ARM64_GICV3_SPI_ROUTING_CPU0`、`ARM64_DECODEFIQ` |

两点值得注意：

- **GICv3 根本不用 `CONFIG_GICD_BASE`**，GICv2 才用。
- **`CONFIG_GICR_OFFSET` 只有 GICv3 用**（每 CPU redistributor 的跨距）。
  GICv2 没有 redistributor，本端口的 `chip.h` 中**不应定义该宏** —— 已确认删除正确。
- GICv2 路径下，`CONFIG_GICR_BASE` 承载的是 **GICC（CPU 接口）** 地址，
  不是 v3 的 redistributor。RK3576 取 `0x2a702000`。

### ② 驱动会在初始化时自检 GIC 版本

```c
static int gic_validate_dist_version(void)
{
  reg = getreg32(GIC_ICCIDR) & GIC_ICCIDR_ARCHNO_MASK;      /* CPU 接口 IIDR */
  if (reg == (0x2 << GIC_ICCIDR_ARCHNO_SHIFT)) return 0;

  reg = getreg32(GIC_ICDPIDR(GIC_ICPIDR2)) & GICD_PIDR2_ARCH_MASK;  /* GICD+0xFFE8 */
  if (reg == GICD_PIDR2_ARCH_GICV2) return 0;               /* 0x20 = v2 */

  sinfo("GICv2 not detected\n");
  return -ENODEV;
}
```

**这是一个有用的安全网**：如果 RK3576 的 GICD/GICC 基址填错，或版本判断错了，
`arm64_gic_initialize()` 会返回 `-ENODEV` 并打印 `GICv2 not detected`，
而不是静默死掉。`GICD_PIDR2_ARCH_*` 的取值：v2 = `0x20`、v3 = `0x30`、v4 = `0x40`。

### ③ 串口先于 GIC 初始化，所以上面那条诊断看得见

调用顺序：

```
arm64_head.S
  └─ arm64_chip_boot()            rk3576_boot.c
       ├─ arm64_mmu_init()
       ├─ arm64_psci_init("smc")
       ├─ rk3576_board_initialize()
       └─ arm64_earlyserialinit()  ← 串口在这里就绪
  └─ nx_start()
       └─ up_initialize()          nx_start.c:714
            └─ up_irqinitialize() → arm64_gic_initialize()  ← GIC 在这之后
```

**含义**：上板时如果串口已经出字但 GIC 有问题，能看到具体诊断；
反过来如果连字都没有，问题就在串口/加载地址/MMU，与 GIC 无关。
这条把 M1 之后的排查分成了两个互不干扰的区间。

## 复现方式

```bash
cd <工作区>/nuttx
make distclean && rm -f .config Make.defs
./tools/configure.sh -e qemu-armv8a:nsh_gicv2
make -j$(nproc)
../contest2026_423_nanshannan/scripts/run-qemu.sh 2     # 退出：Ctrl-A 然后 x
```

`run-qemu.sh` 第一个参数是 GIC 版本（2 或 3，默认 3）。
