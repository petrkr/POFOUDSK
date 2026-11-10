/* Blocking USB disk detection (CLI only - the driver reacts to
 * interrupts instead). */

#include "ch375.h"
#include "ch375disk.h"

/* max wait_int() rounds while waiting for CONNECT - untuned */
#define WAIT_CONNECT_MAX_POLLS 50U

/* settle time after CONNECT */
#define CONNECT_SETTLE_MS 100

/* Waits for CONNECT or DISCONNECT; CH375_TIMEOUT otherwise. */
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
    }
    return CH375_TIMEOUT;
}

/* Waits for a device, then DISK_INIT. USB_INT_DISCONNECT if none. */
ch375disk_detect()
{
    int status;

    status = ch375disk_wait_connect();
    if (status == CH375_TIMEOUT)
        return USB_INT_DISCONNECT;
    if (status != USB_INT_CONNECT)
        return status;

    ch375_delay_ms(CONNECT_SETTLE_MS);
    return ch375disk_init();
}
