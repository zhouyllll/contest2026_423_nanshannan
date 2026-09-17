# SDIO HOLD single-variable comparison

Changed only the probe command HOLD bit to match the Rockchip Linux host and
our normal sendcmd path. No pull, clock divider, or reset timing changes.

- Baseline bt probe: CMD0 RINTSTS=0x4; CMD5 RINTSTS=0x2, RESP0=0; ret=-5.
- HOLD enabled bt probe: same result; NSH remained responsive.
- Incremental build and git diff --check passed.
- FIT SHA256: 4c77272b580acaa2f0084e24a867ac9f49ef2a4ef455210969e81bbdec0bd2fc.
- FIT flashed at LBA 24576 and exact readback verified before reset.
- Previous region: /tmp/k7-amp-fit/flash-20260917T172158Z/lba24576-8192-before.bin.
- Host wall-clock labels are archival identifiers, not timing evidence.
- UART logs have missing bytes; key CMD5 status and return code are readable.

Result: enabling HOLD alone did not resolve CMD5. This does not rule out other
clock/phase problems. Do not bypass response errors or claim SDIO enumeration.
The board is running the HOLD-enabled comparison image. Next isolate pin pull
configuration with register readback; do not change multiple factors together.

References: Linux drivers/mmc/host/dw_mmc.c dw_mci_prepare_command defaults to
USE_HOLD_REG; only the Exynos host sets CARD_NO_USE_HOLD in this local tree.
SKW enumeration is handled by MMC core before the vendor SDIO probe.
