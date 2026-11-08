/* CLI client for the CH375 library (ch375.c/ch375.h) and the MBR/disk
 * layer built on top of it (disk.c/disk.h). A GUI client is planned as
 * a separate front-end reusing the same ch375.c/disk.c library.
 */

#include "ch375.h"
#include "disk.h"

/* Poll the command port in a loop, print the byte whenever it
 * changes. No command is issued for this part - it observes whatever
 * is naturally present on the command port over time, including bits
 * 0-6 the datasheet leaves undocumented.
 *
 * Whenever bit 7 goes low (INT# active, interrupt pending), also
 * issue GET_STATUS and print the resulting status byte, to see what
 * condition actually triggered it.
 */
main()
{
    int cur, prev, status;

    /* CH375 boots into invalid USB-DEVICE mode; USB-HOST mode must be
     * enabled explicitly before plugging in a device triggers any
     * interrupt at all. Bus reset (07h) first, then enable with auto
     * SOF (06h), per datasheet section 5.9.
     */
    printf("SET_USB_MODE reset: %02x\n", ch375_set_usb_mode(MODE_HOST_RESET));
    printf("SET_USB_MODE sof: %02x\n", ch375_set_usb_mode(MODE_HOST_SOF));

    prev = -1;

    for (;;) {
        cur = ch375_read_cmd_token();
        if (cur != prev) {
            printf("CMD port: %02x\n", cur);
            prev = cur;

            if ((cur & 0x80) == 0) {
                status = ch375_get_status();
                printf("  GET_STATUS: %02x\n", status);

                if (status == USB_INT_CONNECT)
                    disk_on_connect();
            }
        }
    }
}
