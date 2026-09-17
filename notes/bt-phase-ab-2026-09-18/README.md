# TRM-guided phase comparison

Baseline 437992d. Internal-phase USRID read 0x20230001.
Before: drive=0x4 (180 degrees), sample=0 (0 degrees).
After TRM init_state sequence: drive=0x2 (90 degrees), sample=0.
CMD0: RINTSTS=4. CMD5: RINTSTS=2 RESP0=0, host probe ret=-5.
Result: reference phase configuration alone did not resolve enumeration.
CMD GPIO1_16 and CLK GPIO1_17 explicitly read mux=2 pull=1; D1-D3 also
read mux=2 pull=1. D0 line remains truncated; do not infer missing bytes.
PWREN=1, CTYPE=0, CLKENA=1, DIV=0, SRC=0.

Build (build-trm.log), git diff --check and FIT readback passed.
FIT SHA256 0276d05abf8eefa6dcb5878489881e1a88a1efdf1a868a48b1bf6dda6fd2a894.
Backup: /tmp/k7-amp-fit/flash-20260917T175656Z/lba24576-8192-before.bin.
Board runs this comparison image; NSH responds after probe.

Next compare actual card-clock rate, pad drive/voltage and vendor CMD52-reset
sequence. If software state matches but CMD5 still fails, acquire CMD/CLK
waveforms or runtime register dumps from a known-working Android boot.
No error bits were suppressed. No SDIO/Bluetooth PASS is claimed.
