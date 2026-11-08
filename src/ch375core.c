/* CH375 low-level primitives safe to link into the device driver
 * (CH375.SYS) as well as the CLI - no stdio, no unsigned-long BCC
 * runtime helpers. ch375.c builds on top of these for the CLI-only
 * parts (debug dump via printf, long-based disk_size/disk_read).
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

/* Bounded wait_int() loop count - untuned, picked as "clearly more
 * than any observed real CH375 response latency" without measurement
 * on hardware. See ch375driver.md "Timeout v polling smyčkách". */
#define WAIT_INT_MAX_ITER 60000U

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

/* Poll the command port until bit 7 goes low (INT# active), bounded
 * by WAIT_INT_MAX_ITER iterations. For use only in front of commands
 * the datasheet marks as "Produce interruption" (DISK_INIT,
 * DISK_SIZE, ...) - the command itself runs asynchronously and
 * GET_STATUS must not be issued before it actually completes, or it
 * just echoes a stale/unrelated status.
 *
 * Returns 1 if the interrupt was seen, 0 on timeout - a timeout means
 * the chip stopped responding and the caller (driver guard state
 * machine) must treat this as a lost controller, see
 * ch375driver.md "Ztráta stavu CH375 při trvale přítomném PID".
 */
ch375_wait_int()
{
    unsigned int i;

    for (i = 0; i < WAIT_INT_MAX_ITER; i++) {
        if ((ch375_read_cmd_token() & 0x80) == 0)
            return 1;
    }
    return 0;
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
 * stores them into buf and returns the length. Non-printing variant -
 * the CLI's own ch375_rd_usb_data() in ch375.c wraps this with a
 * printf debug dump; the driver can't link printf at all.
 */
ch375_rd_usb_data_q(buf)
unsigned char *buf;
{
    int len, i;

    ch375_write_cmd(CMD_RD_USB_DATA);
    len = ch375_read_data();

    for (i = 0; i < len; i++)
        buf[i] = ch375_read_data();

    return len;
}
