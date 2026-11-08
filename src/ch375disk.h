/* ch375disk.c - driver-safe disk-level layer on top of ch375core.c's
 * CH375 primitives: USB media detect, DISK_SIZE, streaming sector
 * read, MBR partition table parsing, superfloppy detection, boot
 * sector BPB extraction and FAT12 suitability check.
 *
 * All 32-bit values (LBA, sector counts) are hi/lo unsigned int pairs,
 * never `unsigned long` - same convention as
 * src/ch375sys_version.h's BUILD_ID_HI/LO. No printf, no 512-byte
 * sector buffer - this file must stay linkable into CH375.SYS as well
 * as CH375.COM. Only CH375.COM links it in for now (see Makefile);
 * wiring it into CH375.SYS is a later phase.
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

/* DOS 2.x BPB, extracted from boot sector bytes 0x0B..0x17 (13
 * bytes). */
struct ch375disk_bpb {
    unsigned int bytes_per_sector;
    unsigned char sectors_per_cluster;
    unsigned int reserved_sectors;
    unsigned char fat_count;
    unsigned int root_entries;
    unsigned int total_sectors;
    unsigned char media_descriptor;
    unsigned int sectors_per_fat;
};

/* One MBR partition table entry (16 bytes at 0x1BE + n*16). LBA start
 * and sector count are 32-bit little-endian fields in the MBR, split
 * here into hi/lo unsigned int halves. */
struct ch375disk_part {
    unsigned char boot_flag; /* 0x00 or 0x80 in a valid entry */
    unsigned char type;
    unsigned int lba_start_hi;
    unsigned int lba_start_lo;
    unsigned int sectors_hi;
    unsigned int sectors_lo;
};

#define CH375DISK_PART_COUNT 4

struct ch375disk_mbr {
    struct ch375disk_part part[CH375DISK_PART_COUNT];
    unsigned char has_mbr_sig;    /* 1 if 0x55AA found at offset 510/511 */
    /* 1 if sector 0 is itself a FAT boot sector (no MBR): jump opcode
     * 0xEB/0xE9 at byte 0 AND a sane BPB with non-zero total sectors
     * that fits the disk. 0x55AA alone can't tell MBR from boot
     * sector - both end with it. Takes precedence over the partition
     * table, whose bytes are boot code in that case. */
    unsigned char is_superfloppy;
    struct ch375disk_bpb bpb0;    /* BPB as read from sector 0 */
};

/* Reads LBA (hi/lo) and fills *mbr from the MBR partition table and
 * signature bytes. Returns the streaming read's completion status;
 * *mbr is only valid on USB_INT_SUCCESS. */
extern ch375disk_read_mbr(/* lba_hi, lba_lo, mbr */);


/* Reads LBA (hi/lo) (a partition's first sector) and fills *bpb from
 * bytes 0x0B..0x17, and *boot_jump from byte 0 (for superfloppy
 * detection). Returns the streaming read's completion status. */
extern ch375disk_read_bpb(/* lba_hi, lba_lo, bpb, boot_jump */);

/* 1 if bpb looks like a sane DOS 2.x BPB (bytes/sector 512,
 * sectors/cluster a non-zero power of 2, FAT count non-zero), 0
 * otherwise. */
extern ch375disk_bpb_sane(/* bpb */);

/* FAT12 suitability verdict codes. */
#define CH375DISK_FAT12_OK         0
#define CH375DISK_FAT12_BAD_TYPE   1 /* MBR partition type not 0x01/FAT12 */
#define CH375DISK_FAT12_BAD_BPB    2 /* boot sector BPB fails sanity check */
#define CH375DISK_FAT12_TOO_BIG    3 /* total sectors don't fit 16 bits (>32MB @512B/sector) */

/* part_type: MBR partition type byte, or 0 for a superfloppy (no MBR
 * partition entry to check). sectors_hi/lo: partition (or, for a
 * superfloppy, BPB total) sector count. Returns one of the CH375DISK_FAT12_*
 * codes above. */
/* Picks the volume to mount: the superfloppy if is_superfloppy
 * (start 0, size = BPB total sectors - the FS may be smaller than the
 * disk, e.g. mkfs.fat -F12 /dev/sdX 32000), otherwise the first valid
 * MBR entry (boot flag 0x00/0x80, non-zero type), preferring type
 * 0x01. Returns the partition type (0 for superfloppy) or
 * CH375DISK_NO_VOLUME. */
#define CH375DISK_NO_VOLUME 0xFF
extern unsigned char ch375disk_pick_volume(/* mbr, start_hi, start_lo, sectors_hi, sectors_lo */);

extern ch375disk_fat12_check(/* part_type, sectors_hi, sectors_lo, bpb */);

/* Per-unit mount status codes - shared by CH375.SYS (src/ch375sys.c,
 * per-unit state array) and the CLI (src/ch375cli.c, IOCTL v1 reply
 * parsing) so both sides agree on the wire values without duplicating
 * the enum. See ch375driver.md "IOCTL v1" / "USB event handling". */
#define CH375DISK_UNIT_NO_CONTROLLER 0
#define CH375DISK_UNIT_NO_USB        1
#define CH375DISK_UNIT_REAL          2
#define CH375DISK_UNIT_EMPTY         3
#define CH375DISK_UNIT_UNSUPPORTED   4
#define CH375DISK_UNIT_GPT           5

/* CH375DISK_UNIT_UNSUPPORTED reason byte. */
#define CH375DISK_UNIT_REASON_BAD_TYPE 0
#define CH375DISK_UNIT_REASON_TOO_BIG  1
#define CH375DISK_UNIT_REASON_BAD_BPB  2
