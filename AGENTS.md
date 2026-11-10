# AGENTS.md

Conventions for coding agents working on POFOUDSK (CH375 USB disk driver
for Atari Portfolio).

## Workflow

- Planning and review: Opus. Larger implementation tasks: delegated to
  Sonnet with a precise task description (files, goal, constraints,
  acceptance criteria). Opus reviews the diff afterwards.
- Never commit, pull, rebase, reset, add or checkout in git. The user does
  that.
- Edit only the files named in the task. No speculative searching, no new
  files unless the task says so.
- Report results faithfully: sizes, build output, what was and was not
  tested.

## Target

- CPU: Intel 80C88 @ 4.9152 MHz. 8086/8088 instructions only - no 186+
  (`push imm`, `shl reg,imm`, `ins/outs`, `pusha`, `enter/leave`, ...).
- OS: DIP DOS (DOS 2.x-like) in ROM, INT 61h BIOS extensions.
- CH375 on the PID card: command port `807Dh`, data port `807Eh`.

## Size budget (hard rule)

- Small code is the goal. Do not bloat: no stdio, no large libraries,
  no convenience abstractions that cost bytes.
- Driver limit: 16 kB (end goal: ROM-only driver on the PID board).
  RAM: ~20 kB free in total, shared with other programs.
- Measure `__end` (resident size) of the .SYS and .COM before and after
  every change and report both numbers.
- Prefer far_copy / tables over byte-by-byte code. No printf, no `long`
  in the CLI.
- Keep C code in small units reusable for the ROM build.

## Toolchain (BCC / dev86)

- No `inp()`/`outp()`; port I/O via `#asm ... #endasm`.
- A function whose body is only `#asm` gets NO `push bp; mov bp,sp`
  prologue. Add `push bp` / `mov bp,sp` ... `pop bp` manually; then
  `4[bp]` = 1st arg, `6[bp]` = 2nd arg.

## CH375 protocol facts (verified on HW)

- USB host start: mode 07h (bus reset) -> delay -> mode 06h -> DISK_INIT.
- Poll by reading the command port (bit 7 = INT#, 0 = pending), then
  issue GET_STATUS (22h) once.
- DISK_INIT may return 51h (CMD_RET_SUCCESS) as well as 14h - both are
  success.
- DISK_INIT leaves a 36-byte INQUIRY response in the read buffer. Drain
  RD_USB_DATA to empty before the next data-returning command.
- DISK_SIZE data is big-endian.
- Insert a short delay between `out` and the following `in` (TSC/TSD).

## DIRFIX

- Real 8088 resumes an interrupted multi-prefix string instruction at its
  last prefix. The INT 1 handler in `src/dirfix.inc` rewinds IP for this -
  never remove it. Any TF-based code must keep this workaround.

## Testing

- Real HW is the reference. MAME is secondary and unreliable for: REP +
  prefix interrupts, HPC-101 8255 port A (bidirectional on HW), serial.
- Tests without CH375 HW: Portfolio emulator (MAME `pofo`). Do not
  mention or use generic DOS emulators.
- Do not reset the emulator or write CONFIG.SYS without asking.

## Facts vs. guesses

- Never present an unverified HW/protocol/timing detail as fact. Mark it
  "unverified" and propose how to verify (datasheet `CH375.pdf`, HW
  measurement, test).

## Forks

- If you fork this repository, prefer contributing back via pull
  requests to the original repository.
- If you keep a separate fork, keep a visible reference to the original
  repository and its author (README, license headers, credits).

## Docs

- Code and docs in English.
