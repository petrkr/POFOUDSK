/* Controller/USB state machine (guard()) and per-unit mount state.
 * Mount/unmount is event-driven: one IN of the CH375 interrupt flag per
 * request, GET_STATUS only when an interrupt is pending. */

#include "ch375.h"
#include "ch375disk.h"
#include "part.h"
#include "pofoudsk.h"
#include "sys.h"

/* Settle time after USB_INT_CONNECT before DISK_INIT. */
#define CONNECT_SETTLE_MS 100

/* TEST_CONNECT, datasheet 5.10 */
#define CMD_TEST_CONNECT 0x16

/* .bss is not zeroed for a .SYS - init_mount_state() sets all of it. */
unsigned char ctrl_state;

/* 1 after a successful DISK_INIT + DISK_SIZE */
unsigned char usb_state;

/* Bumped on every mount/unmount, part of the content id, so swapping
 * one disk for another with the same verdict still reads as a change. */
unsigned char mount_gen;

/* Own copies - ch375disk_vendor/product change on every DISK_INIT. */
char vendor[9];
char product[17];

unsigned int cap_sectors_hi;
unsigned int cap_sectors_lo;

struct unit_state units[MAX_UNITS];

/* BPB of each REAL unit (boot sector bytes 0x0B..0x17). */
unsigned char unit_bpb[MAX_UNITS][13];

/* Resets a unit to status, no geometry, current mount_gen. */
static void set_unit(u, status)
struct unit_state *u;
unsigned char status;
{
    u->status = status;
    u->reason = 0;
    u->part_type = 0;
    u->start_hi = 0;
    u->start_lo = 0;
    u->size_hi = 0;
    u->size_lo = 0;
    u->mount_gen = mount_gen;
}

static void set_all_units(status, reason)
unsigned char status;
unsigned char reason;
{
    struct unit_state *u;

    for (u = units; u < units + MAX_UNITS; u++) {
        set_unit(u, status);
        u->reason = reason;
    }
}

static void clear_usb_info()
{
    usb_state = 0;
    vendor[0] = 0;
    product[0] = 0;
    cap_sectors_hi = 0;
    cap_sectors_lo = 0;
}

void init_mount_state()
{
    ctrl_state = STATE_NO_CONTROLLER;
    mount_gen = 0;
    clear_usb_info();
    set_all_units(CH375DISK_UNIT_NO_CONTROLLER, 0);
}

/* CH375 lost or failed - the next request re-runs HELLO + reset. */
void drop_controller()
{
    ctrl_state = STATE_NO_CONTROLLER;
}

/* All units back to NO_USB, new mount_gen. */
void unmount_all()
{
    clear_usb_info();
    mount_gen++;
    set_all_units(CH375DISK_UNIT_NO_USB, 0);
}

/* On OK the BPB comes from ch375disk_bpb_raw (the sector just read). */
static void apply_verdict(n, verdict)
unsigned char n;
int verdict;
{
    struct unit_state *u;
    unsigned char i;

    u = &units[n];
    if (verdict == CH375DISK_FAT12_OK) {
        u->status = CH375DISK_UNIT_REAL;
        for (i = 0; i < CH375DISK_BPB_RAW_LEN; i++)
            unit_bpb[n][i] = ch375disk_bpb_raw[i];
        return;
    }
    u->status = CH375DISK_UNIT_UNSUPPORTED;
    if (verdict == CH375DISK_FAT12_TOO_BIG)
        u->reason = CH375DISK_UNIT_REASON_TOO_BIG;
    else if (verdict == CH375DISK_FAT12_BAD_BPB)
        u->reason = CH375DISK_UNIT_REASON_BAD_BPB;
    else
        u->reason = CH375DISK_UNIT_REASON_BAD_TYPE;
}

/* Sets units[n] from MBR entry n. Returns CH375_TIMEOUT if the BPB read
 * timed out, else 0. */
static int evaluate_partition(n, p)
unsigned char n;
struct ch375disk_part *p;
{
    struct unit_state *u;
    struct ch375disk_bpb pbpb;
    unsigned char boot_jump;
    int status;

    u = &units[n];
    set_unit(u, CH375DISK_UNIT_UNSUPPORTED);
    u->part_type = p->type;
    u->start_hi = p->lba_start_hi;
    u->start_lo = p->lba_start_lo;
    u->size_hi = p->sectors_hi;
    u->size_lo = p->sectors_lo;

    if (p->type == 0) {
        u->status = CH375DISK_UNIT_EMPTY;
        return 0;
    }
    if (p->type == 0xEE) {
        u->status = CH375DISK_UNIT_GPT;
        return 0;
    }
    if (p->boot_flag != 0x00 && p->boot_flag != 0x80)
        return 0; /* UNSUPPORTED, BAD_TYPE */

    status = ch375disk_read_bpb(p->lba_start_hi, p->lba_start_lo, &pbpb, &boot_jump);
    if (status == CH375_TIMEOUT)
        return CH375_TIMEOUT;
    if (status != USB_INT_SUCCESS) {
        u->reason = CH375DISK_UNIT_REASON_BAD_BPB;
        return 0;
    }

    apply_verdict(n, ch375disk_fat12_check(p->type, p->sectors_hi, p->sectors_lo, &pbpb));
    return 0;
}

/* After USB_INT_CONNECT: DISK_INIT, DISK_SIZE, MBR, then every
 * partition - or the superfloppy (unit 0 = whole disk, units 1-3 EMPTY).
 * A timeout anywhere drops the controller. */
static void mount_usb()
{
    struct ch375disk_mbr mbr;
    int status;
    unsigned char n;

    ch375_delay_ms(CONNECT_SETTLE_MS);

    status = ch375disk_init();
    if (status == USB_INT_SUCCESS)
        status = ch375disk_size();
    if (status != USB_INT_SUCCESS)
        goto fail;

    cap_sectors_hi = ch375disk_sector_count_hi;
    cap_sectors_lo = ch375disk_sector_count_lo;
    for (n = 0; n < 8; n++)
        vendor[n] = ch375disk_vendor[n];
    vendor[8] = 0;
    for (n = 0; n < 16; n++)
        product[n] = ch375disk_product[n];
    product[16] = 0;

    status = ch375disk_read_mbr(0, 0, &mbr);
    if (status == CH375_TIMEOUT)
        goto fail;

    usb_state = 1;
    mount_gen++;

    if (status != USB_INT_SUCCESS || (!mbr.is_superfloppy && !mbr.has_mbr_sig)) {
        /* MBR unreadable, or neither MBR signature nor superfloppy */
        set_all_units(CH375DISK_UNIT_UNSUPPORTED, CH375DISK_UNIT_REASON_BAD_BPB);
        return;
    }

    if (mbr.is_superfloppy) {
        set_all_units(CH375DISK_UNIT_EMPTY, 0);
        units[0].size_lo = mbr.bpb0.total_sectors;
        apply_verdict(0, ch375disk_fat12_check(0, 0, mbr.bpb0.total_sectors, &mbr.bpb0));
        return;
    }

    for (n = 0; n < MAX_UNITS; n++) {
        if (evaluate_partition(n, &mbr.part[n]) == CH375_TIMEOUT) {
            status = CH375_TIMEOUT;
            goto fail;
        }
    }
    return;

fail:
    if (status == CH375_TIMEOUT)
        drop_controller();
    unmount_all();
}

/* Acts on a pending CH375 interrupt (CONNECT/DISCONNECT) only. */
static void check_usb_event()
{
    int status;

    if (ch375_read_cmd_token() & 0x80)
        return; /* no interrupt pending */

    status = ch375_get_status();
    if (status == USB_INT_CONNECT)
        mount_usb();
    else if (status == USB_INT_DISCONNECT)
        unmount_all();
}

/* TEST_CONNECT resync, called from MEDIA_CHECK: catches a CH375 that
 * was reset (e.g. Portfolio sleep/wake) without raising an interrupt.
 * The sleep/wake case is unverified on real HW. */
void test_connect_resync()
{
    unsigned char result;

    ch375_write_cmd(CMD_TEST_CONNECT);
    result = ch375_read_data();
    if (result == 0)
        result = ch375_read_data(); /* not finished yet - one re-read */

    if (result == 0)
        return; /* still not finished - try again next time */

    if (result == USB_INT_DISCONNECT) {
        if (usb_state)
            unmount_all(); /* missed unplug */
    } else if (result == USB_INT_CONNECT) {
        if (usb_state)
            unmount_all(); /* connected but not initialized = device reset */
        mount_usb(); /* missed plug, or remount after a device reset */
    } else if (result == USB_INT_USB_READY) {
        if (!usb_state)
            mount_usb(); /* missed plug */
    } else {
        /* chip no longer in host mode */
        drop_controller();
        unmount_all();
    }
}

/* Runs on every request: PID check always, CH375 HELLO + host reset
 * only when the PID card (re)appears, USB events while CONTROLLER_OK. */
void guard()
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
    /* A disk already plugged in raises CONNECT, handled on the next
     * request. */
}

/* 1 only if the controller is up, USB mounted and the unit is REAL. */
unsigned char unit_is_real(unit)
unsigned char unit;
{
    return ctrl_state == STATE_CONTROLLER_OK && usb_state
        && units[unit].status == CH375DISK_UNIT_REAL;
}

/* (status << 8) | mount_gen; a fixed id while there is no controller
 * or no USB disk. */
unsigned int current_content_id(unit)
unsigned char unit;
{
    if (ctrl_state != STATE_CONTROLLER_OK)
        return CH375DISK_UNIT_NO_CONTROLLER << 8;
    if (!usb_state)
        return CH375DISK_UNIT_NO_USB << 8;
    return ((unsigned int) units[unit].status << 8) | units[unit].mount_gen;
}
