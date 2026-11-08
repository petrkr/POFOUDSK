/* CH375 CLI-only layer: debug dump via printf and the unsigned-long
 * based disk_size()/disk_read() - neither can be linked into the
 * device driver (no stdio, no BCC unsigned-long runtime helpers), so
 * they live here rather than in ch375core.c. The driver-safe
 * primitives (write_cmd, write_data, read_data, get_ic_ver,
 * check_exist, get_status, wait_int, set_usb_mode, rd_usb_data_q) are
 * in ch375core.c and used directly from here too - no duplication.
 */

#include "port.h"
#include "ch375.h"

#define CMD_RD_USB_DATA  0x28
#define CMD_DISK_INIT    0x51
#define CMD_DISK_SIZE    0x53
#define CMD_DISK_READ    0x54
#define CMD_DISK_RD_GO   0x55

/* RD_USB_DATA (datasheet 5.13) with an optional per-byte debug dump -
 * CLI-only wrapper around the same protocol as ch375_rd_usb_data_q()
 * in ch375core.c, duplicated here (not delegated) only because this
 * version also prints via printf, which the core module must stay
 * free of. Queued blocks left over from a prior command (e.g.
 * DISK_INIT's internal INQUIRY) have been a recurring source of
 * confusion, hence the debug option.
 */
ch375_rd_usb_data(buf, debug)
unsigned char *buf;
unsigned char debug;
{
    int len, i;

    ch375_write_cmd(CMD_RD_USB_DATA);
    len = ch375_read_data();

    printf("RD_USB_DATA len=%02x:", len);
    for (i = 0; i < len; i++) {
        buf[i] = ch375_read_data();
        if (debug) {
            printf(" %02x", buf[i]);
        }
    }

    printf("\n");

    return len;
}

/* DISK_INIT (datasheet 5.15): resets the USB bus, configures the
 * attached storage device, and establishes the connection. Must be
 * issued (and succeed) before DISK_SIZE or any other storage command -
 * without it the chip has no storage device session to act on and
 * DISK_SIZE fails with USB_INT_DISK_ERR. Returns the completion
 * status from GET_STATUS (USB_INT_SUCCESS on success), or
 * USB_INT_DISK_ERR if wait_int() timed out.
 */
ch375_disk_init()
{
    ch375_write_cmd(CMD_DISK_INIT);
    if (!ch375_wait_int())
        return USB_INT_DISK_ERR;
    return ch375_get_status();
}

/* DISK_SIZE (datasheet 5.16): issue the command, wait for completion
 * via GET_STATUS, then read the 8-byte result (4-byte sector count +
 * 4-byte sector size) via RD_USB_DATA (5.13) - whose own first byte
 * is the payload length, here always expected to be 8.
 *
 * Both 4-byte fields are big-endian (high byte first) - confirmed on
 * real hardware: a 64512-sector/512-byte-sector USB flash disk
 * returned bytes "00 00 fb ff 00 00 02 00", which only decodes
 * correctly as big-endian (0x0000FBFF, 0x00000200), not the
 * little-endian order the datasheet's wording seemed to suggest at
 * first read.
 *
 * sector_count/sector_size are filled in on success (status ==
 * USB_INT_SUCCESS); the completion status is returned either way.
 */
unsigned long sector_count;
unsigned long sector_size;

ch375_disk_size()
{
    int status;
    unsigned char buf[8];

    ch375_write_cmd(CMD_DISK_SIZE);
    if (!ch375_wait_int())
        return USB_INT_DISK_ERR;
    status = ch375_get_status();
    if (status != USB_INT_SUCCESS)
        return status;

    if (ch375_rd_usb_data(buf, 1) != 8) { /* expected length 8 */
        return status;
    }
    /* Measured off by one against fdisk/dmesg on every tested flash
     * disk - the raw value is the last valid LBA, not a sector count.
     */
    sector_count = 1 + (((unsigned long) buf[0] << 24) | ((unsigned long) buf[1] << 16)
                 | ((unsigned long) buf[2] << 8) | buf[3]);
    sector_size = ((unsigned long) buf[4] << 24) | ((unsigned long) buf[5] << 16)
                | ((unsigned long) buf[6] << 8) | buf[7];

    return status;
}

/* DISK_READ (datasheet 5.22) + DISK_RD_GO (5.18): read one 512-byte
 * sector at the given LBA into buf (caller must provide >= 512 bytes).
 *
 * DISK_READ's input is 5 bytes: LBA address (lowest, lower, upper,
 * highest byte, i.e. little-endian) followed by the sector count (1
 * byte, here always 1).
 *
 * Per 5.18, the transfer then proceeds as a series of interrupts: each
 * one signals either USB_INT_DISK_READ (a 64-byte chunk is ready via
 * RD_USB_DATA, then send DISK_RD_GO to request the next chunk) or
 * USB_INT_SUCCESS (whole read done, no more data) or USB_INT_DISK_ERR
 * (failure). For a 512-byte sector this is documented as 8 data
 * interrupts followed by 1 final one (9 total).
 *
 * Returns the final completion status (USB_INT_SUCCESS on success),
 * or USB_INT_DISK_ERR if wait_int() ever times out.
 */
ch375_disk_read(lba, buf)
unsigned long lba;
unsigned char *buf;
{
    int status, offset, len, i;
    unsigned char chunk[64];

    ch375_write_cmd(CMD_DISK_READ);
    ch375_write_data((unsigned char) lba);
    ch375_write_data((unsigned char) (lba >> 8));
    ch375_write_data((unsigned char) (lba >> 16));
    ch375_write_data((unsigned char) (lba >> 24));
    ch375_write_data(1); /* sector count */

    offset = 0;

    for (;;) {
        if (!ch375_wait_int())
            return USB_INT_DISK_ERR;
        status = ch375_get_status();

        if (status == USB_INT_DISK_READ) {
            len = ch375_rd_usb_data(chunk, 0);
            for (i = 0; i < len && offset < 512; i++)
                buf[offset++] = chunk[i];
            ch375_write_cmd(CMD_DISK_RD_GO);
            continue;
        }

        return status; /* USB_INT_SUCCESS or USB_INT_DISK_ERR */
    }
}
