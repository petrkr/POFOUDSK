/* MBR / boot sector BPB parsing and FAT12 check. Only the needed bytes
 * are captured from the 64-byte chunks as they stream in. */

#include "ch375.h"
#include "ch375disk.h"
#include "part.h"

/* Captured on every MBR/BPB read: byte 0, BPB bytes 0x0B..0x17
 * (exported as ch375disk_bpb_raw) and MBR bytes 0x1BE..0x1FF. */
#define MBR_CAP_START 446
#define MBR_CAP_LEN   66
static unsigned char mbr_cap[MBR_CAP_LEN];
static unsigned char cap_byte0;

#define BPB_CAP_START 0x0B
unsigned char ch375disk_bpb_raw[CH375DISK_BPB_RAW_LEN];

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

    /* Superfloppy: jump opcode at byte 0 + a sane BPB that fits the disk
     * (needs ch375disk_size() first). */
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
        /* bytes 8..15 = start + size */
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

/* sectors/cluster must be a power of two */
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

/* MBR type 01h (or 0 = superfloppy), sane BPB, and at most 65535
 * sectors - DOS 2.x has a 16-bit total-sectors field. */
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
