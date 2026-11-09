/* ch375part.c - driver-safe MBR partition table / boot sector BPB
 * parsing and FAT12 suitability check, built on ch375disk.c's
 * streaming ch375disk_read_sector(). Same rules as ch375disk.c: no
 * printf, no unsigned-long helpers, no 512-byte sector buffer - only
 * the few bytes actually needed are captured out of the 64-byte
 * chunks as they stream past. Linked into both CH375.SYS and
 * CH375.COM.
 */

#include "ch375.h"
#include "ch375disk.h"

/* Capture windows, filled by cap_chunk_handler() on every
 * ch375disk_read_mbr()/ch375disk_read_bpb() read:
 *   - byte 0 (boot/jump byte, superfloppy check),
 *   - BPB bytes 0x0B..0x17 (13 bytes, DOS 2.x BPB wire layout -
 *     exported as ch375disk_bpb_raw so the driver can hand it to DOS
 *     verbatim, no pack/unpack),
 *   - MBR partition table + signature, bytes 446 (0x1BE)..511.
 * One handler for both reads - on a BPB read the MBR window just gets
 * overwritten with that sector's bytes, which nothing reads. */
#define MBR_CAP_START 446
#define MBR_CAP_LEN   66
static unsigned char mbr_cap[MBR_CAP_LEN];
static unsigned char cap_byte0;

#define BPB_CAP_START 0x0B
unsigned char ch375disk_bpb_raw[CH375DISK_BPB_RAW_LEN];

/* Little-endian 16-bit value at r. */
static unsigned int get16(r)
unsigned char *r;
{
    return ((unsigned int) r[1] << 8) | r[0];
}

static void parse_bpb(bpb)
struct ch375disk_bpb *bpb;
{
    unsigned char *r;

    r = ch375disk_bpb_raw;
    bpb->bytes_per_sector    = get16(r + 0);
    bpb->sectors_per_cluster = r[2];
    bpb->reserved_sectors    = get16(r + 3);
    bpb->fat_count           = r[5];
    bpb->root_entries        = get16(r + 6);
    bpb->total_sectors       = get16(r + 8);
    bpb->media_descriptor    = r[10];
    bpb->sectors_per_fat     = get16(r + 11);
}

static cap_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    unsigned int i, pos;

    for (i = 0; i < (unsigned int) len; i++) {
        pos = offset + i;
        if (pos == 0)
            cap_byte0 = chunk[i];
        else if (pos >= BPB_CAP_START && pos < BPB_CAP_START + CH375DISK_BPB_RAW_LEN)
            ch375disk_bpb_raw[pos - BPB_CAP_START] = chunk[i];
        else if (pos >= MBR_CAP_START)
            mbr_cap[pos - MBR_CAP_START] = chunk[i];
    }
}

ch375disk_read_mbr(lba_hi, lba_lo, mbr)
unsigned int lba_hi;
unsigned int lba_lo;
struct ch375disk_mbr *mbr;
{
    int status, n, i;
    unsigned char *entry, *dst;
    struct ch375disk_part *p;

    status = ch375disk_read_sector(lba_hi, lba_lo, cap_chunk_handler);
    if (status != USB_INT_SUCCESS)
        return status;

    mbr->has_mbr_sig = (mbr_cap[64] == 0x55 && mbr_cap[65] == 0xAA);
    parse_bpb(&mbr->bpb0);

    /* Superfloppy (sector 0 IS the FAT boot sector): jump opcode
     * 0xEB/0xE9 plus a sane BPB whose size fits the disk (needs
     * ch375disk_size() called first). MBR code typically starts with
     * something else (e.g. 0x33, 0xFA) and has no BPB. */
    mbr->is_superfloppy = (cap_byte0 == 0xEB || cap_byte0 == 0xE9)
        && ch375disk_bpb_sane(&mbr->bpb0)
        && mbr->bpb0.total_sectors != 0
        && (ch375disk_sector_count_hi != 0
            || mbr->bpb0.total_sectors <= ch375disk_sector_count_lo);

    entry = mbr_cap;
    p = mbr->part;
    for (n = 0; n < CH375DISK_PART_COUNT; n++) {
        p->boot_flag = entry[0];
        p->type = entry[4];
        /* bytes 8..15 = start + size, see struct ch375disk_part */
        dst = (unsigned char *) &p->lba_start_lo;
        for (i = 8; i < 16; i++)
            *dst++ = entry[i];
        entry += 16;
        p++;
    }

    return status;
}

ch375disk_read_bpb(lba_hi, lba_lo, bpb, boot_jump)
unsigned int lba_hi;
unsigned int lba_lo;
struct ch375disk_bpb *bpb;
unsigned char *boot_jump;
{
    int status;

    status = ch375disk_read_sector(lba_hi, lba_lo, cap_chunk_handler);
    if (status != USB_INT_SUCCESS)
        return status;

    parse_bpb(bpb);
    *boot_jump = cap_byte0;
    return status;
}

/* 1 if sectors_per_cluster is a non-zero power of 2 - v & (v - 1)
 * clears the lowest set bit, so it is 0 only for a single set bit. */
ch375disk_bpb_sane(bpb)
struct ch375disk_bpb *bpb;
{
    unsigned char spc;

    spc = bpb->sectors_per_cluster;
    if (bpb->bytes_per_sector != 512)
        return 0;
    if (spc == 0 || (spc & (spc - 1)) != 0)
        return 0;
    if (bpb->fat_count == 0)
        return 0;
    return 1;
}

/* FAT12 suitability: MBR type 0x01, or a superfloppy (part_type == 0,
 * caller's own convention for "no MBR partition entry to check"), and
 * the BPB must pass ch375disk_bpb_sane(), and the partition/disk
 * sector count must fit DOS 2.x's 16-bit BPB total-sectors field
 * (<= 65535 sectors, i.e. <= 32MB at 512 bytes/sector) - sectors_hi
 * must be 0 since unsigned int is itself only 16 bits here. */
ch375disk_fat12_check(part_type, sectors_hi, sectors_lo, bpb)
unsigned char part_type;
unsigned int sectors_hi;
unsigned int sectors_lo;
struct ch375disk_bpb *bpb;
{
    if (part_type != 0 && part_type != 0x01)
        return CH375DISK_FAT12_BAD_TYPE;
    if (sectors_hi != 0)
        return CH375DISK_FAT12_TOO_BIG;
    if (!ch375disk_bpb_sane(bpb))
        return CH375DISK_FAT12_BAD_BPB;
    return CH375DISK_FAT12_OK;
}
