# pofoch375

WCH CH375 USB host controller library for 8086 / BCC (K&R C): chip
primitives and the chip's built-in mass-storage firmware commands.

Driver-safe: no libc, no `long`, no division - linkable into a DOS
device driver as well as a `.COM` program. Must not depend on
application code.

## Files

- `ch375.h` - command, mode and status codes; `CH375_TIMEOUT` (not a
  chip code - a bounded wait timed out).
- `ch375core.c` - command/data/interrupt-token I/O, bounded INT# wait,
  USB mode, host reset, `RD_USB_DATA`, busy-wait delay.
- `ch375disk.h`, `ch375disk.c` - `DISK_INIT` (with INQUIRY drain),
  `DISK_SIZE`, `REQUEST SENSE`, single-sector `READ`/`WRITE` streamed in
  64-byte chunks. 32-bit values are hi/lo `unsigned int` pairs.
- `ch375det.c` - blocking, bounded wait for a USB disk + `DISK_INIT`.
- `transport.h` - bus transport interface (`tr_*`), implementation
  selected at link time.
- `tr_pid.c` - PID card transport: I/O `807Dh` (command) / `807Eh`
  (data), the Atari Portfolio PID card wiring.

## Transport

All CH375 bus access goes through `transport.h` (`tr_present`,
`tr_cmd`, `tr_wdata`, `tr_rdata`, `tr_token`, `tr_wdata_far`). Today
only `tr_pid.c` exists. Another transport (e.g. serial/parallel) adds
its own `tr_*.c` and is selected via the Makefile's `TRANSPORT`
variable.
