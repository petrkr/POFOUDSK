/* CH375 low-level primitives safe to link into the device driver
 * (POFOUDSK.SYS) as well as the CLI - no stdio, no unsigned-long BCC
 * runtime helpers. ch375disk.c builds the driver-safe disk-level
 * layer (DISK_SIZE, streaming sector read, MBR/BPB parsing) on top of
 * these; disk.c then adds CLI-only printf/unsigned-long presentation
 * on top of that. CH375 mapped at I/O 807Dh (command port) / 807Eh
 * (data port).
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
 * on hardware. */
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
 * machine) must treat this as a lost controller (CH375 lost its state
 * while the PID card stayed present).
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

/* Busy-wait roughly ms milliseconds. UNTUNED: DELAY_LOOPS_PER_MS is a
 * guess for the 4.9 MHz 80C88, erring on the long side - measure on
 * HW before relying on exact timing. */
#define DELAY_LOOPS_PER_MS 200U

ch375_delay_ms(ms)
unsigned int ms;
{
    unsigned int i;
    unsigned char sink;

    while (ms-- != 0) {
        for (i = 0; i < DELAY_LOOPS_PER_MS; i++)
            sink = (unsigned char) i; /* keep the loop from being trivial */
    }
}

/* Delay between bus reset (mode 07h) and enable (mode 06h) - the
 * reset must actually last on the bus. Needed on HW; 50 ms is an
 * unverified guess, not measured. */
#define HOST_RESET_MS 50

/* Datasheet 5.9: "enter mode 7 first and then switch to mode 6".
 * Returns 1 if both mode switches reported CMD_RET_SUCCESS. */
ch375_host_reset()
{
    if (ch375_set_usb_mode(MODE_HOST_RESET) != CMD_RET_SUCCESS)
        return 0;
    ch375_delay_ms(HOST_RESET_MS);
    if (ch375_set_usb_mode(MODE_HOST_SOF) != CMD_RET_SUCCESS)
        return 0;
    return 1;
}

/* RD_USB_DATA (datasheet 5.13): reads whatever data block is queued
 * in the chip's current interrupt endpoint buffer. First byte read is
 * the block length (0-64), followed by exactly that many data bytes -
 * stores them into buf and returns the length. Non-printing - used
 * directly by ch375disk.c's driver-safe layer (DISK_SIZE, streaming
 * sector read); the driver can't link printf at all.
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
