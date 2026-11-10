/* CH375.SYS controller/USB state machine and per-unit mount state -
 * see sys.h.
 *
 * USB media mount/unmount is event-driven, not polled by blocking:
 * guard() reads the CH375 interrupt flag (one cheap IN) on every
 * request while CONTROLLER_OK, and only issues GET_STATUS (and acts
 * on CONNECT/DISCONNECT) when a pending interrupt is seen. MBR parsing
 * and per-partition FAT12 suitability checks come from the shared
 * driver-safe ch375disk.c/ch375part.c layer (also used by CH375.COM).
 */

#include "ch375.h"
#include "ch375disk.h"
#include "sys.h"

/* Delay after USB_INT_CONNECT before DISK_INIT - lets the freshly
 * powered device settle (USB spec allows it 100 ms). Same value
 * ch375det.c's own (CLI) CONNECT_SETTLE_MS uses. */
#define CONNECT_SETTLE_MS 100

/* TEST_CONNECT (datasheet 5.10), see test_connect_resync(). */
#define CMD_TEST_CONNECT 0x16

/* NOTE: .bss is NOT zeroed for a .SYS - DOS loads only the file
 * image, and .bss lies past its end, so every uninitialized static
 * below starts out as leftover memory garbage (observed: ctrl_state
 * read back as 0x5D). init_mount_state() sets them explicitly on
 * INIT, before guard() runs for the first time. */
unsigned char ctrl_state;

/* 1 once DISK_INIT+DISK_SIZE succeeded for the currently connected
 * device, 0 otherwise (no device, or disconnected). */
unsigned char usb_state;

/* Bumped on every mount and unmount - part of the per-unit content id
 * (see current_content_id()), so re-inserting a *different* disk that
 * happens to produce the same per-unit verdict is still detected as a
 * media change. */
unsigned char mount_gen;

/* Vendor/product strings copied from ch375disk_vendor/product (see
 * ch375disk.c drain_inquiry()) at mount time - own copies, since
 * those globals get overwritten by the next ch375disk_init() call. */
char vendor[9];
char product[17];

unsigned int cap_sectors_hi;
unsigned int cap_sectors_lo;

struct unit_state units[MAX_UNITS];

/* Real partition BPB, 13 bytes each (same layout as the virtual bpb[]
 * in sysvdisk.c: bytes 0x0B..0x17 of a FAT boot sector), copied from
 * ch375disk_bpb_raw only for units whose status is
 * CH375DISK_UNIT_REAL. */
unsigned char unit_bpb[MAX_UNITS][13];

/* Resets one unit to status with no geometry, stamped with the current
 * mount_gen - the common starting point for every unit state change. */
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

/* Explicit startup values - see the .bss note at ctrl_state. */
void init_mount_state()
{
    ctrl_state = STATE_NO_CONTROLLER;
    mount_gen = 0;
    clear_usb_info();
    set_all_units(CH375DISK_UNIT_NO_CONTROLLER, 0);
}

/* Drops to NO_CONTROLLER - the single place any CH375 command
 * failure/timeout must call (CH375 lost its state while the PID card
 * stayed present). Callers that also lose the current USB
 * mount (guard(), mount/read paths) additionally call unmount_all();
 * current_content_id() reads ctrl_state live regardless, so the next
 * MEDIA_CHECK automatically compares against the new (NO_CONTROLLER)
 * id and reports "changed" on its own. */
void drop_controller()
{
    ctrl_state = STATE_NO_CONTROLLER;
}

/* Clears all per-unit mount state back to CH375DISK_UNIT_NO_USB and
 * bumps mount_gen - called on USB_INT_DISCONNECT, on any disk-op
 * CH375_TIMEOUT, and when the
 * controller itself is lost (guard()). current_content_id() ignores
 * units[]/mount_gen whenever ctrl_state != CONTROLLER_OK or
 * !usb_state (it reports a fixed id for those cases), so this mainly
 * matters for INFO.TXT/IOCTL cosmetics and for the next real mount to
 * start from a clean state. */
void unmount_all()
{
    clear_usb_info();
    mount_gen++;
    set_all_units(CH375DISK_UNIT_NO_USB, 0);
}

/* Applies a ch375disk_fat12_check() verdict to unit n; on OK the
 * unit's BPB is copied verbatim from ch375disk_bpb_raw, which still
 * holds the sector the caller just read. */
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

/* Evaluates MBR primary partition entry n (0-based, so DOS unit n =
 * "Pn+1") and sets units[n] accordingly. Returns CH375_TIMEOUT if the BPB read itself times
 * out (caller must drop_controller()+unmount_all), else 0. */
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
        return 0; /* UNSUPPORTED, REASON_BAD_TYPE (0) */

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

/* Runs the full mount sequence after USB_INT_CONNECT: settle delay,
 * DISK_INIT, DISK_SIZE, MBR read, then per-partition evaluation (or
 * the superfloppy special case: unit 0 is the whole-disk FAT12 volume,
 * start 0, size = BPB total sectors, units 1-3 EMPTY). Any
 * CH375_TIMEOUT along the way drops the controller entirely. */
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
        /* Disk responded to INIT/SIZE but the MBR read failed, or
         * sector 0 has neither a 0x55AA signature nor a superfloppy
         * boot sector - nothing usable per partition (bad BPB is the
         * closest existing reason code). */
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
    unmount_all(); /* stay NO_USB - not a successful mount */
}

/* Event-driven USB handling - called once per request while
 * CONTROLLER_OK (see guard()). ch375_read_cmd_token() is a plain IN on
 * the command port (no command issued), so this costs nothing when no
 * interrupt is pending; GET_STATUS (and any further CH375 traffic) is
 * only issued once an interrupt is actually seen. */
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
    /* any other code: ignore, same as before */
}

/* TEST_CONNECT (0x16, datasheet 5.10) - no interrupt, finishes in
 * ~2us. Resyncs ctrl_state/usb_state after an event that leaves the
 * CH375 reset (e.g. Portfolio sleep/wake dropping its power) without
 * ever raising a CONNECT/DISCONNECT interrupt (UNVERIFIED on real HW, including
 * USB_INT_USB_READY's 0x18 value). Called from MEDIA_CHECK only (not
 * every request) while CONTROLLER_OK. */
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
        /* else: already the correct "no device" state */
    } else if (result == USB_INT_CONNECT) {
        if (usb_state)
            unmount_all(); /* connected but not initialized = device reset */
        mount_usb(); /* missed plug, or remount after the reset above */
    } else if (result == USB_INT_USB_READY) {
        if (!usb_state)
            mount_usb(); /* missed plug */
        /* else: already the correct "mounted and ready" steady state */
    } else {
        /* unexpected value - the chip is not in host mode any more */
        drop_controller();
        unmount_all();
    }
}

/* Runs on every dispatch() entry. pid_check() (cheap, BIOS-only) runs every time; the
 * expensive CH375 HELLO (CHECK_EXIST) + reinit (SET_USB_MODE 07h then
 * 06h) run only on the NO_CONTROLLER -> PID-present transition.
 * check_usb_event() (one cheap IN, expensive GET_STATUS/mount only if
 * an interrupt is actually pending) runs on every request while
 * CONTROLLER_OK. */
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
    /* USB media/BPB are not re-scanned on this same request - a flash
     * already plugged in will raise its own CONNECT interrupt, picked
     * up by check_usb_event() on the *next* request. */
}

/* Returns 1 only if unit is both reachable (controller present, USB
 * mounted) AND its own verdict is CH375DISK_UNIT_REAL - the single
 * place BUILD_BPB/READ/WRITE consult before handing out real-partition
 * data, so a stale units[] entry left over from a dropped controller
 * never gets used after the fact (ctrl_state/usb_state are checked
 * live). */
unsigned char unit_is_real(unit)
unsigned char unit;
{
    return ctrl_state == STATE_CONTROLLER_OK && usb_state
        && units[unit].status == CH375DISK_UNIT_REAL;
}

/* Current content id for one unit - (status, mount generation) packed
 * into one unsigned int, see ch375sys.c presented_content_id. While
 * the controller is absent or no USB disk is mounted, every unit
 * reports the same fixed id (generation 0) regardless of stale
 * units[] data - never trusts units[] unless both ctrl_state and
 * usb_state agree a real mount is live. */
unsigned int current_content_id(unit)
unsigned char unit;
{
    if (ctrl_state != STATE_CONTROLLER_OK)
        return CH375DISK_UNIT_NO_CONTROLLER << 8;
    if (!usb_state)
        return CH375DISK_UNIT_NO_USB << 8;
    return ((unsigned int) units[unit].status << 8) | units[unit].mount_gen;
}
