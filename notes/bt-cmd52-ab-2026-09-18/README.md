# Pre-enumeration CMD52 reset comparison

Reference: Linux mmc_rescan_try_freq -> sdio_reset. Read CCCR address 6,
then write its reset bit (fallback value 8 on failed read), before CMD0/CMD5.
Both CMD52 operations use response CRC and STOP. Polling has finite bounds.

Board result: CMD52 read and reset both report RINTSTS=2 RESP0=0.
CMD0 RINTSTS=4. CMD5 remains RINTSTS=2 RESP0=0. Probe returns -5 to NSH.
The reset command has NOT been verified as accepted by the card.
This change does not resolve enumeration and does not prove card activity.

FIT SHA256 98e7ddf4f1c68974b48c37d0d0a4940861ff54a4b4a3cc05da820157652d77d4.
Build, diff check and readback passed. Raw serial output contains missing bytes.
Previous region: /tmp/k7-amp-fit/flash-20260917T182555Z/lba24576-8192-before.bin.
