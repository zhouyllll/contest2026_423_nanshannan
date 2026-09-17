# RK3576 SDIO pre-CMD5 reference comparison

Reference root: /home/dministrator/rk3576-amp/kernel-6.1.
This is local SDK source analysis, not proof that every conditional branch
matches the shipped Android kernel build.

## Call chain

SKW Rockchip scan: skw_sdio_host.c:35 skw_sdio_mmc_scan ->
rockchip_wifi_power(1), 150 ms wait, carddetect -> MMC rescan.
core.c:mmc_rescan_try_freq -> mmc_power_up -> pwrseq_pre_power_on ->
MMC_POWER_UP/set_ios -> initial signaling voltage -> stabilization ->
pwrseq_post_power_on (reset release + DT 200 ms) -> f_init/MMC_POWER_ON ->
set_ios/setup_bus -> stabilization -> sdio_reset(CMD52) -> CMD0 ->
mmc_attach_sdio -> mmc_send_io_op_cond(CMD5/R4).
Only successful enumeration reaches the SKW SDIO function driver.

## Comparison

| Item | SDK reference | Current NuttX probe |
|---|---|---|
| Host | mmc@2a320000; HCLK/CCLK; SDGMAC | same instance, separate clocks and reset |
| Pins | GPIO1 B4-B7/C0/C1 function 2, pull-up, drive level 2 | mux set; pull/drive not explicitly set; measure inherited state |
| PWREN | dw_mci_set_ios MMC_POWER_UP sets slot bit regardless of external power-pin mux | previously skipped on SDIO; this experiment sets bit 0 |
| CTYPE | explicit 1-bit identification mode | not explicit in probe; readback added |
| UHS | clears DDR for legacy; signaling set via voltage path | not explicit in probe; needs readback |
| Clock | ciu=f_init*2, bus_hz=ciu/2, divider from bus_hz | xin24m/60, CLKDIV=0; 200 kHz by SDK formula, not claimed 400 kHz |
| Phase | set_ios adjusts phases only if clock handles valid; init contains internal-phase handling | no explicit phase setup; must examine actual USRID and matching init branch |
| Reset | release, 200 ms delay, then f_init | aligned in ecaedf6; did not resolve CMD5 |
| HOLD | enabled in Rockchip command path | aligned in 22239d0; did not resolve CMD5 |
| Initial commands | CMD52 reset, CMD0, CMD5 (no-sd skips CMD8) | CMD0, CMD5; CMD52 reset omitted |
| CMD5 | short response, no CRC, RTO/RESP_ERR fatal | same response/CRC rule; errors retained |
| OCR | query once with 0, then negotiate host-supported voltage, up to 100 tries | 20 tries, uses returned 24-bit field; later-stage difference, not explanation for first R4 failure |

CMD0 completion demonstrates host progress but no returned card response.
Do not equate RESP_ERR with measured card activity. Do not implement blind
phase scanning or disable error checking before checking reference state.
