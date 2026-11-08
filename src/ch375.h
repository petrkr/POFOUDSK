/* CH375 low-level driver library for BCC (K&R C).
 * CH375 mapped at I/O 807Dh (command port) / 807Eh (data port).
 */

/* SET_USB_MODE mode codes, datasheet section 5.9 */
#define MODE_DEVICE_INVALID  0x00 /* default after power-up/reset */
#define MODE_DEVICE_EXT      0x01
#define MODE_DEVICE_INNER    0x02
#define MODE_HOST_INVALID    0x04
#define MODE_HOST_NO_SOF     0x05
#define MODE_HOST_SOF        0x06
#define MODE_HOST_RESET      0x07

/* Command operation status, datasheet "State code" table */
#define CMD_RET_SUCCESS      0x51
#define CMD_RET_ABORT        0x5F

/* Interrupt status codes, datasheet section 5.12 */
#define USB_INT_SUCCESS     0x14
#define USB_INT_CONNECT     0x15
#define USB_INT_DISCONNECT  0x16
#define USB_INT_DISK_READ   0x1D
#define USB_INT_DISK_ERR    0x1F

extern unsigned long sector_count;
extern unsigned long sector_size;

extern ch375_write_cmd();
extern ch375_write_data();
extern ch375_read_data();
extern ch375_read_cmd_token();
extern ch375_get_ic_ver();
extern ch375_check_exist();
extern ch375_get_status();
extern ch375_wait_int();
extern ch375_set_usb_mode();
extern ch375_rd_usb_data();
extern ch375_rd_usb_data_q();
extern ch375_disk_init();
extern ch375_disk_size();
extern ch375_disk_read();
