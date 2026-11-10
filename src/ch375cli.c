/* POFOUDSK.COM - CLI client for the CH375 USB host controller.
 *
 * Two modes, picked automatically, no command-line switches:
 *
 *   - POFOUDSK.SYS loaded: scan A:..E: via INT 21h/44h/04h ("read control
 *     data") for the driver's "POFOUDSK" signature. If found, use
 *     IOCTL only (driver version + ctrl_state) and never touch the
 *     CH375 ports directly - that would desync the driver's own guard
 *     state machine.
 *   - No driver: direct hardware mode. PID check (INT 61h/AH=1Ah,
 *     ID 0x75, same check the driver itself does) before any port
 *     access, then CHECK_EXIST, IC version, USB host mode (reset +
 *     SOF), bounded wait for USB connect, DISK_INIT, DISK_SIZE, MBR
 *     partition table, and the BPB + FAT12 verdict for the chosen
 *     partition.
 *
 * Output is kept to short lines - the Portfolio screen is 40x8;
 * redirect to a file with `>` for the full dump. No printf - lines
 * are built with lib/pofo tinylib's fmt() into out_line (disk.c) and
 * written with con_puts().
 */

#include "ch375.h"
#include "ch375disk.h"
#include "part.h"
#include "pofoudsk.h"
#include "disk.h"
#include "tinylib.h"
#include "tinynum.h"

#define IOCTL_SIG_LEN POFOUDSK_IOCTL_V0_LEN

/* Set by call_ioctl() as a side effect (its body is #asm only, so it
 * can't just return two values) - carry flag after INT 21h: 1 = CF
 * set (DOS rejected/doesn't support the call), 0 = CF clear. */
static unsigned char dos_carry;

/* AH=44h dispatcher - al picks the IOCTL subfunction, bl the drive
 * number (1=A:), cx a count, dx the offset of a local buffer (same
 * data segment as this .COM program, so a near pointer is enough).
 * Returns AX as seen right after INT 21h; dos_carry is set from the
 * carry flag. */
static unsigned int call_ioctl(al, bl, cx, dx)
unsigned char al;
unsigned char bl;
unsigned int cx;
unsigned int dx;
{
#asm
    push bp
    mov bp,sp
    push bx
    push cx
    push dx
    mov al,4[bp]
    mov bl,6[bp]
    mov cx,8[bp]
    mov dx,10[bp]
    mov ah,#0x44
    int 0x21
    jc ioctl_cf_set
    mov byte ptr _dos_carry,#0
    jmp ioctl_cf_done
ioctl_cf_set:
    mov byte ptr _dos_carry,#1
ioctl_cf_done:
    pop dx
    pop cx
    pop bx
    pop bp
#endasm
}

/* PID presence check (Atari Portfolio BIOS, INT 61h/AH=1Ah), same
 * check POFOUDSK.SYS's own guard() runs: AL=0 -> no PID card, AL!=0 ->
 * present, AH = PID id byte. Returns 1 only for PID_ID_CH375. Must be
 * called (and succeed) before any direct 807D/807E port access. */
#define PID_ID_CH375 0x75

static unsigned char pid_check()
{
#asm
    mov ah,#0x1A
    int 0x61
    or al,al
    jz cli_pid_absent
    cmp ah,#PID_ID_CH375
    jne cli_pid_absent
    mov al,#1
    jmp cli_pid_done
cli_pid_absent:
    xor al,al
cli_pid_done:
#endasm
}

static char ioctl_buf[IOCTL_SIG_LEN];

/* IOCTL v1 reply layout (see src/ch375sys.c cmd_ioctl_in()): v0's 13-byte signature/version/state
 * block, then usb_state(1)/mount_gen(1)/unit_count(1), vendor[8],
 * product[16], capacity sectors hi/lo (2+2), then MAX_UNITS=4 * 10
 * bytes (type, unit status, start hi/lo, size hi/lo). */
#define IOCTL_V1_LEN (IOCTL_SIG_LEN + 3 + 8 + 16 + 4 + 4 * 10)
static unsigned char v1_buf[IOCTL_V1_LEN];

static unsigned int get16(p)
unsigned char *p;
{
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

/* "Drv X: vM.m.p stSS" - driver identification from the v0 part of
 * an IOCTL reply, into out_line; returns the terminator position so
 * the caller can append. */
static char *put_drv(first_letter, r)
char first_letter;
unsigned char *r;
{
    char *p;

    p = fmt(out_line, "Drv ");
    *p++ = first_letter;
    return fmt(p, ": v%u.%u.%u st%x", r[8], r[9], r[10], r[11]);
}

/* Prints the per-unit summary lines from an already-fetched v1_buf -
 * one line per unit, drive letter = first_letter + unit index (unit n
 * = MBR partition n+1). */
static void print_units(first_letter, unit_count)
char first_letter;
unsigned char unit_count;
{
    unsigned char n, status;
    unsigned char *u;
    char *p;

    u = v1_buf + IOCTL_SIG_LEN + 3 + 8 + 16 + 4;
    for (n = 0; n < unit_count; n++, u += 10) {
        status = u[1];

        p = out_line;
        *p++ = first_letter + n;
        p = fmt(p, ": P%u ", n + 1);
        if (status == CH375DISK_UNIT_EMPTY)
            fmt(p, "empty\r\n");
        else if (status == CH375DISK_UNIT_GPT)
            fmt(p, "GPT unsup\r\n");
        else if (status == CH375DISK_UNIT_REAL) {
            p = fmt(p, "t%x ", u[0]);
            p = put_mb(p, get16(u + 6), get16(u + 8));
            fmt(p, " OK\r\n");
        } else
            fmt(p, "t%x unsup\r\n", u[0]);
        con_puts(out_line);
    }
}

/* Scans A:..E: for POFOUDSK.SYS's IOCTL signature reply. On a match, fetches the full v1
 * reply and prints a compact driver-mode summary (<=7 lines on the
 * 40x8 screen), and returns 1 (driver mode handled). Returns 0 if no
 * drive answered - caller falls back to direct hardware mode. */
static unsigned char try_driver_mode()
{
    unsigned char drive, i;
    unsigned int ax;
    char first_letter;
    char vend[9], prod[17];
    unsigned char unit_count;

    for (drive = 1; drive <= 5; drive++) {
        for (i = 0; i < IOCTL_SIG_LEN; i++)
            ioctl_buf[i] = 0;

        ax = call_ioctl(0x04, drive, IOCTL_SIG_LEN, (unsigned int) ioctl_buf);
        if (dos_carry)
            continue;

        if (ioctl_buf[0] != 'P' || ioctl_buf[1] != 'O' || ioctl_buf[2] != 'F'
            || ioctl_buf[3] != 'O' || ioctl_buf[4] != 'U' || ioctl_buf[5] != 'D'
            || ioctl_buf[6] != 'S' || ioctl_buf[7] != 'K')
            continue;

        first_letter = 'A' + drive - 1;

        for (i = 0; i < IOCTL_V1_LEN; i++)
            v1_buf[i] = 0;
        ax = call_ioctl(0x04, drive, IOCTL_V1_LEN, (unsigned int) v1_buf);
        if (dos_carry) {
            /* Driver too old for v1 (or the call failed outright) -
             * fall back to the v0-only line. */
            fmt(put_drv(first_letter, ioctl_buf), "\r\n");
            con_puts(out_line);
            return 1;
        }

        unit_count = v1_buf[15];
        if (unit_count == 0 || unit_count > 4)
            unit_count = 1;

        fmt(put_drv(first_letter, v1_buf), " n%u\r\n", unit_count);
        con_puts(out_line);

        if (v1_buf[13]) { /* usb_state */
            for (i = 0; i < 8; i++)
                vend[i] = v1_buf[16 + i];
            vend[8] = 0;
            for (i = 0; i < 16; i++)
                prod[i] = v1_buf[24 + i];
            prod[16] = 0;
            fmt(put_mb(fmt(out_line, "%s %s\r\n", vend, prod),
                       get16(v1_buf + 40), get16(v1_buf + 42)), "\r\n");
            con_puts(out_line);
        }

        print_units(first_letter, unit_count);
        return 1;
    }

    return 0;
}

/* "<s><hex byte>" status line. */
static void print_hex(s, v)
char *s;
unsigned char v;
{
    fmt(out_line, "%s%x\r\n", s, v);
    con_puts(out_line);
}

static void direct_hw_mode()
{
    int status, n, verdict;
    unsigned char ver, part_type;
    unsigned int start_hi, start_lo, sectors_hi, sectors_lo;
    struct ch375disk_mbr mbr;
    struct ch375disk_bpb bpb;
    unsigned char boot_jump;

    if (!pid_check()) {
        con_puts("No PID card\r\n");
        return;
    }

    if (ch375_check_exist(0x5A) != (unsigned char)~0x5A) {
        con_puts("Not CH375\r\n");
        return;
    }

    ver = ch375_get_ic_ver();
    print_hex("CH375 ver", ver);

    if (!ch375_host_reset()) {
        con_puts("USB mode fail\r\n");
        return;
    }

    con_puts("Wait USB...\r\n");
    status = ch375disk_detect();
    if (status == USB_INT_DISCONNECT) {
        con_puts("No USB disk\r\n");
        return;
    }
    if (status != USB_INT_SUCCESS) {
        print_hex("DISK_INIT ", status);
        return;
    }
    fmt(out_line, "%s %s\r\n", ch375disk_vendor, ch375disk_product);
    con_puts(out_line);

    status = ch375disk_size();
    if (status != USB_INT_SUCCESS) {
        fmt(out_line, "DISK_SIZE %x (%u tries)\r\n", status, ch375disk_size_tries);
        con_puts(out_line);
        return;
    }
    if (ch375disk_size_tries > 1) {
        fmt(out_line, "DISK_SIZE ok, try %u\r\n", ch375disk_size_tries);
        con_puts(out_line);
    }
    disk_print_capacity();

    status = ch375disk_read_mbr(0, 0, &mbr);
    if (status != USB_INT_SUCCESS) {
        print_hex("MBR read ", status);
        return;
    }

    if (mbr.is_superfloppy) {
        con_puts("no MBR, superfloppy\r\n");
    } else if (mbr.has_mbr_sig) {
        for (n = 0; n < CH375DISK_PART_COUNT; n++)
            if (mbr.part[n].type != 0)
                disk_print_mbr_partition(&mbr.part[n], n);
    } else {
        con_puts("no MBR sig\r\n");
    }

    part_type = ch375disk_pick_volume(&mbr, &start_hi, &start_lo, &sectors_hi, &sectors_lo);
    if (part_type == CH375DISK_NO_VOLUME) {
        con_puts("no partition\r\n");
        return;
    }

    status = ch375disk_read_bpb(start_hi, start_lo, &bpb, &boot_jump);
    if (status != USB_INT_SUCCESS) {
        print_hex("BPB read ", status);
        return;
    }

    disk_print_bpb(&bpb);
    verdict = ch375disk_fat12_check(part_type, sectors_hi, sectors_lo, &bpb);
    disk_print_fat12_verdict(verdict);
}

main()
{
    if (try_driver_mode())
        return 0;

    direct_hw_mode();
    return 0;
}
