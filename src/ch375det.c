/* ch375det.c - CLI-only (CH375.COM) USB media detection and volume
 * picking: blocking, bounded wait for USB_INT_CONNECT + DISK_INIT, and
 * "first usable MBR volume" selection. CH375.SYS never blocks waiting
 * for a device (it reacts to CONNECT/DISCONNECT interrupts instead, see
 * ch375sys.c) and evaluates every partition per unit, so none of this
 * is linked into the driver.
 */

#include "ch375.h"
#include "ch375disk.h"

/* Outer bound on ch375disk_wait_connect()'s polling loop - each
 * iteration itself already waits up to ch375_wait_int()'s own bound,
 * so this just limits how many such bounded waits we retry across
 * spurious/irrelevant interrupt codes before giving up. Untuned, same
 * spirit as ch375core.c's WAIT_INT_MAX_ITER. */
#define WAIT_CONNECT_MAX_POLLS 50U

/* Delay after USB_INT_CONNECT before DISK_INIT - lets the freshly
 * powered device settle (USB spec allows it 100 ms). Precaution, not
 * a HW-confirmed requirement. */
#define CONNECT_SETTLE_MS 100

/* Poll GET_STATUS (via the bounded ch375_wait_int()) until
 * USB_INT_CONNECT or USB_INT_DISCONNECT is seen, retrying across any
 * other/spurious status up to WAIT_CONNECT_MAX_POLLS times. Returns
 * that status, or CH375_TIMEOUT if the outer bound is exhausted or
 * any individual wait_int() times out. */
ch375disk_wait_connect()
{
    unsigned int i;
    int status;

    for (i = 0; i < WAIT_CONNECT_MAX_POLLS; i++) {
        if (!ch375_wait_int())
            return CH375_TIMEOUT;
        status = ch375_get_status();
        if (status == USB_INT_CONNECT || status == USB_INT_DISCONNECT)
            return status;
        /* anything else: keep waiting, bounded by the outer loop */
    }
    return CH375_TIMEOUT;
}

/* USB media detection: wait (bounded) for USB_INT_CONNECT, then run
 * DISK_INIT. Returns USB_INT_DISCONNECT if no device is attached,
 * CH375_TIMEOUT on timeout, or DISK_INIT's own completion status
 * (USB_INT_SUCCESS on success) once a device was seen connecting. */
ch375disk_detect()
{
    int status;

    status = ch375disk_wait_connect();
    if (status == CH375_TIMEOUT)
        return USB_INT_DISCONNECT; /* no connect event in time = no device */
    if (status != USB_INT_CONNECT)
        return status;

    ch375_delay_ms(CONNECT_SETTLE_MS);
    return ch375disk_init();
}

static unsigned char part_valid(p)
struct ch375disk_part *p;
{
    return p->type != 0 && (p->boot_flag == 0x00 || p->boot_flag == 0x80);
}

unsigned char ch375disk_pick_volume(mbr, start_hi, start_lo, sectors_hi, sectors_lo)
struct ch375disk_mbr *mbr;
unsigned int *start_hi;
unsigned int *start_lo;
unsigned int *sectors_hi;
unsigned int *sectors_lo;
{
    int n, found;

    if (mbr->is_superfloppy) {
        *start_hi = 0;
        *start_lo = 0;
        *sectors_hi = 0;
        *sectors_lo = mbr->bpb0.total_sectors;
        return 0;
    }

    if (!mbr->has_mbr_sig)
        return CH375DISK_NO_VOLUME;

    /* First FAT12 entry wins; otherwise the first valid entry, so the
     * caller can still report why it's unusable. */
    found = -1;
    for (n = 0; n < CH375DISK_PART_COUNT; n++) {
        if (!part_valid(&mbr->part[n]))
            continue;
        if (mbr->part[n].type == 0x01) {
            found = n;
            break;
        }
        if (found < 0)
            found = n;
    }
    if (found < 0)
        return CH375DISK_NO_VOLUME;

    *start_hi = mbr->part[found].lba_start_hi;
    *start_lo = mbr->part[found].lba_start_lo;
    *sectors_hi = mbr->part[found].sectors_hi;
    *sectors_lo = mbr->part[found].sectors_lo;
    return mbr->part[found].type;
}
