/* CH375 low-level driver library for BCC (K&R C).
 * CH375 mapped at I/O 807Dh (command port) / 807Eh (data port).
 */

#include "port.h"
#include "ch375.h"

#define CH375_CMD 0x807D
#define CH375_DAT 0x807E

#define CMD_GET_IC_VER   0x01
#define CMD_CHECK_EXIST  0x06
#define CMD_SET_USB_MODE 0x15
#define CMD_GET_STATUS   0x22
#define CMD_RD_USB_DATA  0x28
#define CMD_DISK_INIT    0x51
#define CMD_DISK_SIZE    0x53
#define CMD_DISK_READ    0x54
#define CMD_DISK_RD_GO   0x55

ch375_write_cmd(cmd)
unsigned char cmd;
{
    port_out(CH375_CMD, cmd);
}

ch375_write_data(b)
unsigned char b;
{
    port_out(CH375_DAT, b);
}

ch375_read_data()
{
    return port_in(CH375_DAT);
}

/* Reads the command port without issuing any command first. Per the
 * datasheet (6.2.1), this returns the "interrupt token" byte where
 * bit 7 mirrors the INT# pin - bits 0-6 are undocumented.
 */
ch375_read_cmd_token()
{
    return port_in(CH375_CMD);
}

ch375_get_ic_ver()
{
    ch375_write_cmd(CMD_GET_IC_VER);
    return ch375_read_data();
}

ch375_check_exist(b)
unsigned char b;
{
    ch375_write_cmd(CMD_CHECK_EXIST);
    ch375_write_data(b);
    return ch375_read_data();
}

ch375_get_status()
{
    ch375_write_cmd(CMD_GET_STATUS);
    return ch375_read_data();
}

/* Poll the command port until bit 7 goes low (INT# active). For use
 * only in front of commands the datasheet marks as "Produce
 * interruption" (DISK_INIT, DISK_SIZE, ...) - the command itself runs
 * asynchronously and GET_STATUS must not be issued before it actually
 * completes, or it just echoes a stale/unrelated status.
 */
ch375_wait_int()
{
    while (ch375_read_cmd_token() & 0x80)
        ;
}

ch375_set_usb_mode(mode)
unsigned char mode;
{
    ch375_write_cmd(CMD_SET_USB_MODE);
    ch375_write_data(mode);
    return ch375_read_data();
}

/* RD_USB_DATA (datasheet 5.13): reads whatever data block is queued
 * in the chip's current interrupt endpoint buffer. First byte read is
 * the block length (0-64), followed by exactly that many data bytes -
 * stores them into buf and returns the length. When debug is nonzero,
 * prints each byte read, since queued blocks left over from a prior
 * command (e.g. DISK_INIT's internal INQUIRY) have been a recurring
 * source of confusion.
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
 * status from GET_STATUS (USB_INT_SUCCESS on success).
 */
ch375_disk_init()
{
    ch375_write_cmd(CMD_DISK_INIT);
    ch375_wait_int();
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
    ch375_wait_int();
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
 * Returns the final completion status (USB_INT_SUCCESS on success).
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
        ch375_wait_int();
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
