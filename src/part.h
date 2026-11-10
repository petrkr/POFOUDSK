/* MBR / BPB / FAT12 layer (part.c; partpick.c is CLI only). */

/* DOS 2.x BPB, boot sector bytes 0x0B..0x17 */
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

/* MBR partition entry. Field order matters: two chars then four ints
 * in lo,hi order (no BCC padding) is exactly entry bytes 8..15, which
 * ch375disk_read_mbr() copies verbatim. */
struct ch375disk_part {
    unsigned char boot_flag;
    unsigned char type;
    unsigned int lba_start_lo;
    unsigned int lba_start_hi;
    unsigned int sectors_lo;
    unsigned int sectors_hi;
};

#define CH375DISK_PART_COUNT 4

struct ch375disk_mbr {
    struct ch375disk_part part[CH375DISK_PART_COUNT];
    unsigned char has_mbr_sig;    /* 0x55AA at 510 */
    /* sector 0 is a FAT boot sector (no MBR) */
    unsigned char is_superfloppy;
    struct ch375disk_bpb bpb0;
};

/* Raw BPB of the sector last read - valid until the next read. */
#define CH375DISK_BPB_RAW_LEN 13
extern unsigned char ch375disk_bpb_raw[];

/* Returns the read status; *mbr valid only on USB_INT_SUCCESS. */
extern ch375disk_read_mbr(/* lba_hi, lba_lo, mbr */);

/* Reads a partition's boot sector BPB and byte 0. */
extern ch375disk_read_bpb(/* lba_hi, lba_lo, bpb, boot_jump */);

extern ch375disk_bpb_sane(/* bpb */);

#define CH375DISK_FAT12_OK         0
#define CH375DISK_FAT12_BAD_TYPE   1 /* MBR type not 01h */
#define CH375DISK_FAT12_BAD_BPB    2 /* BPB fails ch375disk_bpb_sane() */
#define CH375DISK_FAT12_TOO_BIG    3 /* more than 65535 sectors */

/* part_type 0 = superfloppy; sectors = partition (or BPB) size. */
extern ch375disk_fat12_check(/* part_type, sectors_hi, sectors_lo, bpb */);

/* CLI only (partpick.c): superfloppy, else the first FAT12 entry, else
 * the first valid one. Returns the type or CH375DISK_NO_VOLUME. */
#define CH375DISK_NO_VOLUME 0xFF
extern unsigned char ch375disk_pick_volume(/* mbr, start_hi, start_lo, sectors_hi, sectors_lo */);
