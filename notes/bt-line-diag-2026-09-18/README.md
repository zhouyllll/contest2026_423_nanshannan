# SDIO idle-pad and failed-command observation

Baseline a429470. Added read-only GPIO EXT_PORT samples and host status reads.
The diagnostic adds small delays; it does not change mux, pulls, voltage,
phase or clock dividers. UART output still drops some bytes.

Observed:
- CMD GPIO1_16 high in 64/64 samples; DAT0 GPIO1_12 high in 0/64.
- Idle STATUS=0x206: command FSM[7:4]=0; inverted DAT0 bit9=1;
  DAT3 bit8=0. Non-removable slot, so DAT3 must not be used to claim absence.
- UHS=0. This is a controller register, not a measurement of VCCIO voltage.
- CMD52 read/reset: RE=1, RESP0=0.
- CMD0 completes. CMD5 immediate RINTSTS=2 RESP0=0.
- After delay, CMD=0x20000045 (START clear), STATUS=0x206;
  settled RINTSTS=6 (CMD_DONE and RE), RESP0=0; CMD pad=1.
- Probe returns -5 and NSH remains responsive.

Interpretation: CMD_DONE arrives later than RE but does not remove RE.
Do not suppress RE or treat this as successful enumeration. DAT0 low is
an electrical/state clue, not proof the module is defective or absent.
GPIO EXT_PORT sampling is not a substitute for oscilloscope measurements.
The D0 mux/pull diagnostic line is truncated; its complete readback still
needs capture, despite the other pins reporting mux=2 pull=1.

Next: verify D0 mux/pull readback and pad voltage/drive configuration;
compare CMD/DAT idle levels and initialization register values with the
known-working Android system or measure at the module. Do not turn off
power rails or change voltage based solely on UHS register bits.

FIT SHA256 289303c33841b5b9c72578a810962f31e80a50a37ae07b1b0fa4450b2f31d55f.
Build/readback passed. Backup:
/tmp/k7-amp-fit/flash-20260918T010811Z/lba24576-8192-before.bin.
Board runs this diagnostic image. See TRM Part1 p1384 for STATUS fields.
