# SDIO reset/clock ordering comparison

Baseline: 22239d0 HOLD-enabled probe. Changed only reset-release ordering:
configure card clock while disabled, release module reset, wait 200 ms,
enable card clock, wait 2 ms, issue CMD0/CMD5. This matches the ordering in
Linux mmc_power_up() and pwrseq_simple post_power_on. Both existing enable
pins are still toggled; pulls, clock dividers and HOLD setting are unchanged.

Build and FIT readback passed. FIT SHA256:
bbcf5d8a56191e2743fb0fa8bc47c0532589ce39563d9743f20a476d9c86a80d.
Previous FIT region backup:
/tmp/k7-amp-fit/flash-20260917T174531Z/lba24576-8192-before.bin

Board result: CMD0 RINTSTS=0x4; CMD5 RINTSTS=0x2, RESP0=0; host probe -5.
The ordering change alone did not resolve enumeration. CMD0 has no response,
so its completion does not demonstrate a functioning module response path.
Raw serial output has dropped bytes; do not reconstruct a strict PASS.
Board now runs this reset-before-clock comparison image.

This experiment changes reset versus card-clock ordering, not a physical
power-rail cycle. It does not exclude voltage, reset pulse, pull, clock phase,
or cold-start problems. Next collect pin/clock register readbacks and compare
with the known-working Android environment before changing further variables.
