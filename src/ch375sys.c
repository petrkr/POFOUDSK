/* CH375.SYS - block device driver.
 *
 * Registers with DOS, claims a single drive letter, and runs a
 * guard() state machine (NO_CONTROLLER / CONTROLLER_OK) on every
 * dispatch() entry: cheap PID check (INT 61h) every time, expensive
 * CH375 HELLO (CHECK_EXIST) + reinit (SET_USB_MODE) only on the
 * NO_CONTROLLER -> PID-present transition. Never refuses to install
 * even if the controller is absent right now (soft-fail): INIT always
 * reports success regardless of guard() state. See ch375driver.md for
 * the full design (guard state machine, static unit count, virtual
 * INFO disk, MEDIA_CHECK/BUILD_BPB semantics).
 *
 * Whenever no usable USB media is present, the driver presents a tiny
 * synthetic read-only FAT12 "virtual disk" instead of refusing I/O:
 * its root directory holds INFO.TXT, explaining the current state.
 * The INIT BPB IS this virtual disk's real geometry - never all-zero,
 * since DOS 2.x builds a DPB from the INIT BPB immediately and
 * divides by bytes/sector and sectors/cluster (see sysdriver.md).
 *
 * MBR parsing, real media mount and /N: CONFIG.SYS parsing are NOT
 * implemented yet (later phase, see ch375driver.md "Stop here" in the
 * owner's spec) - once real USB media is mounted, BUILD_BPB will hand
 * DOS the real partition BPB instead of the virtual one.
 *
 * Reuses the same C+asm split as vdiskc3.c: device header, strategy/
 * interrupt trampolines and peek/poke far-memory helpers are
 * hand-written asm (BCC has no `far` pointers); everything else is
 * normal C. No stdio - uses int 21h/09h ($-terminated string) like
 * vdiskc3.c, not BCC libc's buffered I/O (unavailable/unwanted in a
 * device driver context). The _interrupt trampoline also switches to
 * its own stack (int_stack[]) before calling any C code and restores
 * the caller's SS:SP before retf - see sysdriver.md "SS != DS".
 *
 * Build: see Makefile (sys target) - bcc -S, as86 -u -o,
 * ld86 -0 -d, same pipeline as vdiskc3.c/sysdriver.md.
 */

#include "ch375.h"
#include "port.h"
#include "ch375sys_version.h"

#define ST_DONE    0x0100
#define ST_ERROR   0x8000
#define ERR_UNKNOWN    0x0003
#define ERR_WRITEPROT  0x0000
#define ERR_SECTOR_NOT_FOUND 0x0008

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
#define CMD_READ          4
#define CMD_WRITE         8
#define CMD_WRITE_VERIFY  9

#define UNIT_COUNT 1 /* fixed for now - /N: CONFIG.SYS parsing not yet done */

/* PID identification byte for the CH375 card, set by a jumper on the
 * board - this value matches the current dev board jumper setting,
 * see ch375driver.md. */
#define PID_ID_CH375 0x75

/* guard() state machine, see ch375driver.md "Stavovy automat". */
#define STATE_NO_CONTROLLER 0
#define STATE_CONTROLLER_OK 1

/* "Presented content id" - which virtual-disk state was last handed
 * to DOS via BUILD_BPB. Only two states exist today (no real media
 * mount yet); a future id (e.g. CONTENT_REAL_MEDIA) is added once
 * real USB media mounting lands - see ch375driver.md TODO. */
#define CONTENT_NO_CONTROLLER 0
#define CONTENT_NO_USB        1

#define INT_STACK_SIZE 512

/* Virtual INFO disk geometry - see ch375driver.md "Virtualni INFO
 * disk". bytes/sector 512, sectors/cluster 1, reserved 1, FATs 2,
 * root entries 16 (= 1 sector), total sectors 8, media 0xF8,
 * sectors/FAT 1. Sector map: 0 boot sector, 1-2 FAT copies, 3 root
 * dir, 4 INFO.TXT data (cluster 2), 5-7 unused/zero. */
#define VDISK_TOTAL_SECTORS 8
#define VDISK_SECTOR_SIZE   512

/* Device header must be the literal first bytes of the file - top-
 * level #asm block before any C code/data, same reasoning as
 * vdiskc3.c. `export _start` satisfies ld86's entry point
 * requirement even though DOS never calls it. */
#asm
export _start
_device_header:
    dd 0xFFFFFFFF
    dw 0x2000
    dw _strategy
    dw _interrupt
    db 1
    db 0,0,0,0,0,0,0
_start:
    retf
#endasm

static unsigned int req_off;
static unsigned int req_seg;

static unsigned char ctrl_state; /* STATE_NO_CONTROLLER at startup (.bss) */

/* Per-unit "last content id handed to DOS via BUILD_BPB" - see
 * ch375driver.md "MEDIA_CHECK a id obsahu". Starts at 0 (.bss), which
 * equals CONTENT_NO_CONTROLLER, matching ctrl_state's own startup
 * value, so the very first MEDIA_CHECK before any BUILD_BPB correctly
 * reports "not changed" relative to whatever INIT already presented.
 * UNIT_COUNT is fixed at 1 for now, but kept as an array for when /N:
 * support lands. */
static unsigned char presented_content_id[UNIT_COUNT];

/* Own stack for the driver (see sysdriver.md "SS != DS") - the
 * _interrupt trampoline switches onto this before calling any BCC-
 * compiled C code, and back onto the caller's SS:SP before retf. A
 * plain static array, so it is ordinary .bss and automatically counted
 * before ld86's __end symbol (get_driver_end() below) - no special
 * placement needed. 512 bytes is a reasonable first guess, untuned. */
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
 * to the virtual disk's INFO.TXT - see write_info_text() below. */
static char msg_version[] = "CH375.SYS v0.0.0 (build 00000000)\r\n$";
#define MSG_VER_MAJOR_OFFSET 11
#define MSG_VER_MINOR_OFFSET 13
#define MSG_VER_PATCH_OFFSET 15
#define MSG_VER_BUILDID_OFFSET 24

static char msg_found[]      = "CH375 controller found, IC ver=  \r\n$";
#define MSG_VER_OFFSET 31 /* two hex-digit placeholder before \r */

static char msg_not_found[]  = "CH375 controller not present (ok for now)\r\n$";

/* INFO.TXT body text, state-dependent (no trailing '$' - these are
 * embedded into the virtual disk's data sector, not printed via
 * int 21h/09h). TODO: a future "Unsupported partition - format FAT12
 * <=32MB" message is added here once real MBR/partition parsing
 * lands (see ch375driver.md). */
static char info_text_no_ctrl[] = "CH375 controller not present\r\n";
static char info_text_no_usb[]  = "No USB disk connected\r\n";

/* Virtual disk BPB - real geometry of a disk that actually exists
 * (the synthetic INFO disk), not a placeholder. DOS 2.x builds its
 * DPB from this immediately at INIT and divides by bytes/sector and
 * sectors/cluster, so neither may ever be zero (see sysdriver.md).
 * Once real USB media mounting lands (later phase), BUILD_BPB will
 * hand out the real partition BPB instead - whose sector size must
 * never exceed this BPB's 512. */
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

static unsigned int bpb_array[UNIT_COUNT];

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
 * pri trvale pritomnem PID". Only guard() calls this today; READ/real
 * BUILD_BPB media access (not implemented yet, later phase) must call
 * it too once they issue real CH375 commands that can fail/timeout.
 * Nothing else to do here any more: current_content_id() reads
 * ctrl_state live, so the next MEDIA_CHECK automatically compares
 * against the new (NO_CONTROLLER) id and reports "changed" on its
 * own - see cmd_media_check() below. */
static void drop_controller()
{
    ctrl_state = STATE_NO_CONTROLLER;
}

/* Runs on every dispatch() entry - see ch375driver.md "Stavovy
 * automat". pid_check() (cheap, BIOS-only) runs every time; the
 * expensive CH375 HELLO (CHECK_EXIST) + reinit (SET_USB_MODE 07h then
 * 06h) run only on the NO_CONTROLLER -> PID-present transition. */
static void guard()
{
    unsigned char pid_present;

    pid_present = pid_check();

    if (ctrl_state == STATE_CONTROLLER_OK) {
        if (!pid_present)
            drop_controller();
        return;
    }

    /* STATE_NO_CONTROLLER */
    if (!pid_present)
        return;

    if (ch375_check_exist(0x5A) != (unsigned char)~0x5A)
        return; /* PID present but not our CH375 - stay NO_CONTROLLER */

    if (ch375_set_usb_mode(MODE_HOST_RESET) != CMD_RET_SUCCESS)
        return;
    if (ch375_set_usb_mode(MODE_HOST_SOF) != CMD_RET_SUCCESS)
        return;
    ctrl_state = STATE_CONTROLLER_OK;
    /* Still on the virtual disk - real media scan not implemented yet
     * (later phase). The virtual disk's content id (CONTENT_NO_USB)
     * differs from CONTENT_NO_CONTROLLER, so MEDIA_CHECK still
     * reports "changed" on this transition. */
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

/* Current virtual-disk content id, derived live from ctrl_state - see
 * "Presented content id" above. No separate flag to keep in sync: a
 * controller appearing/disappearing changes ctrl_state, which changes
 * this return value immediately, which is exactly what
 * cmd_media_check() compares against presented_content_id[]. */
static unsigned char current_content_id()
{
    if (ctrl_state == STATE_CONTROLLER_OK)
        return CONTENT_NO_USB;
    return CONTENT_NO_CONTROLLER;
}

/* Combined length of the current INFO.TXT body (state message plus
 * the version/build line, minus msg_version's trailing '$' which is
 * only needed by print_dollar_string()). sizeof() on a string literal
 * includes the terminating NUL, hence the "-1"/"-2" adjustments. */
static unsigned int current_info_len()
{
    unsigned int len;

    if (ctrl_state == STATE_CONTROLLER_OK)
        len = sizeof(info_text_no_usb) - 1;
    else
        len = sizeof(info_text_no_ctrl) - 1;

    len = len + (sizeof(msg_version) - 2);
    return len;
}

/* Writes the current INFO.TXT body into the far buffer at seg:off -
 * state message first, then the version/build line (msg_version,
 * dropping its trailing '$'). msg_version must already have been
 * formatted by print_version() (done once at INIT). */
static void write_info_text(seg, off)
unsigned int seg;
unsigned int off;
{
    char *msg;
    unsigned int msg_len;
    unsigned int i, pos;

    if (ctrl_state == STATE_CONTROLLER_OK) {
        msg = info_text_no_usb;
        msg_len = sizeof(info_text_no_usb) - 1;
    } else {
        msg = info_text_no_ctrl;
        msg_len = sizeof(info_text_no_ctrl) - 1;
    }

    for (i = 0; i < msg_len; i++)
        poke_b(seg, off + i, msg[i]);

    pos = msg_len;
    for (i = 0; i < sizeof(msg_version) - 2; i++)
        poke_b(seg, off + pos + i, msg_version[i]);
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
    unsigned int i;

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
        poke_b(seg, off + 6, ' ');  poke_b(seg, off + 7, ' ');
        poke_b(seg, off + 8, ' ');  poke_b(seg, off + 9, ' ');
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
        poke_w(seg, off + 32 + 28, current_info_len());
        poke_w(seg, off + 32 + 30, 0);    /* size hi (fits in 16 bits) */
    } else if (sector == 4) {
        write_info_text(seg, off);
    }
    /* sectors 5-7: left zero. */
}

static void cmd_init()
{
    poke_b(req_seg, req_off + REQ_UNITS, UNIT_COUNT);
    poke_w(req_seg, req_off + REQ_BRK_OFF, get_driver_end());
    poke_w(req_seg, req_off + REQ_BRK_SEG, get_cs());
    bpb_array[0] = (unsigned int)bpb;
    poke_w(req_seg, req_off + REQ_BPB_OFF, (unsigned int)bpb_array);
    poke_w(req_seg, req_off + REQ_BPB_SEG, get_cs());

    print_dollar_string(msg_installed);
    print_version();

    probe_controller(); /* reports state guard() already set at dispatch entry; soft-fail: never changes REQ_UNITS/status below */

    /* DOS gets the virtual BPB right here, so the id it was just
     * handed starts out equal to whatever guard() settled on for this
     * dispatch - see "Presented content id" above. */
    presented_content_id[0] = current_content_id();

    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

static void cmd_unknown()
{
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | ERR_UNKNOWN);
}

/* MEDIA_CHECK (cmd 1) - return byte at REQ_MEDIA_RET. "Changed" iff
 * the virtual disk's current content id differs from what was last
 * handed to DOS through BUILD_BPB - this also catches the controller
 * appearing/disappearing while staying virtual, since that changes
 * current_content_id()'s return value too. */
static void cmd_media_check()
{
    unsigned char ret;

    ret = (presented_content_id[0] != current_content_id())
          ? DOS_MEDIA_CHANGED : DOS_MEDIA_NOT_CHANGED;
    poke_b(req_seg, req_off + REQ_MEDIA_RET, ret);
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* BUILD_BPB (cmd 2) - DOS does not supply a destination pointer here;
 * it reads the BPB through the far pointer the driver itself writes
 * back at REQ_BPB_OFF/REQ_BPB_SEG (offset = BPB address, segment =
 * CS), same as vdiskc3.c:287-293. There is always a valid disk to
 * describe (the virtual one if nothing else), so there is no
 * not-ready error path any more. */
static void cmd_build_bpb()
{
    poke_w(req_seg, req_off + REQ_BPB_OFF, (unsigned int)bpb);
    poke_w(req_seg, req_off + REQ_BPB_SEG, get_cs());

    presented_content_id[0] = current_content_id();

    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE);
}

/* READ (cmd 4) - transfer address dword at REQ_BRK_OFF/SEG (0x0E/
 * 0x10), sector count at REQ_BPB_OFF (0x12), start sector at
 * REQ_BPB_SEG (0x14) - same request layout as vdiskc3.c's cmd_read().
 * Generates virtual-disk sectors on the fly; an out-of-range sector
 * stops the transfer and reports ERR_SECTOR_NOT_FOUND, with the
 * sector count field updated to how many sectors were actually
 * transferred before that point. */
static void cmd_read()
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

/* WRITE (cmds 8/9) on the virtual disk - always write-protected, same
 * as vdiskc3.c's cmd_write_protected(). */
static void cmd_write_protected()
{
    poke_w(req_seg, req_off + REQ_STATUS, ST_DONE | ST_ERROR | ERR_WRITEPROT);
}

void dispatch()
{
    unsigned char cmd;

    guard();

    cmd = peek_b(req_seg, req_off + REQ_CMD);

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
    case CMD_READ:
        cmd_read();
        break;
    case CMD_WRITE:
    case CMD_WRITE_VERIFY:
        cmd_write_protected();
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
