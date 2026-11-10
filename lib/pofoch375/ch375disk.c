/* CH375 disk commands: DISK_INIT, DISK_SIZE, REQUEST SENSE, single-sector
 * READ/WRITE streamed in 64-byte chunks (no sector buffer). */

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

#define DISK_SIZE_TRIES    5
#define DISK_SIZE_RETRY_MS 100

/* vendor/product from INQUIRY, empty if none was seen */
char ch375disk_vendor[9];
char ch375disk_product[17];

/* Upper bound on RD_USB_DATA blocks drained after DISK_INIT. */
#define DRAIN_MAX_BLOCKS 32

/* DISK_INIT/DISK_SIZE may report success as CMD_RET_SUCCESS (51h)
 * (observed on HW). */
static int norm_status(s)
int s;
{
    return (s == CMD_RET_SUCCESS) ? USB_INT_SUCCESS : s;
}

/* DISK_INIT leaves its INQUIRY reply queued - unless drained, the next
 * RD_USB_DATA returns it (observed on HW). Keeps vendor/product. */
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

/* DISK_INIT, datasheet 5.15 */
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

/* DISK_R_SENSE (5Ah): clears a pending check condition, discards the
 * sense data. */
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

int ch375disk_size_tries;

static int disk_size_once();

/* DISK_SIZE (5.16). The first one after DISK_INIT fails on some disks
 * (Alcor 058f:6387), retried after REQUEST SENSE. Big-endian last LBA +
 * sector size -> hi/lo sector count. */
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

/* DISK_READ (5.22) + DISK_RD_GO (5.18), chunk by chunk to handler. */
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

/* DISK_WRITE (5.19) + WR_USB_DATA7 (5.14) + DISK_WR_GO (5.20), chunk
 * by chunk from supplier. */
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
