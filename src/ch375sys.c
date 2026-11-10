/* POFOUDSK.SYS - block device driver.
 *
 * Registers with DOS, claims a fixed number of drive letters (/N:x,
 * MAX_UNITS primary MBR partitions), and runs a guard() state machine
 * (NO_CONTROLLER / CONTROLLER_OK, sysmount.c) on every dispatch()
 * entry: cheap PID check (INT 61h) every time, expensive CH375 HELLO
 * (CHECK_EXIST) + reinit (SET_USB_MODE) only on the NO_CONTROLLER ->
 * PID-present transition, and a cheap interrupt-flag poll for USB
 * connect/disconnect events on every request while CONTROLLER_OK.
 * Never refuses to install even if the controller is absent right now
 * (soft-fail): INIT always reports success regardless of guard()
 * state.
 *
 * This unit holds the device header, the strategy/interrupt
 * trampolines with the driver's own stack, request dispatch and the
 * DOS command handlers. Units without a usable real partition get the
 * virtual status disk (sysvdisk.c). See sys.h for the module split.
 *
 * C+asm split: device header and strategy/interrupt trampolines are
 * hand-written asm here, far memory and port helpers in sysfar.c (BCC
 * has no `far` pointers); everything else is normal C. No stdio - uses
 * int 21h/09h ($-terminated string), not BCC libc's buffered I/O. The
 * _interrupt trampoline also switches to its own stack (int_stack[])
 * before calling any C code and restores the caller's SS:SP before
 * retf - BCC code assumes SS == DS, and the DOS caller's stack is
 * neither ours nor big.
 *
 * Build: see Makefile (sys target) - bcc -S, as86 -u -o, ld86 -0 -d,
 * this object first; links lib/pofoch375's ch375core.c/ch375disk.c,
 * src/part.c and lib/pofo/tinynum.c (driver-safe) - never
 * disk.c/ch375det.c/partpick.c (CLI only).
 */

#include "ch375.h"
#include "ch375disk.h"
#include "pofoudsk.h"
#include "tinynum.h"
#include "sys.h"
#include "pofoudsk_version.h"

#define ST_DONE    0x0100
#define ST_ERROR   0x8000
#define ERR_UNKNOWN    0x0003
#define ERR_WRITEPROT  0x0000
#define ERR_NOT_READY  0x0002
#define ERR_SECTOR_NOT_FOUND 0x0008
#define ERR_WRITE_FAULT 0x000A
#define ERR_READ_FAULT 0x000B

#define REQ_CMD        2
#define REQ_STATUS     3
#define REQ_UNITS      0x0D
#define REQ_BRK_OFF    0x0E
#define REQ_BRK_SEG    0x10
#define REQ_BPB_OFF    0x12
#define REQ_BPB_SEG    0x14

/* MEDIA_CHECK (cmd 1) return byte shares the request packet offset
 * used by INIT's break-address field (0x0E) - a union in the real DOS
 * request packet layout, not a coincidence. */
#define REQ_MEDIA_RET  0x0E

#define DOS_MEDIA_CHANGED     0xFF /* -1 as a byte: media changed/unknown */
#define DOS_MEDIA_NOT_CHANGED 0x01

#define CMD_INIT          0
#define CMD_MEDIA_CHECK   1
#define CMD_BUILD_BPB     2
#define CMD_IOCTL_IN      3
#define CMD_READ          4
#define CMD_WRITE         8
#define CMD_WRITE_VERIFY  9
#define CMD_IOCTL_OUT     0x0C

/* Unit count comes from CONFIG.SYS: DEVICE=POFOUDSK.SYS /N:x, x in
 * 1..MAX_UNITS (default 1). Unit n maps to MBR primary partition n+1,
 * so MAX_UNITS = 4 primary partitions (CH375DISK_PART_COUNT). */
#define DEFAULT_UNITS 1
#define REQ_UNIT      1    /* unit number byte in every request packet */
#define CMDLINE_MAX   128  /* scan limit for the CONFIG.SYS line */

/* Deepest path (MEDIA_CHECK -> resync -> mount_usb -> read_bpb ->
 * read_sector with its 64-byte chunk -> chunk handler) estimated at
 * ~250 bytes from the frame sizes - 512 keeps a 2x margin for that
 * plus hardware interrupts landing on this stack. Not measured on HW. */
#define INT_STACK_SIZE 512

/* Device header must be the literal first bytes of the file - top-
 * level #asm block before any C code/data. `export _start` satisfies ld86's entry point
 * requirement even though DOS never calls it. */
#asm
export _start
_device_header:
    dd 0xFFFFFFFF
    dw 0x6000
    dw _strategy
    dw _interrupt
    db 1
    db 0,0,0,0,0,0,0
_start:
    retf
#endasm

static unsigned int req_off;
static unsigned int req_seg;

/* NOTE: .bss is NOT zeroed for a .SYS (see sysmount.c ctrl_state) -
 * init_state() sets everything explicitly on INIT. */

/* Last byte written by an IOCTL output call (cmd 12/0x0C) - echoed
 * back as the last byte of the fixed-size v0 part of the IOCTL input
 * (cmd 3) reply, so a test tool can see its own byte round-trip
 * through the driver. */
unsigned char ioctl_last_cmd;

/* Unit count parsed at INIT, and the unit of the request being
 * handled (set in dispatch()). */
unsigned char unit_count;
unsigned char cur_unit;

/* /NODIRFIX from CONFIG.SYS - disables the DIRFIX MKDIR/directory-
 * extension fix (see dirfix.inc). */
static unsigned char nodirfix;


/* Per-unit "last content id handed to DOS via BUILD_BPB". Encodes (status, mount
 * generation) in one unsigned int so swapping one FAT12 flash for
 * another (same verdict, different content) is still seen as
 * "changed" - see current_content_id() in sysmount.c. */
static unsigned int presented_content_id[MAX_UNITS];

/* Own stack for the driver (BCC code assumes SS == DS) - the
 * _interrupt trampoline switches onto this before calling any BCC-
 * compiled C code, and back onto the caller's SS:SP before retf. A
 * plain static array, so it is ordinary .bss and automatically counted
 * before ld86's __end symbol (get_driver_end() below). */
static unsigned char int_stack[INT_STACK_SIZE];
static unsigned int saved_ss;
static unsigned int saved_sp;

/* REQ_FIRSTDRIVE (0x16) is a DOS 3.0+ field - DOS 2.x never fills it
 * (confirmed: DOS assigned E:, it read back as 0), so the assigned
 * letter is not printed. */

/* "POFOUDSK vM.m.p (build XXXXXXXX)\r\n$" - single-digit M/m/p (0-9,
 * matches version.h's current values) and 8 build-id hex digits
 * filled in at INIT time, same info/format as PFTC's startup title
 * (pftc.c: "PFTC v%u.%u.%u (%x%x%x%x)") but without printf/stdio.
 */
char msg_version[] = "POFOUDSK v0.0.0 (build 00000000)\r\n$";
#define MSG_VER_MAJOR_OFFSET 10
#define MSG_VER_MINOR_OFFSET 12
#define MSG_VER_PATCH_OFFSET 14
#define MSG_VER_BUILDID_OFFSET 23

static char msg_found[]      = "CH375 controller found, IC ver=  \r\n$";
#define MSG_VER_OFFSET 31 /* two hex-digit placeholder before \r */

static char msg_not_found[]  = "CH375 controller not present\r\n$";

static char msg_units[] = "Units: 0, DIRFIX $";
#define MSG_UNITS_OFFSET 7
static char msg_dirfix_on[] = "on\r\n$";
static char msg_dirfix_off[] = "off\r\n$";

static char msg_dirfix_bad[] = "unsupported ROM\r\n$";

static unsigned int bpb_array[MAX_UNITS];

/* End of resident driver memory - ld86's own __end symbol (end of
 * .bss). Must be read via asm, not `extern unsigned int __end` in C,
 * because BCC prefixes C-visible extern names with an extra leading
 * underscore. */
static unsigned int get_driver_end()
{
#asm
    mov ax,#__end
#endasm
}

/* --- request packet access (req_seg:req_off + field) --- */

static unsigned int req_get(field)
unsigned int field;
{
    return peek_w(req_seg, req_off + field);
}

static void req_set(field, val)
unsigned int field;
unsigned int val;
{
    poke_w(req_seg, req_off + field, val);
}

static void req_status(st)
unsigned int st;
{
    poke_w(req_seg, req_off + REQ_STATUS, st);
}

/* Explicit startup values for all runtime state - see the .bss note
 * above. Must run before the first guard(). */
static void init_state()
{
    unsigned char i;


    ioctl_last_cmd = 0;
    unit_count = DEFAULT_UNITS;
    cur_unit = 0;
    nodirfix = 0;
    init_mount_state();
    for (i = 0; i < MAX_UNITS; i++)
        presented_content_id[i] = CH375DISK_UNIT_NO_CONTROLLER << 8;
}

/* Matches an upper-case literal at seg:off, case-insensitively
 * (letters compared via & 0xDF). Used by parse_cmdline() below. */
static unsigned char match_word(seg, off, word)
unsigned int seg;
unsigned int off;
char *word;
{
    unsigned char c;

    while (*word != 0) {
        c = peek_b(seg, off++) & 0xDF;
        if (c != *word++)
            return 0;
    }
    return 1;
}

/* Scans the whole CONFIG.SYS line DOS passes in the INIT request
 * (far pointer at 0x12, text after "DEVICE=", terminated by CR/LF/
 * NUL) for /N:x and /NODIRFIX switches, both case-insensitive.
 * Whether DIP DOS 2.11 passes this pointer at all is unverified - if
 * not, the scan finds nothing and the defaults stay. */
static void parse_cmdline()
{
    unsigned int seg, off, end;
    unsigned char c, n;

    off = req_get(REQ_BPB_OFF);
    seg = req_get(REQ_BPB_SEG);

    for (end = off + CMDLINE_MAX; off != end; off++) {
        c = peek_b(seg, off);
        if (c == 0 || c == '\r' || c == '\n')
            return;
        if (c != '/')
            continue;
        if (match_word(seg, off + 1, "NODIRFIX")) {
            nodirfix = 1;
            continue;
        }
        c = peek_b(seg, off + 1) & 0xDF; /* upper case */
        if (c != 'N' || peek_b(seg, off + 2) != ':')
            continue;
        n = peek_b(seg, off + 3) - '0';
        if (n >= 1 && n <= MAX_UNITS)
            unit_count = n;
    }
}

static void print_version()
{
    char *p;

    msg_version[MSG_VER_MAJOR_OFFSET] = '0' + POFOUDSK_VERSION_MAJOR;
    msg_version[MSG_VER_MINOR_OFFSET] = '0' + POFOUDSK_VERSION_MINOR;
    msg_version[MSG_VER_PATCH_OFFSET] = '0' + POFOUDSK_VERSION_PATCH;

    p = msg_version + MSG_VER_BUILDID_OFFSET;
    p = put_hex2(p, POFOUDSK_BUILD_ID_HI >> 8);
    p = put_hex2(p, POFOUDSK_BUILD_ID_HI & 0xFF);
    p = put_hex2(p, POFOUDSK_BUILD_ID_LO >> 8);
    put_hex2(p, POFOUDSK_BUILD_ID_LO & 0xFF);

    print_dollar_string(msg_version);
}

/* Reports whatever state guard() already settled this request on -
 * does not repeat PID check or CHECK_EXIST itself (that would HELLO
 * on every INIT, not just on the NO_CONTROLLER -> PID-present
 * transition, contradicting the guard() contract). */
static void probe_controller()
{
    if (ctrl_state != STATE_CONTROLLER_OK) {
        print_dollar_string(msg_not_found);
        return;
    }

    put_hex2(msg_found + MSG_VER_OFFSET, ch375_get_ic_ver());
    print_dollar_string(msg_found);
}

static void cmd_init()
{
    unsigned char i;

    parse_cmdline(); /* before 0x12 is overwritten with bpb_array */

    poke_b(req_seg, req_off + REQ_UNITS, unit_count);
    req_set(REQ_BRK_OFF, get_driver_end());
    req_set(REQ_BRK_SEG, get_cs());
    /* bpb_array[] always points at the virtual BPB here - no media is
     * known yet at INIT, even if the controller is already OK. Real
     * per-unit BPBs are only ever handed out later, via BUILD_BPB. */
    for (i = 0; i < MAX_UNITS; i++)
        bpb_array[i] = (unsigned int)bpb;
    req_set(REQ_BPB_OFF, (unsigned int)bpb_array);
    req_set(REQ_BPB_SEG, get_cs());

    print_version();
    probe_controller(); /* reports state guard() already set at dispatch entry; soft-fail: never changes REQ_UNITS/status below */
    msg_units[MSG_UNITS_OFFSET] = '0' + unit_count;
    print_dollar_string(msg_units);
    if (nodirfix)
        print_dollar_string(msg_dirfix_off);
    else if (dirfix_install())
        print_dollar_string(msg_dirfix_on);
    else
        print_dollar_string(msg_dirfix_bad);

    /* DOS gets the virtual BPB right here, so the id it was just
     * handed starts out equal to whatever guard() settled on for this
     * dispatch - see current_content_id(). */
    for (i = 0; i < MAX_UNITS; i++)
        presented_content_id[i] = current_content_id(i);

    req_status(ST_DONE);
}

/* MEDIA_CHECK (cmd 1) - return byte at REQ_MEDIA_RET. Runs
 * test_connect_resync() first (while CONTROLLER_OK) to catch a
 * CH375 that silently lost its USB host state without ever raising a
 * CONNECT/DISCONNECT interrupt. Then "changed" iff cur_unit's current content id differs
 * from what was last handed to DOS through BUILD_BPB - this also
 * catches the controller appearing/disappearing, a USB connect/
 * disconnect, or one FAT12 flash disk being swapped for another,
 * since all of those change current_content_id()'s return value. */
static void cmd_media_check()
{
    if (ctrl_state == STATE_CONTROLLER_OK)
        test_connect_resync();

    poke_b(req_seg, req_off + REQ_MEDIA_RET,
           (presented_content_id[cur_unit] != current_content_id(cur_unit))
           ? DOS_MEDIA_CHANGED : DOS_MEDIA_NOT_CHANGED);
    req_status(ST_DONE);
}

/* BUILD_BPB (cmd 2) - DOS does not supply a destination pointer here;
 * it reads the BPB through the far pointer the driver itself writes
 * back at REQ_BPB_OFF/REQ_BPB_SEG (offset = BPB address, segment =
 * CS). There is always a valid disk to
 * describe (the virtual one if nothing else, else the real partition
 * BPB once it is CH375DISK_UNIT_REAL) - no not-ready error path. */
static void cmd_build_bpb()
{
    req_set(REQ_BPB_OFF, unit_is_real(cur_unit)
            ? (unsigned int) unit_bpb[cur_unit] : (unsigned int) bpb);
    req_set(REQ_BPB_SEG, get_cs());

    presented_content_id[cur_unit] = current_content_id(cur_unit);

    req_status(ST_DONE);
}

/* Far DOS transfer buffer pointer for the current sector's chunks,
 * consulted by real_chunk_handler()/real_chunk_supplier() below - set
 * once per sector by cmd_rw() right before calling
 * ch375disk_read_sector()/ch375disk_write_sector(). Not a function
 * argument: both handler/supplier signatures are fixed as
 * fn(offset, ...). Shared between READ and WRITE - the two never run
 * concurrently (one dispatch() at a time). */
static unsigned int buf_seg;
static unsigned int buf_off;

/* ch375disk_read_sector() chunk handler for a real partition READ -
 * copies each (normally 64-byte) USB chunk straight into the DOS far
 * buffer via far_copy(). Only wraps the destination segment at
 * whole-sector granularity (see cmd_rw()'s own off/seg stepping), not
 * mid-sector. */
static real_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    far_copy(buf_seg, buf_off + offset, chunk, (unsigned int) len);
}

/* ch375disk_write_sector() chunk supplier for a real partition WRITE -
 * writes the length byte (always 64 here) then copies the next 64
 * bytes straight from the DOS far buffer to the data port via
 * far_out(). */
static real_chunk_supplier(offset, len)
unsigned int offset;
unsigned int len;
{
    ch375_write_data((unsigned char) len);
    far_out(buf_seg, buf_off + offset, len);
}

static int rw_sector(is_write, lba_hi, lba_lo)
unsigned char is_write;
unsigned int lba_hi;
unsigned int lba_lo;
{
    if (is_write)
        return ch375disk_write_sector(lba_hi, lba_lo, real_chunk_supplier);
    return ch375disk_read_sector(lba_hi, lba_lo, real_chunk_handler);
}

/* READ (cmd 4) / WRITE (cmds 8/9) - transfer address dword at
 * REQ_BRK_OFF/SEG (0x0E/0x10), sector count at REQ_BPB_OFF (0x12),
 * start sector at REQ_BPB_SEG (0x14) - same request layout as
 * DOS 2.x READ/WRITE request packet. The transferred count is always written back
 * to 0x12; out-of-range stops the transfer with ERR_SECTOR_NOT_FOUND.
 *
 * Virtual disk (unit not CH375DISK_UNIT_REAL): READ generates sectors
 * on the fly (sysvdisk.c), WRITE is always ERR_WRITEPROT - there is no
 * real medium to write to.
 *
 * Real unit: physical LBA = partition start (32-bit hi/lo) + DOS
 * sector (16-bit), added with manual hi/lo carry (no long helpers),
 * one sector at a time. A disk error (not a timeout)
 * gets one REQUEST_SENSE + retry of that same sector before giving up; a lost
 * controller/USB (CH375_TIMEOUT, USB_INT_DISCONNECT) or a retry that
 * still fails all stop the transfer early. Error codes differ between
 * READ and WRITE only in the final giving-up case (ERR_WRITE_FAULT vs
 * ERR_READ_FAULT). */
static void cmd_rw(is_write)
unsigned char is_write;
{
    unsigned int off, seg, count, sector, transferred, limit;
    unsigned int lba_hi, lba_lo;
    unsigned char real;
    struct unit_state *u;
    int status;
    unsigned int stop_err; /* 0 = clean EOF, else DOS error code to report */

    real = unit_is_real(cur_unit);
    if (is_write && !real) {
        req_status(ST_DONE | ST_ERROR | ERR_WRITEPROT);
        return;
    }

    off = req_get(REQ_BRK_OFF);
    seg = req_get(REQ_BRK_SEG);
    count = req_get(REQ_BPB_OFF);
    sector = req_get(REQ_BPB_SEG);

    u = &units[cur_unit];
    /* size_hi is always 0 for a REAL unit, see ch375disk_fat12_check() */
    limit = real ? u->size_lo : VDISK_TOTAL_SECTORS;
    transferred = 0;
    stop_err = 0;

    for (; count != 0; count--) {
        if (sector >= limit) {
            stop_err = ERR_SECTOR_NOT_FOUND;
            break;
        }

        if (!real) {
            generate_sector(seg, off, sector);
        } else {
            lba_lo = u->start_lo + sector;
            lba_hi = u->start_hi;
            if (lba_lo < u->start_lo)
                lba_hi++; /* carry out of the low half */

            buf_seg = seg;
            buf_off = off;

            status = rw_sector(is_write, lba_hi, lba_lo);
            if (status != USB_INT_SUCCESS && status != CH375_TIMEOUT
                && status != USB_INT_DISCONNECT) {
                /* USB_INT_DISK_ERR or anything else - one request-sense
                 * + retry of this same sector before giving up. */
                ch375disk_request_sense();
                status = rw_sector(is_write, lba_hi, lba_lo);
                if (status != USB_INT_SUCCESS) {
                    drop_controller();
                    unmount_all();
                    stop_err = is_write ? ERR_WRITE_FAULT : ERR_READ_FAULT;
                    break;
                }
            }
            if (status != USB_INT_SUCCESS) {
                if (status == CH375_TIMEOUT)
                    drop_controller();
                unmount_all();
                stop_err = ERR_NOT_READY;
                break;
            }
        }

        sector++;
        off += 512;
        if (off < 512)
            seg += 0x1000;
        transferred++;
    }

    req_set(REQ_BPB_OFF, transferred);
    req_status(ST_DONE | (stop_err ? ST_ERROR | stop_err : 0));
}

/* IOCTL input (cmd 3, DOS INT 21h/44h/04h "read control data") -
 * transfer address dword at REQ_BRK_OFF/SEG (0x0E/0x10), byte count at
 * REQ_BPB_OFF (0x12). v1 reply layout:
 * the original 13-byte v0 signature/version/state block is unchanged
 * (offsets 0-12), followed by usb_state, mount_gen, unit_count,
 * vendor[8], product[16], capacity sectors hi/lo, then 4 partitions'
 * worth of (type, unit status, start hi/lo, size hi/lo), all 16-bit
 * values little-endian. Writes min(requested, total) bytes, so an old
 * 13-byte reader still works unchanged. */
#define IOCTL_V1_LEN (POFOUDSK_IOCTL_V0_LEN + 3 + 8 + 16 + 4 + MAX_UNITS * 10)

static unsigned char *put16(p, v)
unsigned char *p;
unsigned int v;
{
    *p++ = (unsigned char) v;
    *p++ = (unsigned char) (v >> 8);
    return p;
}

static unsigned char *put_bytes(p, s, n)
unsigned char *p;
unsigned char *s;
unsigned char n;
{
    while (n-- != 0)
        *p++ = *s++;
    return p;
}

static void cmd_ioctl_in()
{
    unsigned int count;
    unsigned char *p;
    struct unit_state *u;
    unsigned char reply[IOCTL_V1_LEN];

    p = put_bytes(reply, "POFOUDSK", POFOUDSK_IOCTL_SIG_LEN);
    *p++ = POFOUDSK_VERSION_MAJOR;
    *p++ = POFOUDSK_VERSION_MINOR;
    *p++ = POFOUDSK_VERSION_PATCH;
    *p++ = ctrl_state;
    *p++ = ioctl_last_cmd;

    *p++ = usb_state;
    *p++ = mount_gen;
    *p++ = unit_count;

    p = put_bytes(p, vendor, 8);
    p = put_bytes(p, product, 16);
    p = put16(p, cap_sectors_hi);
    p = put16(p, cap_sectors_lo);

    for (u = units; u < units + MAX_UNITS; u++) {
        *p++ = u->part_type;
        *p++ = u->status;
        p = put16(p, u->start_hi);
        p = put16(p, u->start_lo);
        p = put16(p, u->size_hi);
        p = put16(p, u->size_lo);
    }

    count = req_get(REQ_BPB_OFF);
    if (count > IOCTL_V1_LEN)
        count = IOCTL_V1_LEN;

    far_copy(req_get(REQ_BRK_SEG), req_get(REQ_BRK_OFF), reply, count);

    req_set(REQ_BPB_OFF, count);
    req_status(ST_DONE);
}

/* IOCTL output (cmd 12/0x0C, DOS INT 21h/44h/05h "write control
 * data") - same request packet fields as cmd_ioctl_in(). Stores the
 * first byte of the caller's buffer, so it shows up as the last byte
 * of the v0 part of the next IOCTL input reply (round-trip visibility
 * for testing). The count field is left as-is (= all bytes taken). */
static void cmd_ioctl_out()
{
    if (req_get(REQ_BPB_OFF) != 0)
        ioctl_last_cmd = peek_b(req_get(REQ_BRK_SEG), req_get(REQ_BRK_OFF));

    req_status(ST_DONE);
}

void dispatch()
{
    unsigned char cmd;

    cmd = peek_b(req_seg, req_off + REQ_CMD);

    if (cmd == CMD_INIT)
        init_state();
    else {
        cur_unit = peek_b(req_seg, req_off + REQ_UNIT);
        if (cur_unit >= MAX_UNITS)
            cur_unit = 0;
    }

    guard();

    switch (cmd) {
    case CMD_INIT:
        cmd_init();
        break;
    case CMD_MEDIA_CHECK:
        cmd_media_check();
        break;
    case CMD_BUILD_BPB:
        cmd_build_bpb();
        break;
    case CMD_IOCTL_IN:
        cmd_ioctl_in();
        break;
    case CMD_READ:
        cmd_rw(0);
        break;
    case CMD_WRITE:
    case CMD_WRITE_VERIFY:
        cmd_rw(1);
        break;
    case CMD_IOCTL_OUT:
        cmd_ioctl_out();
        break;
    default:
        req_status(ST_DONE | ST_ERROR | ERR_UNKNOWN);
        break;
    }
}

/* strategy/interrupt trampolines - top-level #asm labels, not C
 * functions, so BCC's prologue/epilogue never runs here (raw retf
 * required). Same pattern as lib/pofo/pofo_edit.c. */
#asm
_strategy:
    seg cs
    mov	_req_off,bx
    seg cs
    mov	_req_seg,es
    retf

_interrupt:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    cli
    mov ax,ss
    seg cs
    mov _saved_ss,ax
    mov ax,sp
    seg cs
    mov _saved_sp,ax

    mov ax,cs
    mov ss,ax
    mov sp,#_int_stack+INT_STACK_SIZE
    sti

    mov ax,cs
    mov ds,ax
    mov es,ax

    call _dispatch

    cli
    seg cs
    mov ax,_saved_ss
    mov ss,ax
    seg cs
    mov ax,_saved_sp
    mov sp,ax
    sti

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    retf
#endasm
