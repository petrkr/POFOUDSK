/* POFOUDSK.COM - status tool. With POFOUDSK.SYS loaded it only talks to
 * the driver via IOCTL (direct port access would desync the driver);
 * otherwise it probes the CH375 directly: PID check, chip, USB disk,
 * capacity, MBR, BPB and FAT12 verdict. Lines fit the 40x8 screen. */

#include "ch375.h"
#include "ch375disk.h"
#include "part.h"
#include "pofoudsk.h"
#include "disk.h"
#include "tinylib.h"
#include "tinynum.h"

#define IOCTL_SIG_LEN POFOUDSK_IOCTL_V0_LEN

/* CF after the last INT 21h/44h */
static unsigned char dos_carry;

/* INT 21h/44h: al = subfunction, bl = drive (1 = A:), cx = count,
 * dx = buffer. Returns AX, sets dos_carry. */
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

/* INT 61h/1Ah: 1 if the CH375 PID card is present - required before
 * any port access. */
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

/* IOCTL v1 reply: v0 block (13), usb_state, mount_gen, unit_count,
 * vendor[8], product[16], capacity hi/lo, 4 x 10 bytes per unit. */
#define IOCTL_V1_LEN (IOCTL_SIG_LEN + 3 + 8 + 16 + 4 + 4 * 10)
static unsigned char v1_buf[IOCTL_V1_LEN];

static unsigned int get16(p)
unsigned char *p;
{
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

/* "Drv X: vM.m.p stSS" into out_line; returns the end for appending. */
static char *put_drv(first_letter, r)
char first_letter;
unsigned char *r;
{
    char *p;

    p = fmt(out_line, "Drv ");
    *p++ = first_letter;
    return fmt(p, ": v%u.%u.%u st%x", r[8], r[9], r[10], r[11]);
}

/* One line per unit: drive letter, partition, type, size/verdict. */
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

/* Finds the driver on A:..E: by its IOCTL signature and prints its
 * state. Returns 0 if not found. */
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
            /* driver without v1 reply: v0 line only */
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
