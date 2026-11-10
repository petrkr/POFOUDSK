/* POFOUDSK.SYS - DOS 2.x block device driver for USB flash disks on a
 * CH375 PID card: device header, strategy/interrupt entry (own stack,
 * BCC code needs SS == DS), request dispatch and command handlers.
 * Must be linked first - the header has to be at file offset 0. */

#include "ch375.h"
#include "ch375disk.h"
#include "transport.h"
#include "pofoudsk.h"
#include "tinynum.h"
#include "sys.h"
#include "version.h"
#include "build_id.h"

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

/* MEDIA_CHECK return byte - same offset as INIT's break address. */
#define REQ_MEDIA_RET  0x0E

#define DOS_MEDIA_CHANGED     0xFF /* -1: changed or unknown */
#define DOS_MEDIA_NOT_CHANGED 0x01

#define CMD_INIT          0
#define CMD_MEDIA_CHECK   1
#define CMD_BUILD_BPB     2
#define CMD_IOCTL_IN      3
#define CMD_READ          4
#define CMD_WRITE         8
#define CMD_WRITE_VERIFY  9
#define CMD_IOCTL_OUT     0x0C

/* /N:x, x = 1..MAX_UNITS; unit n = MBR primary partition n+1. */
#define DEFAULT_UNITS 1
#define REQ_UNIT      1    /* unit number byte in every request packet */
#define CMDLINE_MAX   128  /* scan limit for the CONFIG.SYS line */

/* Deepest path (mount from MEDIA_CHECK) estimated at ~250 B; not
 * measured on HW. */
#define INT_STACK_SIZE 512

/* Device header - must be the first bytes of the file. _start only
 * satisfies ld86's entry point. */
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

/* .bss is not zeroed for a .SYS - init_state() sets everything. */

/* Last IOCTL output byte, echoed in the IOCTL input reply. */
unsigned char ioctl_last_cmd;

unsigned char unit_count;
unsigned char cur_unit;

/* /NODIRFIX: DIRFIX not installed. */
static unsigned char nodirfix;


/* Content id last handed to DOS via BUILD_BPB. */
static unsigned int presented_content_id[MAX_UNITS];

/* Driver stack - _interrupt switches to it. */
static unsigned char int_stack[INT_STACK_SIZE];
static unsigned int saved_ss;
static unsigned int saved_sp;

/* DOS 2.x does not fill REQ_FIRSTDRIVE (0x16), so the assigned drive
 * letter is not printed. */

/* Version and build id are filled in at INIT. */
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

/* ld86's __end (end of .bss) = end of resident memory. */
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

/* Must run before the first guard() - .bss is not zeroed. */
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

/* Case-insensitive match of an upper-case literal at seg:off. */
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

/* /N:x and /NODIRFIX from the CONFIG.SYS line (far pointer at 0x12 of
 * the INIT request). */
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
    /* constant shifts - folded by BCC, no long helpers */
    p = put_hex2(p, (unsigned int) (POFOUDSK_BUILD_ID >> 24) & 0xFF);
    p = put_hex2(p, (unsigned int) (POFOUDSK_BUILD_ID >> 16) & 0xFF);
    p = put_hex2(p, (unsigned int) (POFOUDSK_BUILD_ID >> 8) & 0xFF);
    put_hex2(p, (unsigned int) POFOUDSK_BUILD_ID & 0xFF);

    print_dollar_string(msg_version);
}

/* Reports the state guard() already settled - no extra HELLO. */
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
    /* No media known at INIT - every unit starts with the virtual BPB. */
    for (i = 0; i < MAX_UNITS; i++)
        bpb_array[i] = (unsigned int)bpb;
    req_set(REQ_BPB_OFF, (unsigned int)bpb_array);
    req_set(REQ_BPB_SEG, get_cs());

    print_version();
    probe_controller();
    msg_units[MSG_UNITS_OFFSET] = '0' + unit_count;
    print_dollar_string(msg_units);
    if (nodirfix)
        print_dollar_string(msg_dirfix_off);
    else if (dirfix_install())
        print_dollar_string(msg_dirfix_on);
    else
        print_dollar_string(msg_dirfix_bad);

    for (i = 0; i < MAX_UNITS; i++)
        presented_content_id[i] = current_content_id(i);

    req_status(ST_DONE);
}

/* MEDIA_CHECK: resync via TEST_CONNECT first (a reset CH375 raises no
 * interrupt), then "changed" iff the unit's content id differs from
 * the one handed out by BUILD_BPB. */
static void cmd_media_check()
{
    if (ctrl_state == STATE_CONTROLLER_OK)
        test_connect_resync();

    poke_b(req_seg, req_off + REQ_MEDIA_RET,
           (presented_content_id[cur_unit] != current_content_id(cur_unit))
           ? DOS_MEDIA_CHANGED : DOS_MEDIA_NOT_CHANGED);
    req_status(ST_DONE);
}

/* BUILD_BPB: far pointer to the unit's BPB returned at 0x12. */
static void cmd_build_bpb()
{
    req_set(REQ_BPB_OFF, unit_is_real(cur_unit)
            ? (unsigned int) unit_bpb[cur_unit] : (unsigned int) bpb);
    req_set(REQ_BPB_SEG, get_cs());

    presented_content_id[cur_unit] = current_content_id(cur_unit);

    req_status(ST_DONE);
}

/* DOS buffer of the sector in transfer - the chunk callbacks have a
 * fixed signature. */
static unsigned int buf_seg;
static unsigned int buf_off;

/* READ: USB chunk -> DOS buffer. */
static real_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    far_copy(buf_seg, buf_off + offset, chunk, (unsigned int) len);
}

/* WRITE: length byte + 64 bytes from the DOS buffer -> data port. */
static real_chunk_supplier(offset, len)
unsigned int offset;
unsigned int len;
{
    tr_wdata((unsigned char) len);
    tr_wdata_far(buf_seg, buf_off + offset, len);
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

/* READ (cmd 4) / WRITE (cmds 8, 9): buffer at 0x0E, sector count at
 * 0x12 (written back as transferred), start sector at 0x14. Virtual
 * units: generated sectors, writes rejected. Real units: one sector at
 * a time, a disk error gets one REQUEST SENSE + retry, a timeout or
 * disconnect drops the mount. */
static void cmd_rw(is_write)
unsigned char is_write;
{
    unsigned int off, seg, count, sector, transferred, limit;
    unsigned int lba_hi, lba_lo;
    unsigned char real;
    struct unit_state *u;
    int status;
    unsigned int stop_err; /* 0 or DOS error code */

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
    /* REAL units have size_hi == 0 */
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
                /* disk error: REQUEST SENSE + one retry */
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

/* IOCTL input (INT 21h/44h/04h), v1 reply, little-endian:
 *   "POFOUDSK", ver major/minor/patch, ctrl_state, last IOCTL byte,
 *   usb_state, mount_gen, unit_count, vendor[8], product[16],
 *   capacity hi/lo, 4 x (type, status, start hi/lo, size hi/lo).
 * At most the requested byte count is returned. */
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

/* IOCTL output (INT 21h/44h/05h): stores the first byte. */
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

/* strategy/interrupt entry points - raw asm, no BCC prologue. */
#asm
_strategy:
    seg cs
    mov	_req_off,bx
    seg cs
    mov	_req_seg,es
    retf

/* DIRFIX single-steps DOS with TF set; the driver itself needs no
 * stepping, so it runs with TF off and the caller's flags come back
 * on return. */
_interrupt:
    pushf
    push ax
    pushf
    pop ax
    and ah,#0xFE
    push ax
    popf
    pop ax
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
    popf
    retf
#endasm
