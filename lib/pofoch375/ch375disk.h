/* ch375disk.h - CH375 built-in mass-storage firmware API (chip-only).
 *
 * Declares the CH375 disk commands implemented in ch375disk.c
 * (DISK_INIT/DISK_SIZE/REQUEST SENSE/streaming sector READ/WRITE) and
 * ch375det.c (blocking connect wait/detect) - all driver-safe (no
 * printf, no `unsigned long`, no 512-byte sector buffer), so this
 * header and its units stay linkable into POFOUDSK.SYS as well as the
 * CLI. MBR/BPB/FAT12 parsing is not chip-specific and lives in
 * src/part.h instead.
 *
 * All 32-bit values (LBA, sector counts) are hi/lo unsigned int pairs,
 * never `unsigned long`.
 */

/* ch375disk_detect()/ch375disk_wait_connect() return one of the
 * ch375.h USB_INT_* codes (USB_INT_CONNECT, USB_INT_DISCONNECT,
 * USB_INT_SUCCESS, USB_INT_DISK_ERR, CH375_TIMEOUT). */
extern ch375disk_wait_connect();
extern ch375disk_init();
extern ch375disk_detect();

extern ch375disk_size();
extern int ch375disk_size_tries; /* attempts the last ch375disk_size() needed */
/* out params, filled on USB_INT_SUCCESS */
extern unsigned int ch375disk_sector_count_hi;
extern unsigned int ch375disk_sector_count_lo;
extern unsigned int ch375disk_sector_size_hi;
extern unsigned int ch375disk_sector_size_lo;

/* Filled by ch375disk_init() from the SCSI INQUIRY reply DISK_INIT
 * leaves queued - see ch375disk.c drain_inquiry(). */
extern char ch375disk_vendor[];
extern char ch375disk_product[];

/* Streaming sector read: issues DISK_READ for one LBA (hi/lo) and
 * drives DISK_RD_GO for each of the (normally 8) 64-byte chunks CH375
 * delivers, calling handler(offset, chunk, len) for each one instead
 * of filling a 512-byte buffer. offset is the chunk's byte offset
 * within the sector (0, 64, 128, ...). Returns the final completion
 * status (USB_INT_SUCCESS) , USB_INT_DISK_ERR, or CH375_TIMEOUT.
 */
extern ch375disk_read_sector(/* lba_hi, lba_lo, handler */);

/* Streaming sector write: issues DISK_WRITE for one LBA (hi/lo), then
 * for each USB_INT_DISK_WRITE interrupt issues WR_USB_DATA7 and calls
 * supplier(offset, 64), which must write the length byte then the 64
 * data bytes to the data port, before DISK_WR_GO. Returns
 * USB_INT_SUCCESS, USB_INT_DISK_ERR, or CH375_TIMEOUT - same
 * conventions as ch375disk_read_sector(). */
extern ch375disk_write_sector(/* lba_hi, lba_lo, supplier */);

/* SCSI REQUEST SENSE (DISK_R_SENSE) - clears a pending check condition
 * (e.g. after a controller reset). Exported so the driver can retry a
 * failed READ/WRITE once after calling this. */
extern ch375disk_request_sense();
