/* CH375 primitives (no libc, no long). */

#include "transport.h"
#include "ch375.h"

#define CMD_GET_IC_VER   0x01
#define CMD_CHECK_EXIST  0x06
#define CMD_SET_USB_MODE 0x15
#define CMD_GET_STATUS   0x22
#define CMD_RD_USB_DATA  0x28

/* Bounded wait_int() loop - untuned, not measured on HW. */
#define WAIT_INT_MAX_ITER 60000U

ch375_get_ic_ver()
{
    tr_cmd(CMD_GET_IC_VER);
    return tr_rdata();
}

ch375_check_exist(b)
unsigned char b;
{
    tr_cmd(CMD_CHECK_EXIST);
    tr_wdata(b);
    return tr_rdata();
}

ch375_get_status()
{
    tr_cmd(CMD_GET_STATUS);
    return tr_rdata();
}

/* Waits for INT# after a command marked "Produce interruption" -
 * GET_STATUS before completion returns a stale status. Returns 0 on
 * timeout (chip stopped responding). */
ch375_wait_int()
{
    unsigned int i;

    for (i = 0; i < WAIT_INT_MAX_ITER; i++) {
        if ((tr_token() & 0x80) == 0)
            return 1;
    }
    return 0;
}

ch375_set_usb_mode(mode)
unsigned char mode;
{
    tr_cmd(CMD_SET_USB_MODE);
    tr_wdata(mode);
    return tr_rdata();
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

    tr_cmd(CMD_RD_USB_DATA);
    len = tr_rdata();

    for (i = 0; i < len; i++)
        buf[i] = tr_rdata();

    return len;
}
