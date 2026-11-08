/* CH375.SYS - block device driver.
 *
 * Registers with DOS, claims a fixed number of drive letters (/N:x,
 * MAX_UNITS primary MBR partitions), and runs a guard() state machine
 * (NO_CONTROLLER / CONTROLLER_OK) on every dispatch() entry: cheap PID
 * check (INT 61h) every time, expensive CH375 HELLO (CHECK_EXIST) +
 * reinit (SET_USB_MODE) only on the NO_CONTROLLER -> PID-present
 * transition, and (new) a cheap interrupt-flag poll for USB connect/
 * disconnect events on every request while CONTROLLER_OK. Never
 * refuses to install even if the controller is absent right now
 * (soft-fail): INIT always reports success regardless of guard()
 * state. See ch375driver.md for the full design.
 *
 * Whenever a unit has no usable real partition mounted, the driver
 * presents a tiny synthetic read-only FAT12 "virtual disk" instead of
 * refusing I/O: its root directory holds INFO.TXT, explaining the
 * current state (controller/USB/partition status) for that unit.
 *
 * USB media mount/unmount is event-driven, not polled by blocking:
 * guard() reads the CH375 interrupt flag (one cheap IN) on every
 * request while CONTROLLER_OK, and only issues GET_STATUS (and acts
 * on CONNECT/DISCONNECT) when a pending interrupt is seen. MBR parsing
 * and per-partition FAT12 suitability checks are built on the shared
 * driver-safe ch375disk.c layer (also used by CH375.COM).
 *
 * Reuses the same C+asm split as vdiskc3.c: device header, strategy/
 * interrupt trampolines, peek/poke far-memory helpers and far_copy
 * (bulk far copy for READ) are hand-written asm (BCC has no `far`
 * pointers); everything else is normal C. No stdio - uses int 21h/09h
 * ($-terminated string) like vdiskc3.c, not BCC libc's buffered I/O.
 * The _interrupt trampoline also switches to its own stack
 * (int_stack[]) before calling any C code and restores the caller's
 * SS:SP before retf - see sysdriver.md "SS != DS".
 *
 * Build: see Makefile (sys target) - bcc -S, as86 -u -o, ld86 -0 -d,
 * links ch375core.c (CH375 primitives) and ch375disk.c (driver-safe
 * disk/MBR/BPB layer) - never disk.c (printf/unsigned-long, CLI only).
 */

#include "ch375.h"
#include "ch375disk.h"
#include "port.h"
#include "ch375sys_version.h"

#define ST_DONE    0x0100
#define ST_ERROR   0x8000
#define ERR_UNKNOWN    0x0003
#define ERR_WRITEPROT  0x0000
#define ERR_NOT_READY  0x0002
#define ERR_SECTOR_NOT_FOUND 0x0008
#define ERR_READ_FAULT 0x000B

#define REQ_CMD        2
#define REQ_STATUS     3
#define REQ_UNITS      0x0D
#define REQ_BRK_OFF    0x0E
#define REQ_BRK_SEG    0x10
#define REQ_BPB_OFF    0x12
#define REQ_BPB_SEG    0x14
#define REQ_FIRSTDRIVE 0x16

/* MEDIA_CHECK (cmd 1) return byte shares the request packet offset
 * used by INIT's break-address field (0x0E) - a union in the real DOS
 * request packet layout, not a coincidence. See ch375driver.md. */
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

/* Unit count comes from CONFIG.SYS: DEVICE=CH375.SYS /N:x, x in
 * 1..MAX_UNITS (default 1). Unit n maps to MBR primary partition n+1,
 * so MAX_UNITS = 4 primary partitions (CH375DISK_PART_COUNT). */
#define MAX_UNITS     4
#define DEFAULT_UNITS 1
#define REQ_UNIT      1    /* unit number byte in every request packet */
#define CMDLINE_MAX   128  /* scan limit for the CONFIG.SYS line */

/* PID identification byte for the CH375 card, set by a jumper on the
 * board - this value matches the current dev board jumper setting,
 * see ch375driver.md. */
#define PID_ID_CH375 0x75

/* guard() state machine, see ch375driver.md "Stavovy automat". */
#define STATE_NO_CONTROLLER 0
#define STATE_CONTROLLER_OK 1

#define INT_STACK_SIZE 1024

/* Virtual INFO disk geometry - see ch375driver.md "Virtualni INFO
 * disk". bytes/sector 512, sectors/cluster 1, reserved 1, FATs 2,
 * root entries 16 (= 1 sector), total sectors 8, media 0xF8, sectors/
 * FAT 1. Sector map: 0 boot sector, 1-2 FAT copies, 3 root dir, 4
 * INFO.TXT data (cluster 2), 5-7 unused/zero. */
#define VDISK_TOTAL_SECTORS 8
#define VDISK_SECTOR_SIZE   512

/* Delay after USB_INT_CONNECT before DISK_INIT - lets the freshly
 * powered device settle (USB spec allows it 100 ms). Same value
 * ch375disk.c's own (private) CONNECT_SETTLE_MS uses. */
#define CONNECT_SETTLE_MS 100

/* Device header must be the literal first bytes of the file - top-
 * level #asm block before any C code/data, same reasoning as
 * vdiskc3.c. `export _start` satisfies ld86's entry point
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

/* NOTE: .bss is NOT zeroed for a .SYS - DOS loads only the file
 * image, and .bss lies past its end, so every uninitialized static
 * below starts out as leftover memory garbage (observed: ctrl_state
 * read back as 0x5D). init_state() sets them explicitly on INIT,
 * before guard() runs for the first time. */
static unsigned char ctrl_state;

/* Last byte written by an IOCTL output call (cmd 12/0x0C) - echoed
 * back as the last byte of the fixed-size v0 part of the IOCTL input
 * (cmd 3) reply, so a test tool can see its own byte round-trip
 * through the driver. */
static unsigned char ioctl_last_cmd;

/* Unit count parsed at INIT, and the unit of the request being
 * handled (set in dispatch()). */
static unsigned char unit_count;
static unsigned char cur_unit;

/* Per-unit "last content id handed to DOS via BUILD_BPB" - see
 * ch375driver.md "MEDIA_CHECK a id obsahu". Encodes (status, mount
 * generation) in one unsigned int so swapping one FAT12 flash for
 * another (same verdict, different content) is still seen as
 * "changed" - see current_content_id() below. Sized for MAX_UNITS,
 * only the first unit_count entries are used. */
static unsigned int presented_content_id[MAX_UNITS];

/* --- USB/partition mount state - see ch375driver.md "USB event
 * handling" / "Per-unit stav". --- */

/* 1 once DISK_INIT+DISK_SIZE succeeded for the currently connected
 * device, 0 otherwise (no device, or disconnected). */
static unsigned char usb_state;

/* Bumped on every mount and unmount - part of the per-unit content id
 * (see presented_content_id above), so re-inserting a *different*
 * disk that happens to produce the same per-unit verdict is still
 * detected as a media change. */
static unsigned char mount_gen;

/* Vendor/product strings copied from ch375disk_vendor/product (see
 * ch375disk.c drain_inquiry()) at mount time - own copies, since
 * those globals get overwritten by the next ch375disk_init() call. */
static char vendor[9];
static char product[17];

static unsigned int cap_sectors_hi;
static unsigned int cap_sectors_lo;

struct unit_state {
    unsigned char status;   /* CH375DISK_UNIT_* (ch375disk.h) */
    unsigned char reason;   /* CH375DISK_UNIT_REASON_* - valid only if status == UNSUPPORTED */
    unsigned char part_type; /* MBR partition type byte, 0 for superfloppy/none */
    unsigned int start_hi;
    unsigned int start_lo;
    unsigned int size_hi;
    unsigned int size_lo;
    unsigned char mount_gen; /* snapshot of mount_gen at the time this unit's state was set */
};

static struct unit_state units[MAX_UNITS];

/* Real partition BPB, 13 bytes each (same layout as the virtual bpb[]
 * below: bytes 0x0B..0x17 of a FAT boot sector), filled by
 * store_unit_bpb() only for units whose status is
 * CH375DISK_UNIT_REAL. */
static unsigned char unit_bpb[MAX_UNITS][13];

/* Own stack for the driver (see sysdriver.md "SS != DS") - the
 * _interrupt trampoline switches onto this before calling any BCC-
 * compiled C code, and back onto the caller's SS:SP before retf. A
 * plain static array, so it is ordinary .bss and automatically counted
 * before ld86's __end symbol (get_driver_end() below) - no special
 * placement needed. 1024 bytes - raised from the original 512 to
 * cover the mount path (MBR/BPB structs + the 64-byte chunk buffer
 * several calls deep via ch375disk.c), untuned/unmeasured on HW. */
static unsigned char int_stack[INT_STACK_SIZE];
static unsigned int saved_ss;
static unsigned int saved_sp;

/* REQ_FIRSTDRIVE at INIT time does not reliably reflect the real
 * assigned letter on real hardware (confirmed: DOS assigned E:, this
 * field read back as 0 -> 'A' every time) - same unresolved
 * discrepancy already present in vdiskc3.c/vdisk.asm. Not worth
 * guessing at further for this skeleton; see ch375driver.md if this
 * needs revisiting. */
static char msg_installed[]  = "CH375.SYS installed\r\n$";

/* "CH375.SYS vM.m.p (build XXXXXXXX)\r\n$" - single-digit M/m/p (0-9,
 * matches version.h's current values) and 8 build-id hex digits
 * filled in at INIT time, same info/format as PFTC's startup title
 * (pftc.c: "PFTC v%u.%u.%u (%x%x%x%x)") but without printf/stdio.
 * Also reused (minus the trailing '$') as the version line appended
 * to the virtual disk's INFO.TXT - see build_info_text() below. */
static char msg_version[] = "CH375.SYS v0.0.0 (build 00000000)\r\n$";
#define MSG_VER_MAJOR_OFFSET 11
#define MSG_VER_MINOR_OFFSET 13
#define MSG_VER_PATCH_OFFSET 15
#define MSG_VER_BUILDID_OFFSET 24

static char msg_found[]      = "CH375 controller found, IC ver=  \r\n$";
#define MSG_VER_OFFSET 31 /* two hex-digit placeholder before \r */

static char msg_not_found[]  = "CH375 controller not present (ok for now)\r\n$";

/* INFO.TXT body buffer - built by build_info_text() into a static
 * buffer (never on the stack: this file's stack budget is tight and
 * shared across the mount path too). 160 bytes comfortably covers the
 * worst case (vendor+product+capacity+one unit verdict line+version
 * line), see build_info_text(). */
#define INFO_BUF_MAX 160
static char info_buf[INFO_BUF_MAX];

/* Virtual disk BPB - real geometry of a disk that actually exists
 * (the synthetic INFO disk), not a placeholder. DOS 2.x builds its
 * DPB from this immediately at INIT and divides by bytes/sector and
 * sectors/cluster, so neither may ever be zero (see sysdriver.md).
 * A unit whose mounted partition is CH375DISK_UNIT_REAL gets its own
 * real BPB (unit_bpb[] above) via BUILD_BPB instead - whose sector
 * size must never exceed this BPB's 512 (enforced by
 * ch375disk_bpb_sane()). */
static unsigned char bpb[] = {
    0x00, 0x02, /* bytes per sector = 512 */
    0x01,       /* sectors per cluster = 1 */
    0x01, 0x00, /* reserved sectors = 1 */
    0x02,       /* number of FATs = 2 */
    0x10, 0x00, /* root dir entries = 16 */
    0x08, 0x00, /* total sectors = 8 */
    0xF8,       /* media byte */
    0x01, 0x00  /* sectors per FAT = 1 */
};

static unsigned int bpb_array[MAX_UNITS];

/* --- far memory helpers (seg:off), asm bodies, cdecl args at 4/6[bp] --- */

static unsigned char peek_b(seg, off)
unsigned int seg;
unsigned int off;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    seg es
    mov al,[bx]
    xor ah,ah
    pop bx
    pop es
    pop bp
#endasm
}

static void poke_b(seg, off, val)
unsigned int seg;
unsigned int off;
unsigned char val;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    mov al,8[bp]
    seg es
    mov [bx],al
    pop bx
    pop es
    pop bp
#endasm
}

static void poke_w(seg, off, val)
unsigned int seg;
unsigned int off;
unsigned int val;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    mov ax,8[bp]
    seg es
    mov [bx],ax
    pop bx
    pop es
    pop bp
#endasm
}

static unsigned int peek_w(seg, off)
unsigned int seg;
unsigned int off;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    seg es
    mov ax,[bx]
    pop bx
    pop es
    pop bp
#endasm
}

/* Bulk far copy: DS:src (near, our own data/stack segment - always
 * true while the driver runs, since _interrupt sets DS=ES=SS=CS) to
 * dst_seg:dst_off, len bytes. `rep movsb` is an 8088 instruction (no
 * `rep insb`/186+ needed) - used by READ on a real unit to move each
 * 64-byte USB chunk straight into the DOS far transfer buffer instead
 * of a poke_b-per-byte loop. */
static void far_copy(dst_seg, dst_off, src, len)
unsigned int dst_seg;
unsigned int dst_off;
unsigned char *src;
unsigned int len;
{
#asm
    push bp
    mov bp,sp
    push es
    push si
    push di
    push cx
    mov ax,4[bp]
    mov es,ax
    mov di,6[bp]
    mov si,8[bp]
    mov cx,10[bp]
    cld
    rep
    movsb
    pop cx
    pop di
    pop si
    pop es
    pop bp
#endasm
}

static unsigned int get_cs()
{
#asm
    mov ax,cs
#endasm
}

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

static void print_dollar_string(s)
char *s;
{
#asm
    push bp
    mov bp,sp
    push dx
    mov dx,4[bp]
    mov ah,#0x09
    int 0x21
    pop dx
    pop bp
#endasm
}

/* PID presence check (Atari Portfolio BIOS, int 61h/ah=1Ah):
 * AL=0 -> no PID card present; AL!=0 -> present, AH = PID id byte.
 * Returns 1 only if a card with PID_ID_CH375 is present. */
static unsigned char pid_check()
{
#asm
    mov ah,#0x1A
    int 0x61
    or al,al
    jz pid_absent
    cmp ah,#PID_ID_CH375
    jne pid_absent
    mov al,#1
    jmp pid_done
pid_absent:
    xor al,al
pid_done:
#endasm
}

/* Drops to NO_CONTROLLER - the single place any CH375 command
 * failure/timeout must call, per ch375driver.md "Ztrata stavu CH375
 * pri trvale pritomnem PID". Callers that also lose the current USB
 * mount (guard(), mount/read paths) additionally call unmount_all();
 * current_content_id() reads ctrl_state live regardless, so the next
 * MEDIA_CHECK automatically compares against the new (NO_CONTROLLER)
 * id and reports "changed" on its own - see cmd_media_check() below. */
static void drop_controller()
{
    ctrl_state = STATE_NO_CONTROLLER;
}

/* Clears all per-unit mount state back to CH375DISK_UNIT_NO_USB and
 * bumps mount_gen - called on USB_INT_DISCONNECT, on any disk-op
 * CH375_TIMEOUT (per ch375driver.md "Ztrata stavu..."), and when the
 * controller itself is lost (guard()). current_content_id() ignores
 * units[]/mount_gen whenever ctrl_state != CONTROLLER_OK or
 * !usb_state (it reports a fixed id for those cases), so this mainly
 * matters for build_info_text()/IOCTL cosmetics and for the next real
 * mount to start from a clean state. */
static void unmount_all()
{
    unsigned char i;

    usb_state = 0;
    mount_gen++;
    vendor[0] = 0;
    product[0] = 0;
    cap_sectors_hi = 0;
    cap_sectors_lo = 0;
    for (i = 0; i < MAX_UNITS; i++) {
        units[i].status = CH375DISK_UNIT_NO_USB;
        units[i].part_type = 0;
        units[i].start_hi = 0;
        units[i].start_lo = 0;
        units[i].size_hi = 0;
        units[i].size_lo = 0;
        units[i].mount_gen = mount_gen;
    }
}

/* Packs a struct ch375disk_bpb (ch375disk.c's parsed form) into the
 * 13-byte wire layout BUILD_BPB hands to DOS - same byte layout as
 * the virtual bpb[] above. */
static void store_unit_bpb(n, b)
unsigned char n;
struct ch375disk_bpb *b;
{
    unit_bpb[n][0]  = (unsigned char) b->bytes_per_sector;
    unit_bpb[n][1]  = (unsigned char) (b->bytes_per_sector >> 8);
    unit_bpb[n][2]  = b->sectors_per_cluster;
    unit_bpb[n][3]  = (unsigned char) b->reserved_sectors;
    unit_bpb[n][4]  = (unsigned char) (b->reserved_sectors >> 8);
    unit_bpb[n][5]  = b->fat_count;
    unit_bpb[n][6]  = (unsigned char) b->root_entries;
    unit_bpb[n][7]  = (unsigned char) (b->root_entries >> 8);
    unit_bpb[n][8]  = (unsigned char) b->total_sectors;
    unit_bpb[n][9]  = (unsigned char) (b->total_sectors >> 8);
    unit_bpb[n][10] = b->media_descriptor;
    unit_bpb[n][11] = (unsigned char) b->sectors_per_fat;
    unit_bpb[n][12] = (unsigned char) (b->sectors_per_fat >> 8);
}

/* Evaluates MBR primary partition entry n (0-based, so DOS unit n =
 * "Pn+1") against part[n] and sets units[n] accordingly - see
 * ch375driver.md "Per-unit stav". Returns CH375_TIMEOUT if the BPB
 * read itself times out (caller must drop_controller()+unmount_all),
 * else 0. */
static int evaluate_partition(n, mbr)
unsigned char n;
struct ch375disk_mbr *mbr;
{
    unsigned char type, boot_flag;
    unsigned int start_hi, start_lo, sectors_hi, sectors_lo;
    struct ch375disk_bpb pbpb;
    unsigned char boot_jump;
    int status, verdict;

    type = mbr->part[n].type;
    boot_flag = mbr->part[n].boot_flag;
    start_hi = mbr->part[n].lba_start_hi;
    start_lo = mbr->part[n].lba_start_lo;
    sectors_hi = mbr->part[n].sectors_hi;
    sectors_lo = mbr->part[n].sectors_lo;

    units[n].part_type = type;
    units[n].start_hi = start_hi;
    units[n].start_lo = start_lo;
    units[n].size_hi = sectors_hi;
    units[n].size_lo = sectors_lo;
    units[n].mount_gen = mount_gen;

    if (type == 0) {
        units[n].status = CH375DISK_UNIT_EMPTY;
        return 0;
    }
    if (type == 0xEE) {
        units[n].status = CH375DISK_UNIT_GPT;
        return 0;
    }
    if (boot_flag != 0x00 && boot_flag != 0x80) {
        units[n].status = CH375DISK_UNIT_UNSUPPORTED;
        units[n].reason = CH375DISK_UNIT_REASON_BAD_TYPE;
        return 0;
    }

    status = ch375disk_read_bpb(start_hi, start_lo, &pbpb, &boot_jump);
    if (status == CH375_TIMEOUT)
        return CH375_TIMEOUT;
    if (status != USB_INT_SUCCESS) {
        units[n].status = CH375DISK_UNIT_UNSUPPORTED;
        units[n].reason = CH375DISK_UNIT_REASON_BAD_BPB;
        return 0;
    }

    verdict = ch375disk_fat12_check(type, sectors_hi, sectors_lo, &pbpb);
    if (verdict == CH375DISK_FAT12_OK) {
        units[n].status = CH375DISK_UNIT_REAL;
        store_unit_bpb(n, &pbpb);
    } else {
        units[n].status = CH375DISK_UNIT_UNSUPPORTED;
        if (verdict == CH375DISK_FAT12_TOO_BIG)
            units[n].reason = CH375DISK_UNIT_REASON_TOO_BIG;
        else if (verdict == CH375DISK_FAT12_BAD_BPB)
            units[n].reason = CH375DISK_UNIT_REASON_BAD_BPB;
        else
            units[n].reason = CH375DISK_UNIT_REASON_BAD_TYPE;
    }
    return 0;
}

/* Superfloppy case (mbr->is_superfloppy): unit 0 is the whole-disk
 * FAT12 volume (start 0, size = BPB total sectors), units 1-3 are
 * EMPTY - see ch375driver.md. */
static void mount_superfloppy(mbr)
struct ch375disk_mbr *mbr;
{
    unsigned char n;
    int verdict;

    units[0].part_type = 0;
    units[0].start_hi = 0;
    units[0].start_lo = 0;
    units[0].size_hi = 0;
    units[0].size_lo = mbr->bpb0.total_sectors;
    units[0].mount_gen = mount_gen;

    verdict = ch375disk_fat12_check(0, 0, mbr->bpb0.total_sectors, &mbr->bpb0);
    if (verdict == CH375DISK_FAT12_OK) {
        units[0].status = CH375DISK_UNIT_REAL;
        store_unit_bpb(0, &mbr->bpb0);
    } else {
        units[0].status = CH375DISK_UNIT_UNSUPPORTED;
        units[0].reason = (verdict == CH375DISK_FAT12_TOO_BIG)
            ? CH375DISK_UNIT_REASON_TOO_BIG : CH375DISK_UNIT_REASON_BAD_BPB;
    }

    for (n = 1; n < MAX_UNITS; n++) {
        units[n].status = CH375DISK_UNIT_EMPTY;
        units[n].part_type = 0;
        units[n].start_hi = 0;
        units[n].start_lo = 0;
        units[n].size_hi = 0;
        units[n].size_lo = 0;
        units[n].mount_gen = mount_gen;
    }
}

/* Runs the full mount sequence after USB_INT_CONNECT: settle delay,
 * DISK_INIT, DISK_SIZE, MBR read, then per-partition evaluation (or
 * the superfloppy special case). Any CH375_TIMEOUT along the way
 * drops the controller entirely (per ch375driver.md) - the caller
 * (check_usb_event()) already holds the guard() context. */
static void mount_usb()
{
    struct ch375disk_mbr mbr;
    int status;
    unsigned char n, i;

    ch375_delay_ms(CONNECT_SETTLE_MS);

    status = ch375disk_init();
    if (status == CH375_TIMEOUT) {
        drop_controller();
        unmount_all();
        return;
    }
    if (status != USB_INT_SUCCESS) {
        unmount_all(); /* stay NO_USB - not a successful mount */
        return;
    }

    status = ch375disk_size();
    if (status == CH375_TIMEOUT) {
        drop_controller();
        unmount_all();
        return;
    }
    if (status != USB_INT_SUCCESS) {
        unmount_all();
        return;
    }

    cap_sectors_hi = ch375disk_sector_count_hi;
    cap_sectors_lo = ch375disk_sector_count_lo;
    for (i = 0; i < 8; i++)
        vendor[i] = ch375disk_vendor[i];
    vendor[8] = 0;
    for (i = 0; i < 16; i++)
        product[i] = ch375disk_product[i];
    product[16] = 0;

    status = ch375disk_read_mbr(0, 0, &mbr);
    if (status == CH375_TIMEOUT) {
        drop_controller();
        unmount_all();
        return;
    }

    usb_state = 1;
    mount_gen++;

    if (status != USB_INT_SUCCESS) {
        /* Disk responded to INIT/SIZE but MBR read failed - leave all
         * units marked unusable (bad BPB is the closest existing
         * reason code). */
        for (n = 0; n < MAX_UNITS; n++) {
            units[n].status = CH375DISK_UNIT_UNSUPPORTED;
            units[n].reason = CH375DISK_UNIT_REASON_BAD_BPB;
            units[n].mount_gen = mount_gen;
        }
        return;
    }

    if (mbr.is_superfloppy) {
        mount_superfloppy(&mbr);
        return;
    }

    if (!mbr.has_mbr_sig) {
        /* No 0x55AA signature and not a superfloppy boot sector -
         * nothing usable to report per partition. */
        for (n = 0; n < MAX_UNITS; n++) {
            units[n].status = CH375DISK_UNIT_UNSUPPORTED;
            units[n].reason = CH375DISK_UNIT_REASON_BAD_BPB;
            units[n].mount_gen = mount_gen;
        }
        return;
    }

    for (n = 0; n < MAX_UNITS; n++) {
        if (evaluate_partition(n, &mbr) == CH375_TIMEOUT) {
            drop_controller();
            unmount_all();
            return;
        }
    }
}

/* Event-driven USB handling - called once per request while
 * CONTROLLER_OK (see guard()). ch375_read_cmd_token() is a plain IN on
 * the command port (no command issued), so this costs nothing when no
 * interrupt is pending; GET_STATUS (and any further CH375 traffic) is
 * only issued once an interrupt is actually seen. */
static void check_usb_event()
{
    unsigned char token;
    int status;

    token = ch375_read_cmd_token();
    if (token & 0x80)
        return; /* no interrupt pending */

    status = ch375_get_status();
    if (status == USB_INT_CONNECT)
        mount_usb();
    else if (status == USB_INT_DISCONNECT)
        unmount_all();
    /* any other code: ignore, same as before */
}

/* Runs on every dispatch() entry - see ch375driver.md "Stavovy
 * automat". pid_check() (cheap, BIOS-only) runs every time; the
 * expensive CH375 HELLO (CHECK_EXIST) + reinit (SET_USB_MODE 07h then
 * 06h) run only on the NO_CONTROLLER -> PID-present transition.
 * check_usb_event() (one cheap IN, expensive GET_STATUS/mount only if
 * an interrupt is actually pending) runs on every request while
 * CONTROLLER_OK. */
static void guard()
{
    unsigned char pid_present;

    pid_present = pid_check();

    if (ctrl_state == STATE_CONTROLLER_OK) {
        if (!pid_present) {
            drop_controller();
            unmount_all();
        } else {
            check_usb_event();
        }
        return;
    }

    /* STATE_NO_CONTROLLER */
    if (!pid_present)
        return;

    if (ch375_check_exist(0x5A) != (unsigned char)~0x5A)
        return; /* PID present but not our CH375 - stay NO_CONTROLLER */

    if (!ch375_host_reset())
        return;
    ctrl_state = STATE_CONTROLLER_OK;
    /* USB media/BPB are not re-scanned on this same request - a flash
     * already plugged in will raise its own CONNECT interrupt, picked
     * up by check_usb_event() on the *next* request (see
     * ch375driver.md note on this). */
}

/* Returns 1 only if unit is both reachable (controller present, USB
 * mounted) AND its own verdict is CH375DISK_UNIT_REAL - the single
 * place BUILD_BPB/READ consult before handing out real-partition data,
 * so a stale units[] entry left over from a dropped controller never
 * gets used after the fact (ctrl_state/usb_state are checked live). */
static unsigned char unit_is_real(unit)
unsigned char unit;
{
    if (ctrl_state != STATE_CONTROLLER_OK)
        return 0;
    if (!usb_state)
        return 0;
    return units[unit].status == CH375DISK_UNIT_REAL;
}

static char hex_digit(v)
unsigned char v;
{
    if (v < 10)
        return '0' + v;
    return 'A' + (v - 10);
}

/* Writes 4 hex digits of a 16-bit value into msg_version starting at
 * offset, most significant nibble first. */
static void put_hex16(offset, val)
int offset;
unsigned int val;
{
    msg_version[offset + 0] = hex_digit((unsigned char)((val >> 12) & 0x0F));
    msg_version[offset + 1] = hex_digit((unsigned char)((val >> 8) & 0x0F));
    msg_version[offset + 2] = hex_digit((unsigned char)((val >> 4) & 0x0F));
    msg_version[offset + 3] = hex_digit((unsigned char)(val & 0x0F));
}

static void print_version()
{
    msg_version[MSG_VER_MAJOR_OFFSET] = '0' + CH375SYS_VERSION_MAJOR;
    msg_version[MSG_VER_MINOR_OFFSET] = '0' + CH375SYS_VERSION_MINOR;
    msg_version[MSG_VER_PATCH_OFFSET] = '0' + CH375SYS_VERSION_PATCH;

    put_hex16(MSG_VER_BUILDID_OFFSET, CH375SYS_BUILD_ID_HI);
    put_hex16(MSG_VER_BUILDID_OFFSET + 4, CH375SYS_BUILD_ID_LO);

    print_dollar_string(msg_version);
}

/* Reports whatever state guard() already settled this request on -
 * does not repeat PID check or CHECK_EXIST itself (that would HELLO
 * on every INIT, not just on the NO_CONTROLLER -> PID-present
 * transition, contradicting the guard() contract above). */
static void probe_controller()
{
    unsigned char ver;

    if (ctrl_state != STATE_CONTROLLER_OK) {
        print_dollar_string(msg_not_found);
        return;
    }

    ver = ch375_get_ic_ver();
    msg_found[MSG_VER_OFFSET]     = hex_digit((ver >> 4) & 0x0F);
    msg_found[MSG_VER_OFFSET + 1] = hex_digit(ver & 0x0F);
    print_dollar_string(msg_found);
}

/* Current content id for one unit - (status, mount generation) packed
 * into one unsigned int, see presented_content_id above. While the
 * controller is absent or no USB disk is mounted, every unit reports
 * the same fixed id (generation 0) regardless of stale units[]
 * data - current_content_id() never trusts units[] unless both
 * ctrl_state and usb_state agree a real mount is live. */
static unsigned int current_content_id(unit)
unsigned char unit;
{
    unsigned char status, gen;

    if (ctrl_state != STATE_CONTROLLER_OK) {
        status = CH375DISK_UNIT_NO_CONTROLLER;
        gen = 0;
    } else if (!usb_state) {
        status = CH375DISK_UNIT_NO_USB;
        gen = 0;
    } else {
        status = units[unit].status;
        gen = units[unit].mount_gen;
    }
    return ((unsigned int) status << 8) | gen;
}

/* Decimal (unsigned, <=5 digits) into info_buf at pos, no leading
 * zeros except "0" itself. BCC/as86 -u links no runtime division
 * helper (imodu/idiv_u undefined at link time, confirmed building
 * this driver) - repeated subtraction of each power of ten instead of
 * val%10/val/10, same spirit as the hi/lo 32-bit arithmetic rule. */
static unsigned int write_udec(pos, val)
unsigned int pos;
unsigned int val;
{
    static unsigned int pow10[5] = { 10000, 1000, 100, 10, 1 };
    unsigned char i, started, d;

    started = 0;
    for (i = 0; i < 5; i++) {
        d = 0;
        while (val >= pow10[i]) {
            val -= pow10[i];
            d++;
        }
        if (d != 0 || started || i == 4) {
            info_buf[pos++] = '0' + d;
            started = 1;
        }
    }
    return pos;
}

/* Appends the "Pn: ..." partition verdict line for unit n - see
 * ch375driver.md "Virtualni INFO disk" for the full text table. Only
 * called for a unit that is NOT CH375DISK_UNIT_REAL (a REAL unit never
 * shows the virtual disk/INFO.TXT at all). */
static unsigned int append_unit_line(pos, n)
unsigned int pos;
unsigned char n;
{
    unsigned char st, reason, t;
    char *msg;

    st = units[n].status;

    info_buf[pos++] = 'P';
    info_buf[pos++] = '1' + n;
    info_buf[pos++] = ':';
    info_buf[pos++] = ' ';

    if (st == CH375DISK_UNIT_GPT) {
        msg = "GPT disk - not supported, use MBR";
        while (*msg)
            info_buf[pos++] = *msg++;
    } else if (st == CH375DISK_UNIT_EMPTY) {
        msg = "empty";
        while (*msg)
            info_buf[pos++] = *msg++;
    } else if (st == CH375DISK_UNIT_UNSUPPORTED) {
        reason = units[n].reason;
        if (reason == CH375DISK_UNIT_REASON_TOO_BIG) {
            msg = "FAT12 too big";
            while (*msg)
                info_buf[pos++] = *msg++;
        } else if (reason == CH375DISK_UNIT_REASON_BAD_BPB) {
            msg = "bad BPB";
            while (*msg)
                info_buf[pos++] = *msg++;
        } else {
            t = units[n].part_type;
            msg = "type ";
            while (*msg)
                info_buf[pos++] = *msg++;
            info_buf[pos++] = hex_digit((t >> 4) & 0x0F);
            info_buf[pos++] = hex_digit(t & 0x0F);
            msg = " - unsupported, need FAT12 <=32MB";
            while (*msg)
                info_buf[pos++] = *msg++;
        }
    } else {
        msg = "unknown";
        while (*msg)
            info_buf[pos++] = *msg++;
    }

    info_buf[pos++] = '\r';
    info_buf[pos++] = '\n';
    return pos;
}

/* Appends msg_version's body (minus its trailing '$', only needed by
 * print_dollar_string()) into info_buf. */
static unsigned int append_version_line(pos)
unsigned int pos;
{
    unsigned int i;

    for (i = 0; i < sizeof(msg_version) - 2; i++)
        info_buf[pos + i] = msg_version[i];
    return pos + (sizeof(msg_version) - 2);
}

/* Builds the current INFO.TXT body for cur_unit into the static
 * info_buf and returns its length. State-dependent (CRLF lines):
 *   - no controller / no USB: single state line,
 *   - USB mounted: vendor+product line, capacity line, then this
 *     unit's own "Pn: ..." verdict line (skipped if the unit is
 *     actually REAL - no virtual disk is ever shown for it),
 *   - always: the version/build line last.
 * See ch375driver.md "Virtualni INFO disk" for the full text table.
 */
static unsigned int build_info_text()
{
    unsigned int pos, mb;
    unsigned char i;
    char *msg;

    pos = 0;

    if (ctrl_state != STATE_CONTROLLER_OK) {
        msg = "CH375 controller not present\r\n";
        while (*msg)
            info_buf[pos++] = *msg++;
    } else if (!usb_state) {
        msg = "No USB disk connected\r\n";
        while (*msg)
            info_buf[pos++] = *msg++;
    } else {
        for (i = 0; i < 8 && vendor[i] != 0; i++)
            info_buf[pos++] = vendor[i];
        info_buf[pos++] = ' ';
        for (i = 0; i < 16 && product[i] != 0; i++)
            info_buf[pos++] = product[i];
        info_buf[pos++] = '\r';
        info_buf[pos++] = '\n';

        /* MB = sectors * 512 / 1048576 = sectors / 2048 - as a 32-bit
         * right shift by 11 across the hi/lo pair without any 32-bit
         * long helper: lo>>11 keeps lo's top 5 bits, hi<<5 feeds in
         * the bits that crossed from hi. Truncates for disks far
         * beyond any real USB flash drive's size - cosmetic only. */
        mb = (cap_sectors_hi << 5) | (cap_sectors_lo >> 11);
        msg = "Disk ";
        while (*msg)
            info_buf[pos++] = *msg++;
        pos = write_udec(pos, mb);
        msg = "MB\r\n";
        while (*msg)
            info_buf[pos++] = *msg++;

        if (!unit_is_real(cur_unit))
            pos = append_unit_line(pos, cur_unit);
    }

    pos = append_version_line(pos);
    return pos;
}

/* Generates one 512-byte sector of the virtual INFO disk directly
 * into the far DOS transfer buffer at seg:off - no 512-byte image or
 * sector buffer kept resident (memory is tight), just zero-fill then
 * poke the few non-zero bytes. See ch375driver.md "Virtualni INFO
 * disk" for the full sector map. */
static void generate_sector(seg, off, sector)
unsigned int seg;
unsigned int off;
unsigned int sector;
{
    unsigned int i, len;

    for (i = 0; i < VDISK_SECTOR_SIZE; i++)
        poke_b(seg, off + i, 0);

    if (sector == 0) {
        /* Boot sector: BPB copy at 0x0B, signature at 510. */
        poke_w(seg, off + 0x0B, 512);
        poke_b(seg, off + 0x0D, 1);
        poke_w(seg, off + 0x0E, 1);
        poke_b(seg, off + 0x10, 2);
        poke_w(seg, off + 0x11, 16);
        poke_w(seg, off + 0x13, VDISK_TOTAL_SECTORS);
        poke_b(seg, off + 0x15, 0xF8);
        poke_w(seg, off + 0x16, 1);
        poke_w(seg, off + 510, 0xAA55);
    } else if (sector == 1 || sector == 2) {
        /* FAT copy: cluster 2 = EOF (F8 FF FF FF 0F), rest zero. */
        poke_b(seg, off + 0, 0xF8);
        poke_b(seg, off + 1, 0xFF);
        poke_b(seg, off + 2, 0xFF);
        poke_b(seg, off + 3, 0xFF);
        poke_b(seg, off + 4, 0x0F);
    } else if (sector == 3) {
        /* Root directory: volume label + INFO.TXT entry. */
        poke_b(seg, off + 0, 'N');  poke_b(seg, off + 1, 'O');
        poke_b(seg, off + 2, ' ');  poke_b(seg, off + 3, 'U');
        poke_b(seg, off + 4, 'S');  poke_b(seg, off + 5, 'B');
        poke_b(seg, off + 6, ' ');  poke_b(seg, off + 7, 'P');
        poke_b(seg, off + 8, '1' + cur_unit);  poke_b(seg, off + 9, ' ');
        poke_b(seg, off + 10, ' ');
        poke_b(seg, off + 11, 0x08); /* ATTR_VOLUME_LABEL */

        poke_b(seg, off + 32 + 0, 'I');  poke_b(seg, off + 32 + 1, 'N');
        poke_b(seg, off + 32 + 2, 'F');  poke_b(seg, off + 32 + 3, 'O');
        poke_b(seg, off + 32 + 4, ' ');  poke_b(seg, off + 32 + 5, ' ');
        poke_b(seg, off + 32 + 6, ' ');  poke_b(seg, off + 32 + 7, ' ');
        poke_b(seg, off + 32 + 8, 'T');  poke_b(seg, off + 32 + 9, 'X');
        poke_b(seg, off + 32 + 10, 'T');
        poke_b(seg, off + 32 + 11, 0x01); /* ATTR_READONLY */
        poke_w(seg, off + 32 + 26, 2);    /* start cluster */
        len = build_info_text();
        poke_w(seg, off + 32 + 28, len);
        poke_w(seg, off + 32 + 30, 0);    /* size hi (fits in 16 bits) */
    } else if (sector == 4) {
        len = build_info_text();
        for (i = 0; i < len; i++)
            poke_b(seg, off + i, info_buf[i]);
    }
    /* sectors 5-7: left zero. */
}

/* Explicit startup values for all runtime state - see the .bss note
 * at ctrl_state. Must run before the first guard(). */
static void init_state()
{
    unsigned char i;

    ctrl_state = STATE_NO_CONTROLLER;
    ioctl_last_cmd = 0;
    unit_count = DEFAULT_UNITS;
    cur_unit = 0;
    usb_state = 0;
    mount_gen = 0;
    vendor[0] = 0;
    product[0] = 0;
    cap_sectors_hi = 0;
    cap_sectors_lo = 0;
    for (i = 0; i < MAX_UNITS; i++) {
        units[i].status = CH375DISK_UNIT_NO_CONTROLLER;
        units[i].reason = 0;
        units[i].part_type = 0;
        units[i].start_hi = 0;
        units[i].start_lo = 0;
        units[i].size_hi = 0;
        units[i].size_lo = 0;
        units[i].mount_gen = 0;
        presented_content_id[i] = CH375DISK_UNIT_NO_CONTROLLER << 8;
    }
}

/* Parses /N:x from the CONFIG.SYS line DOS passes in the INIT
 * request (far pointer at 0x12, text after "DEVICE=", terminated by
 * CR/LF/NUL). Whether DIP DOS 2.11 passes this pointer at all is
 * unverified - if not, the scan finds no /N: and the default stays. */
static void parse_cmdline()
{
    unsigned int seg, off, i;
    unsigned char c, n;

    off = peek_w(req_seg, req_off + REQ_BPB_OFF);
    seg = peek_w(req_seg, req_off + REQ_BPB_SEG);

    for (i = 0; i < CMDLINE_MAX; i++) {
        c = peek_b(seg, off + i);
        if (c == 0 || c == '\r' || c == '\n')
            return;
        if (c != '/')
            continue;
        c = peek_b(seg, off + i + 1);
        if ((c != 'N' && c != 'n') || peek_b(seg, off + i + 2) != ':')
            continue;
        n = peek_b(seg, off + i + 3) - '0';
        if (n >= 1 && n <= MAX_UNITS)
            unit_count = n;
        return;
    }
}

static char msg_units[] = "Units: 0\r\n$";
#define MSG_UNITS_OFFSET 7

static void cmd_init()
{
    unsigned char i;

    parse_cmdline(); /* before 0x12 is overwritten with bpb_array */

    poke_b(req_seg, req_off + REQ_UNITS, unit_count);
    poke_w(req_seg, req_off + REQ_BRK_OFF, get_driver_end());
    poke_w(req_seg, req_off + REQ_BRK_SEG, get_cs());
    /* bpb_array[] always points at the virtual BPB here - no media is
     * known yet at INIT, even if the controller is already OK. Real
     * per-unit BPBs are only ever handed out later, via BUILD_BPB. */
    for (i = 0; i < MAX_UNITS; i++)
        bpb_array[i] = (unsigned int)bpb;
    poke_w(req_seg, req_off + REQ_BPB_OFF, (unsigned int)bpb_array);
    poke_w(req_seg, req_off + REQ_BPB_SEG, get_cs());

    print_dollar_string(msg_installed);
    print_version();
    msg_units[MSG_UNITS_OFFSET] = '0' + unit_count;
    print_dollar_string(msg_units);

    probe_controller(); /* reports state guard() already set at dispatch entry; soft-fail: never changes REQ_UNITS/status below */

    /* DOS gets the virtual BPB right here, so the id it was just
     * handed starts out equal to whatever guard() settled on for this
     * dispatch - see current_content_id() above. */
    for (i = 0; i < MAX_UNITS; i++)
        presented_content_id[i] = current_content_id(i);

    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

static void cmd_unknown()
{
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | ERR_UNKNOWN);
}

/* MEDIA_CHECK (cmd 1) - return byte at REQ_MEDIA_RET. "Changed" iff
 * cur_unit's current content id differs from what was last handed to
 * DOS through BUILD_BPB - this also catches the controller appearing/
 * disappearing, a USB connect/disconnect, or one FAT12 flash disk
 * being swapped for another, since all of those change
 * current_content_id()'s return value (see mount_gen above). */
static void cmd_media_check()
{
    unsigned char ret;

    ret = (presented_content_id[cur_unit] != current_content_id(cur_unit))
          ? DOS_MEDIA_CHANGED : DOS_MEDIA_NOT_CHANGED;
    poke_b(req_seg, req_off + REQ_MEDIA_RET, ret);
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* BUILD_BPB (cmd 2) - DOS does not supply a destination pointer here;
 * it reads the BPB through the far pointer the driver itself writes
 * back at REQ_BPB_OFF/REQ_BPB_SEG (offset = BPB address, segment =
 * CS), same as vdiskc3.c:287-293. There is always a valid disk to
 * describe (the virtual one if nothing else, else the real partition
 * BPB once it is CH375DISK_UNIT_REAL) - no not-ready error path. */
static void cmd_build_bpb()
{
    unsigned int off;

    off = unit_is_real(cur_unit) ? (unsigned int) unit_bpb[cur_unit] : (unsigned int) bpb;

    poke_w(req_seg, req_off + REQ_BPB_OFF, off);
    poke_w(req_seg, req_off + REQ_BPB_SEG, get_cs());

    presented_content_id[cur_unit] = current_content_id(cur_unit);

    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* READ (cmd 4) on the virtual disk - transfer address dword at
 * REQ_BRK_OFF/SEG (0x0E/0x10), sector count at REQ_BPB_OFF (0x12),
 * start sector at REQ_BPB_SEG (0x14) - same request layout as
 * vdiskc3.c's cmd_read(). Generates virtual-disk sectors on the fly;
 * an out-of-range sector stops the transfer and reports
 * ERR_SECTOR_NOT_FOUND, with the sector count field updated to how
 * many sectors were actually transferred before that point. */
static void cmd_read_virtual()
{
    unsigned int off, seg, count, sector, transferred;
    unsigned char out_of_range;

    off = peek_w(req_seg, req_off + REQ_BRK_OFF);
    seg = peek_w(req_seg, req_off + REQ_BRK_SEG);
    count = peek_w(req_seg, req_off + REQ_BPB_OFF);
    sector = peek_w(req_seg, req_off + REQ_BPB_SEG);

    transferred = 0;
    out_of_range = 0;

    while (count != 0) {
        if (sector >= VDISK_TOTAL_SECTORS) {
            out_of_range = 1;
            break;
        }

        generate_sector(seg, off, sector);

        sector++;
        off += 512;
        if (off < 512)
            seg += 0x1000;
        count--;
        transferred++;
    }

    poke_w(req_seg, req_off + REQ_BPB_OFF, transferred);

    if (out_of_range)
        poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | ERR_SECTOR_NOT_FOUND);
    else
        poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* Destination far pointer for the current sector's chunks, consulted
 * by real_chunk_handler() below - set once per sector by
 * cmd_read_real() right before calling ch375disk_read_sector(). Not a
 * function argument: ch375disk_read_sector()'s handler signature is
 * fixed as handler(offset, chunk, len). */
static unsigned int dst_seg;
static unsigned int dst_off;

/* ch375disk_read_sector() chunk handler for a real partition READ -
 * copies each (normally 64-byte) USB chunk straight into the DOS far
 * buffer via far_copy(), no intermediate poke_b-per-byte loop. Mirrors
 * cmd_read_virtual()/generate_sector(): only wraps the destination
 * segment at whole-sector granularity (see cmd_read_real()'s own
 * off/seg stepping), not mid-sector - same simplification already
 * accepted there. */
static real_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    far_copy(dst_seg, dst_off + offset, chunk, (unsigned int) len);
}

/* READ (cmd 4) on a CH375DISK_UNIT_REAL unit - same request packet
 * fields as cmd_read_virtual(). Physical LBA = partition start (32-bit
 * hi/lo) + DOS sector (16-bit, from the request packet), added with
 * manual hi/lo carry (see ch375driver.md "32bitova LBA"). One sector
 * at a time via ch375disk_read_sector(); out-of-range, a disk error or
 * a lost controller/USB all stop the transfer early with the sector
 * count field reporting how far it got. */
static void cmd_read_real()
{
    unsigned int off, seg, count, dos_sector, transferred;
    unsigned int lba_hi, lba_lo;
    unsigned int size_lo;
    int status;
    unsigned char stop_err; /* 0 = clean EOF, else DOS error code to report */

    off = peek_w(req_seg, req_off + REQ_BRK_OFF);
    seg = peek_w(req_seg, req_off + REQ_BRK_SEG);
    count = peek_w(req_seg, req_off + REQ_BPB_OFF);
    dos_sector = peek_w(req_seg, req_off + REQ_BPB_SEG);

    size_lo = units[cur_unit].size_lo; /* size_hi is always 0 for a REAL unit, see ch375disk_fat12_check() */

    transferred = 0;
    stop_err = 0;

    while (count != 0) {
        if (dos_sector >= size_lo) {
            stop_err = ERR_SECTOR_NOT_FOUND;
            break;
        }

        lba_lo = units[cur_unit].start_lo + dos_sector;
        lba_hi = units[cur_unit].start_hi;
        if (lba_lo < units[cur_unit].start_lo)
            lba_hi++; /* carry out of the low half */

        dst_seg = seg;
        dst_off = off;

        status = ch375disk_read_sector(lba_hi, lba_lo, real_chunk_handler);

        if (status == CH375_TIMEOUT) {
            drop_controller();
            unmount_all();
            stop_err = ERR_NOT_READY;
            break;
        }
        if (status == USB_INT_DISCONNECT) {
            unmount_all();
            stop_err = ERR_NOT_READY;
            break;
        }
        if (status != USB_INT_SUCCESS) {
            /* USB_INT_DISK_ERR or anything else - no retry (left out
             * deliberately, see final report); report a read fault. */
            stop_err = ERR_READ_FAULT;
            break;
        }

        dos_sector++;
        off += 512;
        if (off < 512)
            seg += 0x1000;
        count--;
        transferred++;
    }

    poke_w(req_seg, req_off + REQ_BPB_OFF, transferred);

    if (stop_err)
        poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | stop_err);
    else
        poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

static void cmd_read()
{
    if (unit_is_real(cur_unit))
        cmd_read_real();
    else
        cmd_read_virtual();
}

/* WRITE (cmds 8/9) - always write-protected for every unit (including
 * a REAL one - real write support is a later phase), same as
 * vdiskc3.c's cmd_write_protected(). */
static void cmd_write_protected()
{
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | ERR_WRITEPROT);
}

/* IOCTL input (cmd 3, DOS INT 21h/44h/04h "read control data") -
 * transfer address dword at REQ_BRK_OFF/SEG (0x0E/0x10), byte count at
 * REQ_BPB_OFF (0x12). v1 reply layout (see ch375driver.md "IOCTL v1"):
 * the original 13-byte v0 signature/version/state block is unchanged
 * (offsets 0-12), followed by usb_state, mount_gen, unit_count,
 * vendor[8], product[16], capacity sectors hi/lo, then 4 partitions'
 * worth of (type, unit status, start hi/lo, size hi/lo). Writes
 * min(requested, total) bytes, so an old 13-byte reader still works
 * unchanged. */
#define IOCTL_V0_LEN 13
#define IOCTL_V1_LEN (IOCTL_V0_LEN + 3 + 8 + 16 + 4 + MAX_UNITS * 10)

static void cmd_ioctl_in()
{
    unsigned int off, seg, count, i, base;
    unsigned char n;
    unsigned char reply[IOCTL_V1_LEN];

    reply[0] = 'C'; reply[1] = 'H'; reply[2] = '3'; reply[3] = '7';
    reply[4] = '5'; reply[5] = 'S'; reply[6] = 'Y'; reply[7] = 'S';
    reply[8] = CH375SYS_VERSION_MAJOR;
    reply[9] = CH375SYS_VERSION_MINOR;
    reply[10] = CH375SYS_VERSION_PATCH;
    reply[11] = ctrl_state;
    reply[12] = ioctl_last_cmd;

    reply[13] = usb_state;
    reply[14] = mount_gen;
    reply[15] = unit_count;

    for (i = 0; i < 8; i++)
        reply[16 + i] = vendor[i];
    for (i = 0; i < 16; i++)
        reply[24 + i] = product[i];

    reply[40] = (unsigned char) cap_sectors_hi;
    reply[41] = (unsigned char) (cap_sectors_hi >> 8);
    reply[42] = (unsigned char) cap_sectors_lo;
    reply[43] = (unsigned char) (cap_sectors_lo >> 8);

    for (n = 0; n < MAX_UNITS; n++) {
        base = 44 + n * 10;
        reply[base + 0] = units[n].part_type;
        reply[base + 1] = units[n].status;
        reply[base + 2] = (unsigned char) units[n].start_hi;
        reply[base + 3] = (unsigned char) (units[n].start_hi >> 8);
        reply[base + 4] = (unsigned char) units[n].start_lo;
        reply[base + 5] = (unsigned char) (units[n].start_lo >> 8);
        reply[base + 6] = (unsigned char) units[n].size_hi;
        reply[base + 7] = (unsigned char) (units[n].size_hi >> 8);
        reply[base + 8] = (unsigned char) units[n].size_lo;
        reply[base + 9] = (unsigned char) (units[n].size_lo >> 8);
    }

    off = peek_w(req_seg, req_off + REQ_BRK_OFF);
    seg = peek_w(req_seg, req_off + REQ_BRK_SEG);
    count = peek_w(req_seg, req_off + REQ_BPB_OFF);

    if (count > IOCTL_V1_LEN)
        count = IOCTL_V1_LEN;

    for (i = 0; i < count; i++)
        poke_b(seg, off + i, reply[i]);

    poke_w(req_seg, req_off + REQ_BPB_OFF, count);
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* IOCTL output (cmd 12/0x0C, DOS INT 21h/44h/05h "write control
 * data") - same request packet fields as cmd_ioctl_in(). Stores the
 * first byte of the caller's buffer, so it shows up as the last byte
 * of the v0 part of the next IOCTL input reply (round-trip visibility
 * for testing). */
static void cmd_ioctl_out()
{
    unsigned int off, seg, count;

    off = peek_w(req_seg, req_off + REQ_BRK_OFF);
    seg = peek_w(req_seg, req_off + REQ_BRK_SEG);
    count = peek_w(req_seg, req_off + REQ_BPB_OFF);

    if (count > 0)
        ioctl_last_cmd = peek_b(seg, off);

    poke_w(req_seg, req_off + REQ_BPB_OFF, count);
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
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
        cmd_read();
        break;
    case CMD_WRITE:
    case CMD_WRITE_VERIFY:
        cmd_write_protected();
        break;
    case CMD_IOCTL_OUT:
        cmd_ioctl_out();
        break;
    default:
        cmd_unknown();
        break;
    }
}

/* strategy/interrupt trampolines - top-level #asm labels, not C
 * functions, so BCC's prologue/epilogue never runs here (raw retf
 * required). Same pattern as vdiskc3.c / src/pofo/pofo_edit.c:104-134. */
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
