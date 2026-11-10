/* CH375 built-in mass-storage firmware commands. 32-bit values (LBA,
 * sector counts) are hi/lo unsigned int pairs. */

/* Return USB_INT_* or CH375_TIMEOUT. */
extern ch375disk_wait_connect();
extern ch375disk_init();
extern ch375disk_detect();

extern ch375disk_size();
extern int ch375disk_size_tries;
/* valid after USB_INT_SUCCESS */
extern unsigned int ch375disk_sector_count_hi;
extern unsigned int ch375disk_sector_count_lo;
extern unsigned int ch375disk_sector_size_hi;
extern unsigned int ch375disk_sector_size_lo;

/* from the INQUIRY reply DISK_INIT leaves queued */
extern char ch375disk_vendor[];
extern char ch375disk_product[];

/* Reads one sector, calling handler(offset, chunk, len) per 64-byte
 * chunk. Returns USB_INT_SUCCESS, USB_INT_DISK_ERR or CH375_TIMEOUT. */
extern ch375disk_read_sector(/* lba_hi, lba_lo, handler */);

/* Writes one sector; supplier(offset, 64) must output the length byte
 * and 64 data bytes. Same return values as the read. */
extern ch375disk_write_sector(/* lba_hi, lba_lo, supplier */);

/* SCSI REQUEST SENSE - clears a pending check condition. */
extern ch375disk_request_sense();
