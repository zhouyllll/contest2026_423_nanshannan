# K7 关键驱动 Mode C 审查（2026-09-15）

审查对象：RK3576 GMAC/以太网、IMX415/CSI/V4L2 摄像头、SAI1/ES8388 音频。按 `.claude/agents/driver-workflow.agent.md` Mode C 和 `driver-code-reviewer` 的两轮规则只读审查；未修改驱动。以下评分为源码静态审查结果，不代表上板回归通过。维度满分依次为 L1-1 20、L1-2 20、L1-3 10、L1-4 10、L1-5 10、L1-6 15、L1-7 15；单维扣分不低于零，Critical 将总分封顶 60。

| 驱动 | L1-1 | L1-2 | L1-3 | L1-4 | L1-5 | L1-6 | L1-7 | 总分 | 结论 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 摄像头/视频 | 5 | 20 | 7 | 2 | 10 | 15 | 15 | 60* | NEEDS_FIX |
| 网口 | 20 | 12 | 2 | 7 | 7 | 15 | 15 | 78 | NEEDS_FIX |
| 音频 | 20 | 20 | 10 | 2 | 10 | 15 | 15 | 92 | PASS（带 High 问题） |

`*` 摄像头原始分数 74，因 Critical 封顶 60。L1-8 设计健康度：摄像头 10/10，网口 10/10，音频 10/10；未对需求设计文档做逐条一致性评分。

## 需要先处理的问题

1. **Critical｜摄像头缓冲区部分分配失败后状态不完整**，L1-1 / P-01。`board/kickpi-k7/src/kickpi_k7_camera.c:632-690` 仅在 `g_cam_buf[0] == NULL` 时分配三块；第 0 块成功、第 1/2 块失败时直接返回，后续调用跳过分配并可能把空地址交给 CIF。应在失败路径释放所有已分配块并置空，进入 DMA 前校验全部指针。
2. **High｜GMAC 中断状态会被覆盖**，L1-2 / P-09。`chip/rk3576/rk3576_eth.c:1868-1894` ISR 用赋值写 `priv->dmasr`，但 `irqwork` 已排队时不再排新 worker；新中断可能覆盖旧状态，worker `:1749-1785` 只处理最后一份。应用临界区保护的按位累积，并让 worker 循环取走待处理状态。
3. **High｜首段录音仍走诊断 DMA 模式**，L1-4 / P-32。`chip/rk3576/rk3576_sai.c:853` 在 `g_rx_nohs_done` 初值为 false 时选择 `DMA_MEM_TO_MEM` 读取 RXDR，`:1233` 后才切回 `DMA_DEV_TO_MEM`。首段仍回调给正常上层，可能污染 ASR/唤醒。诊断路径应仅在显式调试配置启用，正式录音从首段使用外设握手。
4. **Medium｜视频流启动失败未回滚接收端**，L1-3 / P-20。`board/kickpi-k7/src/kickpi_k7_video.c:190-203` 已启动 CSI receiver 后直接返回 `kickpi_camera_stream(true)` 的失败值，接收端保持运行。错误路径关闭 receiver。
5. **High｜网口 down 未禁用通道 IRQ**，L1-3 / P-20。`chip/rk3576/rk3576_eth.c:2066-2067` ifup 启用 EMAC 和 EMAC_CH0，`:2143` ifdown 只禁 EMAC。应对称禁用通道 IRQ 并清理待处理 work。
6. **High｜`is_available` 不代表传感器探测成功**，L1-4 / P-32。`board/kickpi-k7/src/kickpi_k7_video.c:104-112` 使用 `kickpi_camera_status() >= 0`；后者报告 CSI host 状态而非 `g_cam_found`，缺传感器时仍可能报告可用。应读取实际探测状态。

## 次要问题与待确认

- **Medium｜短帧长度下溢**，L1-5 / P-39：`rk3576_eth.c:1312` 对描述符 PL 无条件减 4，再赋给 `uint16_t d_len`；PL<4 时变成很大的长度。先校验 PL≥4，再减 CRC 长度。
- **Medium｜网卡注册结果未检查**，L1-4 / P-32：`rk3576_eth.c:3627` 丢弃 `netdev_register()` 返回值，失败后仍返回 OK。
- **WARNING｜网卡初始化失败回滚不全**，L1-3 / P-20：`rk3576_eth.c:3562-3619` 第二个 `irq_attach` 或 PHY 初始化失败时未卸载已挂的 IRQ；需核对初始化只执行一次的生命周期。
- **WARNING（暂不扣分）｜音频首缓冲日志固定读四个 32 位字**：`rk3576_sai.c:1228-1240` 未验证 `rx_result >= 16`。如上层允许更短缓冲会越界；需结合 I2S/PCM 调用约束确认。

两轮中曾提出 `rk3576_sai_send()` 的 `ret` 未初始化。复读 `rk3576_sai.c:1297-1310` 可见它先接收 `nxmutex_lock()` 的返回值，成功为 0，故该项是误报，已剔除。

修复优先级：摄像头缓冲失败路径 → 首段录音 DMA 模式 → GMAC 中断状态与 IRQ 对称性 → 视频启动回滚与探测状态 → 初始化/边界检查。Mode C 仅审查；修复后需分别上板验证相机连续拍摄、录音首段、网口收发与 ifdown/ifup 循环。
