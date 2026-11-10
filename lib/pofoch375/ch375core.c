/* CH375 primitives (no libc, no long). */

#include "port.h"
#include "ch375.h"

#define CH375_CMD 0x807D
#define CH375_DAT 0x807E

#define CMD_GET_IC_VER   0x01
#define CMD_CHECK_EXIST  0x06
#define CMD_SET_USB_MODE 0x15
#define CMD_GET_STATUS   0x22
#define CMD_RD_USB_DATA  0x28

/* Bounded wait_int() loop - untuned, not measured on HW. */
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

/* Command port read without a command: bit 7 = INT# (datasheet 6.2.1). */
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

/* Waits for INT# after a command marked "Produce interruption" -
 * GET_STATUS before completion returns a stale status. Returns 0 on
 * timeout (chip stopped responding). */
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

/* Busy wait, ~ms milliseconds. DELAY_LOOPS_PER_MS is a guess for the
 * 4.9 MHz 80C88, not measured. */
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

/* Bus reset duration (mode 07h -> 06h); 50 ms is a guess. */
#define HOST_RESET_MS 50

/* Datasheet 5.9: mode 7, then mode 6. Returns 1 on success. */
ch375_host_reset()
{
    if (ch375_set_usb_mode(MODE_HOST_RESET) != CMD_RET_SUCCESS)
        return 0;
    ch375_delay_ms(HOST_RESET_MS);
    if (ch375_set_usb_mode(MODE_HOST_SOF) != CMD_RET_SUCCESS)
        return 0;
    return 1;
}

/* RD_USB_DATA (5.13): length byte + data into buf; returns length. */
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
