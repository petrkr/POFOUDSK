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

Format the partitions on a PC. DOS on the Portfolio only knows FAT12,
16-bit sector counts and uses a single FAT:

```
mkfs.fat -F 12 -f 1 -S 512 -s <sectors per cluster> /dev/sdXn
```

Partition type must be `01` (FAT12). With `-s 1` a partition is limited
to ~2 MB (4084 clusters).

## ROM compatibility

| DIP ROM | driver | DIRFIX |
|---|---|---|
| 1.072 | tested | **tested, compatible** |
| 1.052, 1.056, 1.130, other | untested | not compatible - not installed (`unsupported ROM`) |

DIRFIX patches code at fixed ROM addresses and only installs on 1.072.
On other ROM versions use **one sector per cluster** (partitions up to
~2 MB): the DOS bug only occurs with more than one sector per cluster.
Larger clusters work for reading and writing files, but `MKDIR` or
filling a subdirectory can hang the machine and corrupt the file system.

If you have a different ROM version, please open an issue - DIRFIX can
be extended once someone needs it.

## Status

Developed and tested mainly in MAME with a real CH375 attached; the
PID card itself is tested for reading. Not yet verified on real
hardware: writing via the PID card, resume after sleep/wake.

## Layout

- `src/` - driver, CLI, partition/FAT12 code, DIRFIX
- `lib/pofoch375/` - CH375 chip library
- `lib/pofo/` - shared Portfolio library
