/* CH375.COM - CLI client for the CH375 USB host controller.
 *
 * Two modes, picked automatically, no command-line switches:
 *
 *   - CH375.SYS loaded: scan A:..E: via INT 21h/44h/04h ("read control
 *     data") for the driver's "CH375SYS" signature. If found, use
 *     IOCTL only (driver version + ctrl_state) and never touch the
 *     CH375 ports directly - that would desync the driver's own guard
 *     state machine. More IOCTL commands land in a later phase.
 *   - No driver: direct hardware mode. PID check (INT 61h/AH=1Ah,
 *     ID 0x75, same check the driver itself does) before any port
 *     access, then CHECK_EXIST, IC version, USB host mode (reset +
 *     SOF), bounded wait for USB connect, DISK_INIT, DISK_SIZE, MBR
 *     partition table, and the BPB + FAT12 verdict for the chosen
 *     partition.
 *
 * Output is kept to short lines - the Portfolio screen is 40x8;
 * redirect to a file with `>` for the full dump.
 */

#include "ch375.h"
#include "ch375disk.h"
#include "disk.h"

#define IOCTL_SIG_LEN 13

/* Set by call_ioctl() as a side effect (its body is #asm only, so it
 * can't just return two values) - carry flag after INT 21h: 1 = CF
 * set (DOS rejected/doesn't support the call), 0 = CF clear. Same
 * pattern as the removed ioctltst.c. */
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
 * check CH375.SYS's own guard() runs: AL=0 -> no PID card, AL!=0 ->
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

/* IOCTL v1 reply layout (see src/ch375sys.c cmd_ioctl_in(),
 * ch375driver.md "IOCTL v1"): v0's 13-byte signature/version/state
 * block, then usb_state(1)/mount_gen(1)/unit_count(1), vendor[8],
 * product[16], capacity sectors hi/lo (2+2), then MAX_UNITS=4 * 10
 * bytes (type, unit status, start hi/lo, size hi/lo). */
#define IOCTL_V1_LEN (IOCTL_SIG_LEN + 3 + 8 + 16 + 4 + 4 * 10)
static unsigned char v1_buf[IOCTL_V1_LEN];

/* Unit status codes, matching ch375disk.h's CH375DISK_UNIT_* - used
 * here only for display, not protocol logic. */

/* Prints the per-unit summary lines from an already-fetched v1_buf -
 * one line per unit, drive letter = first_letter + unit index (see
 * ch375driver.md "Pn mapovani"). */
static void print_units(first_letter, unit_count)
char first_letter;
unsigned char unit_count;
{
    unsigned char n, base, status, part_type;
    unsigned int size_hi, size_lo;
    unsigned long mb;

    for (n = 0; n < unit_count; n++) {
        base = IOCTL_SIG_LEN + 3 + 8 + 16 + 4 + n * 10;
        part_type = v1_buf[base + 0];
        status = v1_buf[base + 1];
        size_hi = (unsigned int) v1_buf[base + 6] | ((unsigned int) v1_buf[base + 7] << 8);
        size_lo = (unsigned int) v1_buf[base + 8] | ((unsigned int) v1_buf[base + 9] << 8);

        printf("%c: P%u ", first_letter + n, n + 1);
        if (status == CH375DISK_UNIT_REAL) {
            mb = (disk_hilo_to_long(size_hi, size_lo) * 512UL) / 1048576UL;
            printf("t%02x %luMB OK\n", (unsigned int) part_type, mb);
        } else if (status == CH375DISK_UNIT_EMPTY) {
            printf("empty\n");
        } else if (status == CH375DISK_UNIT_GPT) {
            printf("GPT unsup\n");
        } else {
            printf("t%02x unsup\n", (unsigned int) part_type);
        }
    }
}

/* Scans A:..E: for CH375.SYS's IOCTL signature reply (see
 * ch375driver.md "IOCTL odpoved"). On a match, fetches the full v1
 * reply and prints a compact driver-mode summary (<=7 lines on the
 * 40x8 screen), and returns 1 (driver mode handled). Returns 0 if no
 * drive answered - caller falls back to direct hardware mode. */
static unsigned char try_driver_mode()
{
    unsigned char drive, i;
    unsigned int ax;
    char first_letter;
    char vend[9], prod[17];
    unsigned char usb_mounted, unit_count;
    unsigned int cap_hi, cap_lo;
    unsigned long mb;

    for (drive = 1; drive <= 5; drive++) {
        for (i = 0; i < IOCTL_SIG_LEN; i++)
            ioctl_buf[i] = 0;

        ax = call_ioctl(0x04, drive, IOCTL_SIG_LEN, (unsigned int) ioctl_buf);
        if (dos_carry)
            continue;

        if (ioctl_buf[0] != 'C' || ioctl_buf[1] != 'H' || ioctl_buf[2] != '3'
            || ioctl_buf[3] != '7' || ioctl_buf[4] != '5' || ioctl_buf[5] != 'S'
            || ioctl_buf[6] != 'Y' || ioctl_buf[7] != 'S')
            continue;

        first_letter = 'A' + drive - 1;

        for (i = 0; i < IOCTL_V1_LEN; i++)
            v1_buf[i] = 0;
        ax = call_ioctl(0x04, drive, IOCTL_V1_LEN, (unsigned int) v1_buf);
        if (dos_carry) {
            /* Driver too old for v1 (or the call failed outright) -
             * fall back to the v0-only line. */
            printf("Drv %c: v%u.%u.%u st%02x\n", first_letter,
                   (unsigned int) ioctl_buf[8], (unsigned int) ioctl_buf[9],
                   (unsigned int) ioctl_buf[10], (unsigned int) ioctl_buf[11]);
            return 1;
        }

        unit_count = v1_buf[15];
        if (unit_count == 0 || unit_count > 4)
            unit_count = 1;

        printf("Drv %c: v%u.%u.%u st%02x n%u\n", first_letter,
               (unsigned int) v1_buf[8], (unsigned int) v1_buf[9],
               (unsigned int) v1_buf[10], (unsigned int) v1_buf[11],
               (unsigned int) unit_count);

        usb_mounted = v1_buf[13];
        if (usb_mounted) {
            for (i = 0; i < 8; i++)
                vend[i] = v1_buf[16 + i];
            vend[8] = 0;
            for (i = 0; i < 16; i++)
                prod[i] = v1_buf[24 + i];
            prod[16] = 0;
            printf("%s %s\n", vend, prod);

            cap_hi = (unsigned int) v1_buf[40] | ((unsigned int) v1_buf[41] << 8);
            cap_lo = (unsigned int) v1_buf[42] | ((unsigned int) v1_buf[43] << 8);
            mb = (disk_hilo_to_long(cap_hi, cap_lo) * 512UL) / 1048576UL;
            printf("%luMB\n", mb);
        }

        print_units(first_letter, unit_count);
        return 1;
    }

    return 0;
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
        printf("No PID card\n");
        return;
    }

    if (ch375_check_exist(0x5A) != (unsigned char)~0x5A) {
        printf("Not CH375\n");
        return;
    }

    ver = ch375_get_ic_ver();
    printf("CH375 ver%02x\n", (unsigned int) ver);

    if (!ch375_host_reset()) {
        printf("USB mode fail\n");
        return;
    }

    printf("Wait USB...\n");
    status = ch375disk_detect();
    if (status == USB_INT_DISCONNECT) {
        printf("No USB disk\n");
        return;
    }
    if (status != USB_INT_SUCCESS) {
        printf("DISK_INIT %02x\n", status);
        return;
    }
    printf("%s %s\n", ch375disk_vendor, ch375disk_product);

    status = ch375disk_size();
    if (status != USB_INT_SUCCESS) {
        printf("DISK_SIZE %02x (%d tries)\n", status, ch375disk_size_tries);
        return;
    }
    if (ch375disk_size_tries > 1)
        printf("DISK_SIZE ok, try %d\n", ch375disk_size_tries);
    disk_print_capacity();

    status = ch375disk_read_mbr(0, 0, &mbr);
    if (status != USB_INT_SUCCESS) {
        printf("MBR read %02x\n", status);
        return;
    }

    if (mbr.is_superfloppy) {
        printf("no MBR, superfloppy\n");
    } else if (mbr.has_mbr_sig) {
        for (n = 0; n < CH375DISK_PART_COUNT; n++)
            if (mbr.part[n].type != 0)
                disk_print_mbr_partition(&mbr.part[n], n);
    } else {
        printf("no MBR sig\n");
    }

    part_type = ch375disk_pick_volume(&mbr, &start_hi, &start_lo, &sectors_hi, &sectors_lo);
    if (part_type == CH375DISK_NO_VOLUME) {
        printf("no partition\n");
        return;
    }

    status = ch375disk_read_bpb(start_hi, start_lo, &bpb, &boot_jump);
    if (status != USB_INT_SUCCESS) {
        printf("BPB read %02x\n", status);
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
