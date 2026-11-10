# lib/pofoch375

CH375 USB host controller primitives, plus its built-in mass-storage
firmware commands, for 8086/BCC (K&R C). Driver-safe: no libc, no
`long`, no division/modulo - everything here must stay linkable into
a DOS device driver (`POFOUDSK.SYS`) as well as a `.COM` program.

Intended to become a standalone git submodule - it must not depend on
application code (no `src/` includes, no DOS-specific request-packet
handling, no MBR/BPB/FAT parsing - that is not chip-specific and lives
in `src/part.h`/`src/part.c` instead).

## Files

- `ch375.h` - mode/command/status/interrupt code constants
  (`MODE_*`, `CMD_RET_*`, `USB_INT_*`), plus `CH375_TIMEOUT` (not a
  chip code - returned when a bounded wait times out).
- `ch375core.c` - CH375 primitives: command/data/token I/O on
  `807Dh`/`807Eh`, `ch375_wait_int()`, `ch375_set_usb_mode()`,
  `ch375_delay_ms()`, `ch375_host_reset()`, `ch375_rd_usb_data_q()`.
- `ch375disk.h` - the chip's built-in mass-storage firmware API:
  `ch375disk_init`/`_size`/`_read_sector`/`_write_sector`/
  `_request_sense`/`_wait_connect`/`_detect`, vendor/product strings,
  sector count/size globals, `ch375disk_size_tries`.
- `ch375disk.c` - `DISK_INIT` (+ INQUIRY drain), `DISK_SIZE`,
  `REQUEST SENSE`, streaming single-sector `READ`/`WRITE`.
- `ch375det.c` - blocking, bounded `ch375disk_wait_connect()`/
  `ch375disk_detect()` - CLI-only in practice (`POFOUDSK.COM`); the
  driver (`POFOUDSK.SYS`) never blocks waiting for a device, it reacts
  to CONNECT/DISCONNECT interrupts instead (see `src/sysmount.c`).
- `port.c`/`port.h` - 8086 `in`/`out` for BCC (no built-in inp/outp).

## Transport

`port.c`'s `port_in()`/`port_out()` talk to I/O `807Dh` (command) /
`807Eh` (data) - the Atari Portfolio PID card wiring for the CH375.
A different transport (COM/LPT adapter) would need its own `port.c`;
everything above `ch375core.c` is transport-agnostic.

## Rule

Nothing in this directory may `#include` anything from `src/` or
depend on DOS/BCC application code - only the chip and its firmware
commands.
