# POFOUDSK - Portfolio USB Disk

USB flash disk support for the **Atari Portfolio** via the **WCH CH375**
USB host controller on a PID card. DOS 2.x block device driver
(`POFOUDSK.SYS`) plus a status tool (`POFOUDSK.COM`).

Keywords: Atari Portfolio, DIP DOS 2.11, CH375, USB flash, mass storage.

## Features

- FAT12 partitions up to 65535 sectors (32 MB - 512 B), read/write.
- Up to 4 MBR primary partitions as drive letters, or a superfloppy.
- Hot-plug: insert/remove the flash disk at any time.
- Units without a usable partition show a status disk whose volume
  label says why (`NO CH375`, `NO USB`, `P2 EMPTY`, `P2 TYPE 0B`, ...).
- Built-in **DIRFIX**: fix for the DIP DOS bug that corrupts memory and
  hangs on `MKDIR` (or on creating a file in a full subdirectory) on
  drives with more than one sector per cluster - see below.

## Hardware

- Atari Portfolio
- PID card with a CH375 at I/O `807Dh` (command) / `807Eh` (data),
  PID id `75h`

## Build

Requires `bcc`, `as86`, `ld86` (dev86) and `nasm`.

```
make            # build/POFOUDSK.SYS, build/POFOUDSK.COM, build/DIRFIX.COM
```

## Installation

`CONFIG.SYS`:

```
DEVICE=A:\POFOUDSK.SYS /N:4
```

| switch | meaning |
|---|---|
| `/N:x` | number of drive letters, 1-4 (default 1); unit n = MBR partition n+1 |
| `/NODIRFIX` | do not install DIRFIX |

At boot the driver prints its version, whether the CH375 was found, and
`Units: n, DIRFIX on|off|unsupported ROM`. The controller may be absent
at boot - it is picked up when the card appears.

`POFOUDSK.COM` shows the driver state and the partitions; without the
driver loaded it probes the CH375 and the USB disk directly.

## Preparing a flash disk

Partition and format the flash disk on a PC:

```
mkfs.fat -F 12 -f 1 -S 512 -s <sectors per cluster> /dev/sdXn
```

- **FAT12** only, MBR partition type `01` (or a superfloppy without MBR).
- At most **65535 sectors** per partition (32 MB - 512 B) - DOS 2.x has a
  16-bit sector count.
- **One FAT** (`-f 1`): Portfolio DOS only ever updates the first FAT.
  With the `mkfs.fat` default of two FATs the second copy goes stale,
  `fsck.fat` reports "FATs differ" - and repairing from the second copy
  would lose data.
- Not supported: FAT16, FAT32, GPT, more than 4 partitions per disk.

### Supported formats

| sectors per cluster | max. partition size | with DIRFIX (ROM 1.072) | without DIRFIX (other ROM or `/NODIRFIX`) |
|---|---|---|---|
| 1 | ~2 MB | full | full |
| 2 | ~4 MB | full | read/write files; `MKDIR` and filling a subdirectory hang DOS |
| 4 | ~8 MB | full (tested) | same as above |
| 8 | ~16 MB | full | same as above |
| 16, 32 | 32 MB | full | same as above |

The limit per row is 4084 clusters (FAT12), capped at 65535 sectors.

## ROM compatibility

| DIP ROM | driver | DIRFIX |
|---|---|---|
| 1.072 | tested | tested |
| 1.052, 1.056, 1.130, other | expected to work, untested | not installed (`unsupported ROM`) |

The driver only uses the standard DOS 2.x driver interface and the
Portfolio BIOS PID call, so it should work on any ROM version. DIRFIX
patches DOS code at ROM 1.072 addresses and checks them before it
installs; on any other ROM it stays off and only one-sector clusters are
safe for directories (see the table above).

We only have ROM 1.072 available, so we cannot test the driver on the
other versions or port DIRFIX to them. If you own a Portfolio with a
different ROM version, please test it and open an issue with the result
(the boot line shows `DIRFIX on` or `unsupported ROM`) - we can then
agree on a ROM dump and add DIRFIX support for that version.

## Known issues

- **After sleep/wake** the driver may stop reacting to plugging and
  unplugging the flash disk, and disk access ends with a DOS error 11
  (read fault). Workaround: remove and reinsert the PID card, or reboot.
  Probable cause (unverified): the CH375 loses power or is reset during
  sleep and leaves USB host mode without raising an interrupt.

## Status

Developed and tested mainly in MAME with a real CH375 attached; the
PID card itself is tested for reading. Not yet verified on real
hardware: writing via the PID card, resume after sleep/wake.

## Layout

- `src/` - driver, CLI, partition/FAT12 code, DIRFIX
- `lib/pofoch375/` - CH375 chip library
- `lib/pofo/` - shared Portfolio library
