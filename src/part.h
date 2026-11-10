/* part.h - MBR partition table / boot sector BPB / FAT12 suitability
 * layer, built on top of lib/pofoch375/ch375disk.h's streaming
 * ch375disk_read_sector() but not chip-specific itself (pure parsing
 * of bytes already read). Split across three units:
 *   - src/part.c (driver-safe): MBR partition table, superfloppy
 *     detection, boot sector BPB, FAT12 suitability check,
 *   - src/partpick.c (CLI only): first-usable-volume pick.
 *
 * No printf, no unsigned-long helpers, no 512-byte sector buffer -
 * src/part.c stays linkable into POFOUDSK.SYS as well as the CLI.
 */

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
 * here into lo/hi unsigned int halves. Field order is load-bearing:
 * two chars then four ints (no BCC padding) in lo,hi order is exactly
 * the entry's bytes 8..15 in memory, so ch375disk_read_mbr() copies
 * them verbatim. */
struct ch375disk_part {
    unsigned char boot_flag; /* 0x00 or 0x80 in a valid entry */
    unsigned char type;
    unsigned int lba_start_lo;
    unsigned int lba_start_hi;
    unsigned int sectors_lo;
    unsigned int sectors_hi;
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

/* Raw DOS 2.x BPB wire layout (boot sector bytes 0x0B..0x17) of the
 * sector last read by ch375disk_read_mbr()/ch375disk_read_bpb() - valid
 * until the next such read. POFOUDSK.SYS copies it verbatim as a
 * unit's BUILD_BPB reply. */
#define CH375DISK_BPB_RAW_LEN 13
extern unsigned char ch375disk_bpb_raw[];

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
extern ch375disk_fat12_check(/* part_type, sectors_hi, sectors_lo, bpb */);

/* Picks the volume to mount: the superfloppy if is_superfloppy
 * (start 0, size = BPB total sectors - the FS may be smaller than the
 * disk, e.g. mkfs.fat -F12 /dev/sdX 32000), otherwise the first valid
 * MBR entry (boot flag 0x00/0x80, non-zero type), preferring type
 * 0x01. Returns the partition type (0 for superfloppy) or
 * CH375DISK_NO_VOLUME. Implemented in src/partpick.c (CLI only - MBR
 * logic, never linked into POFOUDSK.SYS). */
#define CH375DISK_NO_VOLUME 0xFF
extern unsigned char ch375disk_pick_volume(/* mbr, start_hi, start_lo, sectors_hi, sectors_lo */);
