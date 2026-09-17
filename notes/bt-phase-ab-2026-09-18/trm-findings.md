# TRM cross-check before phase experiment

Source: Rockchip_RK3576_TRM_Part1_V1.2_20240624.pdf, Chapter 39.

- 39.3.2.6: RE means invalid response transmission bit, command index or end bit.
  It does not prove a powered module transmitted a valid electrical response.
- Page 1379: CMD bit29 HOLD applies to outbound CMD/DATA, not a receive switch.
- Pages 1393-1394: CLKGEN_CON0 +0x130, CON1 +0x134. Both reset to 4
  (degree bits 2:1 = 2, 180 degrees). CON0 bit0 is init_state; CON1 bit0 reserved.
- 39.6.6, page 1411: assert init_state, program degree/delay, deassert init_state.
- Phase write mask 0x0bfe excludes reserved bit10 and bit0. CON0 initialization
  is controlled separately with the bit0 write mask.
- Page 1376/1377: PWREN is a card_power_en output. Setting it follows Linux
  but is not proof that board VBAT changed when that output is not connected.

Experiment inherits PWREN=1, HOLD=1 and reset-before-clock from prior tests.
Only on USRID=0x20230001, select driver phase 90 and sample phase 0, matching
SDK dw_mci_rk3288_set_ios legacy defaults. Log register values before/after.
Add 10 ms spacing to short diagnostic lines to reduce serial capture loss.
First build had a short Make.dep clock-skew warning; the final TRM-corrected
build was successful. No wall-clock duration is used as timing evidence.
