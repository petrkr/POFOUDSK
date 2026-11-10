/* ch375disk.c - driver-safe disk I/O layer, see ch375disk.h for the
 * module overview: DISK_INIT (+ INQUIRY drain), DISK_SIZE, REQUEST
 * SENSE and streaming single-sector READ/WRITE. Builds on top of
 * ch375core.c's primitives - never printf, never BCC's unsigned-long
 * runtime helpers, never a 512-byte sector buffer, so this file stays
 * linkable into POFOUDSK.SYS as well as POFOUDSK.COM. MBR/BPB parsing lives
 * in ch375part.c, CLI-only blocking detect in ch375det.c.
 */

#include "port.h"
#include "ch375.h"
#include "ch375disk.h"

#define CMD_DISK_INIT  0x51
#define CMD_DISK_SIZE  0x53
#define CMD_DISK_READ  0x54
#define CMD_DISK_RD_GO 0x55
#define CMD_DISK_WRITE 0x56
#define CMD_DISK_WR_GO 0x57
#define CMD_WR_USB_DATA7 0x2B
#define CMD_DISK_R_SENSE 0x5A

/* DISK_SIZE attempts and pause between them - see ch375disk_size(). */
#define DISK_SIZE_TRIES    5
#define DISK_SIZE_RETRY_MS 100

/* Vendor/product strings from the SCSI INQUIRY reply DISK_INIT
 * leaves queued (see ch375disk_init()), NUL-terminated, trailing
 * spaces kept. Empty if no INQUIRY block was seen. */
char ch375disk_vendor[9];
char ch375disk_product[17];

/* Upper bound on RD_USB_DATA blocks drained after DISK_INIT. */
#define DRAIN_MAX_BLOCKS 32

/* Observed on HW: DISK_INIT/DISK_SIZE may report success as
 * CMD_RET_SUCCESS (51h) instead of USB_INT_SUCCESS (14h). Normalize
 * so callers only check USB_INT_SUCCESS. */
static int norm_status(s)
int s;
{
    return (s == CMD_RET_SUCCESS) ? USB_INT_SUCCESS : s;
}

/* Observed on HW: DISK_INIT internally runs a SCSI INQUIRY and leaves
 * its 36-byte reply queued in the chip's read buffer. Unless drained, RD_USB_DATA after the next command (e.g.
 * DISK_SIZE) returns these stale bytes instead of its own data. Drain
 * until an empty block, keeping vendor (bytes 8-15) and product
 * (16-31) from the first block that holds them. */
static void drain_inquiry()
{
    unsigned char buf[64];
    int len, n, i, got;

    ch375disk_vendor[0] = 0;
    ch375disk_product[0] = 0;
    got = 0;

    for (n = 0; n < DRAIN_MAX_BLOCKS; n++) {
        len = ch375_rd_usb_data_q(buf);
        if (len == 0)
            break;
        if (!got && len >= 32) {
            for (i = 0; i < 8; i++)
                ch375disk_vendor[i] = buf[8 + i];
            ch375disk_vendor[8] = 0;
            for (i = 0; i < 16; i++)
                ch375disk_product[i] = buf[16 + i];
            ch375disk_product[16] = 0;
            got = 1;
        }
    }
}

unsigned int ch375disk_sector_count_hi;
unsigned int ch375disk_sector_count_lo;
unsigned int ch375disk_sector_size_hi;
unsigned int ch375disk_sector_size_lo;

/* DISK_INIT (datasheet 5.15). */
ch375disk_init()
{
    int status;

    ch375_write_cmd(CMD_DISK_INIT);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    if (status == USB_INT_SUCCESS)
        drain_inquiry();
    return status;
}

/* DISK_SIZE (datasheet 5.16) - result kept as hi/lo unsigned int pairs
 * (ch375disk_sector_count_hi/lo, ch375disk_sector_size_hi/lo) instead
 * of `unsigned long`, so this stays safe to link into the driver.
 * Both chip fields are big-endian 4-byte values; the raw sector count
 * field is the last valid LBA, so it is incremented by one here (with
 * hi/lo carry) to get the actual sector count. */
/* DISK_R_SENSE (5Ah): SCSI REQUEST SENSE - clears a pending check
 * condition (e.g. Unit Attention after reset) so the next command can
 * succeed. Its sense data, if queued, is drained and discarded.
 * Exported (not static) so the driver can call it between a failed
 * READ/WRITE and its one retry. */
ch375disk_request_sense()
{
    unsigned char buf[64];
    int status, n;

    ch375_write_cmd(CMD_DISK_R_SENSE);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    for (n = 0; n < DRAIN_MAX_BLOCKS; n++)
        if (ch375_rd_usb_data_q(buf) == 0)
            break;
    return status;
}

/* Number of DISK_SIZE attempts the last ch375disk_size() call needed
 * (diagnostics only). */
int ch375disk_size_tries;

static int disk_size_once();

/* Observed on HW (Alcor 058f:6387): the first DISK_SIZE after
 * DISK_INIT fails with USB_INT_DISK_ERR - consistent with a SCSI Unit
 * Attention pending after reset (not confirmed). Retry with a REQUEST
 * SENSE in between. */
ch375disk_size()
{
    int status;

    for (ch375disk_size_tries = 1; ; ch375disk_size_tries++) {
        status = disk_size_once();
        if (status != USB_INT_DISK_ERR || ch375disk_size_tries >= DISK_SIZE_TRIES)
            return status;
        ch375disk_request_sense();
        ch375_delay_ms(DISK_SIZE_RETRY_MS);
    }
}

static int disk_size_once()
{
    int status;
    unsigned char buf[8];
    unsigned int hi, lo;

    ch375_write_cmd(CMD_DISK_SIZE);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    if (status != USB_INT_SUCCESS)
        return status;

    if (ch375_rd_usb_data_q(buf) != 8) /* expected length 8 */
        return status;

    hi = ((unsigned int) buf[0] << 8) | buf[1];
    lo = ((unsigned int) buf[2] << 8) | buf[3];
    lo++;
    if (lo == 0)
        hi++; /* carry out of the low half */
    ch375disk_sector_count_hi = hi;
    ch375disk_sector_count_lo = lo;

    ch375disk_sector_size_hi = ((unsigned int) buf[4] << 8) | buf[5];
    ch375disk_sector_size_lo = ((unsigned int) buf[6] << 8) | buf[7];

    return status;
}

/* DISK_READ (datasheet 5.22) + DISK_RD_GO (5.18), streaming variant:
 * LBA is an hi/lo unsigned int pair (not `unsigned long`), and instead
 * of filling a 512-byte buffer, each 64-byte chunk is handed to the
 * caller-supplied handler as it arrives: handler(offset, chunk, len).
 * No buffer larger than 64 bytes is ever kept here. */
ch375disk_read_sector(lba_hi, lba_lo, handler)
unsigned int lba_hi;
unsigned int lba_lo;
int (*handler)();
{
    int status, len;
    unsigned int offset;
    unsigned char chunk[64];

    ch375_write_cmd(CMD_DISK_READ);
    ch375_write_data((unsigned char) lba_lo);
    ch375_write_data((unsigned char) (lba_lo >> 8));
    ch375_write_data((unsigned char) lba_hi);
    ch375_write_data((unsigned char) (lba_hi >> 8));
    ch375_write_data(1); /* sector count */

    offset = 0;

    for (;;) {
        if (!ch375_wait_int())
            return CH375_TIMEOUT;
        status = ch375_get_status();

        if (status == USB_INT_DISK_READ) {
            len = ch375_rd_usb_data_q(chunk);
            (*handler)(offset, chunk, len);
            offset += len;
            ch375_write_cmd(CMD_DISK_RD_GO);
            continue;
        }

        return norm_status(status); /* USB_INT_SUCCESS or USB_INT_DISK_ERR */
    }
}

/* DISK_WRITE (datasheet 5.19) + DISK_WR_GO (5.20), streaming variant -
 * mirrors ch375disk_read_sector() in the opposite direction. On each
 * USB_INT_DISK_WRITE, WR_USB_DATA7 (5.14) is issued and the
 * caller-supplied supplier(offset, 64) must write the length byte (64)
 * followed by 64 data bytes to the data port (ch375_write_data() then
 * the far-memory port loop) - no buffer kept here either. */
ch375disk_write_sector(lba_hi, lba_lo, supplier)
unsigned int lba_hi;
unsigned int lba_lo;
int (*supplier)();
{
    int status;
    unsigned int offset;

    ch375_write_cmd(CMD_DISK_WRITE);
    ch375_write_data((unsigned char) lba_lo);
    ch375_write_data((unsigned char) (lba_lo >> 8));
    ch375_write_data((unsigned char) lba_hi);
    ch375_write_data((unsigned char) (lba_hi >> 8));
    ch375_write_data(1); /* sector count */

    offset = 0;

    for (;;) {
        if (!ch375_wait_int())
            return CH375_TIMEOUT;
        status = ch375_get_status();

        if (status == USB_INT_DISK_WRITE) {
            ch375_write_cmd(CMD_WR_USB_DATA7);
            (*supplier)(offset, 64);
            ch375_write_cmd(CMD_DISK_WR_GO);
            offset += 64;
            continue;
        }

        return norm_status(status); /* USB_INT_SUCCESS or USB_INT_DISK_ERR */
    }
}
