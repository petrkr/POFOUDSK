/* ch375disk.c - driver-safe disk-level layer, see ch375disk.h for the
 * module overview. Builds on top of ch375core.c's primitives
 * (ch375_write_cmd/write_data/wait_int/get_status/rd_usb_data_q) -
 * never printf, never BCC's unsigned-long runtime helpers, never a
 * 512-byte sector buffer, so this file stays linkable into CH375.SYS
 * as well as CH375.COM (only CH375.COM links it in for now).
 */

#include "port.h"
#include "ch375.h"
#include "ch375disk.h"

#define CMD_DISK_INIT  0x51
#define CMD_DISK_SIZE  0x53
#define CMD_DISK_READ  0x54
#define CMD_DISK_RD_GO 0x55
#define CMD_DISK_R_SENSE 0x5A

/* DISK_SIZE attempts and pause between them - see ch375disk_size(). */
#define DISK_SIZE_TRIES    5
#define DISK_SIZE_RETRY_MS 100

/* Outer bound on ch375disk_wait_connect()'s polling loop - each
 * iteration itself already waits up to ch375_wait_int()'s own bound,
 * so this just limits how many such bounded waits we retry across
 * spurious/irrelevant interrupt codes before giving up. Untuned, same
 * spirit as ch375core.c's WAIT_INT_MAX_ITER. */
#define WAIT_CONNECT_MAX_POLLS 50U

/* Vendor/product strings from the SCSI INQUIRY reply DISK_INIT
 * leaves queued (see ch375disk_init()), NUL-terminated, trailing
 * spaces kept. Empty if no INQUIRY block was seen. */
char ch375disk_vendor[9];
char ch375disk_product[17];

/* Delay after USB_INT_CONNECT before DISK_INIT - lets the freshly
 * powered device settle (USB spec allows it 100 ms). Precaution, not
 * a HW-confirmed requirement. */
#define CONNECT_SETTLE_MS 100

/* Upper bound on RD_USB_DATA blocks drained after DISK_INIT - same
 * value the original ch375.asm drain used. */
#define DRAIN_MAX_BLOCKS 32

/* Observed on HW: DISK_INIT/DISK_SIZE may report success as
 * CMD_RET_SUCCESS (51h) instead of USB_INT_SUCCESS (14h). Normalize
 * so callers only check USB_INT_SUCCESS. */
static int norm_status(s)
int s;
{
    return (s == CMD_RET_SUCCESS) ? USB_INT_SUCCESS : s;
}

/* Observed on HW (original ch375.asm): DISK_INIT internally runs a
 * SCSI INQUIRY and leaves its 36-byte reply queued in the chip's read
 * buffer. Unless drained, RD_USB_DATA after the next command (e.g.
 * DISK_SIZE) returns these stale bytes instead of its own data. Drain
 * until an empty block, keeping vendor (bytes 8-15) and product
 * (16-31) from the first block that holds them. */
static void drain_inquiry()
{
    unsigned char buf[64];
    int len, n, i, got;

    ch375disk_vendor[0] = 0;
    ch375disk_product[0] = 0;
    got = 0;

    for (n = 0; n < DRAIN_MAX_BLOCKS; n++) {
        len = ch375_rd_usb_data_q(buf);
        if (len == 0)
            break;
        if (!got && len >= 32) {
            for (i = 0; i < 8; i++)
                ch375disk_vendor[i] = buf[8 + i];
            ch375disk_vendor[8] = 0;
            for (i = 0; i < 16; i++)
                ch375disk_product[i] = buf[16 + i];
            ch375disk_product[16] = 0;
            got = 1;
        }
    }
}

unsigned int ch375disk_sector_count_hi;
unsigned int ch375disk_sector_count_lo;
unsigned int ch375disk_sector_size_hi;
unsigned int ch375disk_sector_size_lo;

/* DISK_INIT (datasheet 5.15) - see ch375.c's older comment for the
 * full protocol rationale; moved here because it has no stdio/long
 * dependency and belongs with the rest of the disk-level layer. */
ch375disk_init()
{
    int status;

    ch375_write_cmd(CMD_DISK_INIT);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    if (status == USB_INT_SUCCESS)
        drain_inquiry();
    return status;
}

/* Poll GET_STATUS (via the bounded ch375_wait_int()) until
 * USB_INT_CONNECT or USB_INT_DISCONNECT is seen, retrying across any
 * other/spurious status up to WAIT_CONNECT_MAX_POLLS times. Returns
 * that status, or CH375_TIMEOUT if the outer bound is exhausted or
 * any individual wait_int() times out. */
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
        /* anything else: keep waiting, bounded by the outer loop */
    }
    return CH375_TIMEOUT;
}

/* USB media detection: wait (bounded) for USB_INT_CONNECT, then run
 * DISK_INIT. Returns USB_INT_DISCONNECT if no device is attached,
 * CH375_TIMEOUT on timeout, or DISK_INIT's own completion status
 * (USB_INT_SUCCESS on success) once a device was seen connecting. */
ch375disk_detect()
{
    int status;

    status = ch375disk_wait_connect();
    if (status == CH375_TIMEOUT)
        return USB_INT_DISCONNECT; /* no connect event in time = no device */
    if (status != USB_INT_CONNECT)
        return status;

    ch375_delay_ms(CONNECT_SETTLE_MS);
    return ch375disk_init();
}

/* DISK_SIZE (datasheet 5.16) - same protocol/off-by-one as the old
 * ch375.c version (see git history), but result is hi/lo unsigned int
 * pairs (ch375disk_sector_count_hi/lo, ch375disk_sector_size_hi/lo)
 * instead of `unsigned long`, so this is safe to link into the
 * driver. Both chip fields are big-endian 4-byte values; the raw
 * sector count field is the last valid LBA, so it is incremented by
 * one here (with hi/lo carry) to get the actual sector count. */
/* DISK_R_SENSE (5Ah): SCSI REQUEST SENSE - clears a pending check
 * condition (e.g. Unit Attention after reset) so the next command can
 * succeed. Its sense data, if queued, is drained and discarded. */
static int request_sense()
{
    unsigned char buf[64];
    int status, n;

    ch375_write_cmd(CMD_DISK_R_SENSE);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    for (n = 0; n < DRAIN_MAX_BLOCKS; n++)
        if (ch375_rd_usb_data_q(buf) == 0)
            break;
    return status;
}

/* Number of DISK_SIZE attempts the last ch375disk_size() call needed
 * (diagnostics only). */
int ch375disk_size_tries;

static int disk_size_once();

/* Observed on HW (Alcor 058f:6387): the first DISK_SIZE after
 * DISK_INIT fails with USB_INT_DISK_ERR - consistent with a SCSI Unit
 * Attention pending after reset (not confirmed). Retry with a REQUEST
 * SENSE in between. */
ch375disk_size()
{
    int status;

    for (ch375disk_size_tries = 1; ; ch375disk_size_tries++) {
        status = disk_size_once();
        if (status != USB_INT_DISK_ERR || ch375disk_size_tries >= DISK_SIZE_TRIES)
            return status;
        request_sense();
        ch375_delay_ms(DISK_SIZE_RETRY_MS);
    }
}

static int disk_size_once()
{
    int status;
    unsigned char buf[8];
    unsigned int hi, lo;

    ch375_write_cmd(CMD_DISK_SIZE);
    if (!ch375_wait_int())
        return CH375_TIMEOUT;
    status = norm_status(ch375_get_status());
    if (status != USB_INT_SUCCESS)
        return status;

    if (ch375_rd_usb_data_q(buf) != 8) /* expected length 8 */
        return status;

    hi = ((unsigned int) buf[0] << 8) | buf[1];
    lo = ((unsigned int) buf[2] << 8) | buf[3];
    lo++;
    if (lo == 0)
        hi++; /* carry out of the low half */
    ch375disk_sector_count_hi = hi;
    ch375disk_sector_count_lo = lo;

    ch375disk_sector_size_hi = ((unsigned int) buf[4] << 8) | buf[5];
    ch375disk_sector_size_lo = ((unsigned int) buf[6] << 8) | buf[7];

    return status;
}

/* DISK_READ (datasheet 5.22) + DISK_RD_GO (5.18), streaming variant:
 * same protocol as the old ch375.c ch375_disk_read(), but LBA is an
 * hi/lo unsigned int pair (not `unsigned long`) and instead of
 * filling a 512-byte buffer, each 64-byte chunk is handed to the
 * caller-supplied handler as it arrives: handler(offset, chunk, len).
 * No buffer larger than 64 bytes is ever kept here. */
ch375disk_read_sector(lba_hi, lba_lo, handler)
unsigned int lba_hi;
unsigned int lba_lo;
int (*handler)();
{
    int status, len;
    unsigned int offset;
    unsigned char chunk[64];

    ch375_write_cmd(CMD_DISK_READ);
    ch375_write_data((unsigned char) lba_lo);
    ch375_write_data((unsigned char) (lba_lo >> 8));
    ch375_write_data((unsigned char) lba_hi);
    ch375_write_data((unsigned char) (lba_hi >> 8));
    ch375_write_data(1); /* sector count */

    offset = 0;

    for (;;) {
        if (!ch375_wait_int())
            return CH375_TIMEOUT;
        status = ch375_get_status();

        if (status == USB_INT_DISK_READ) {
            len = ch375_rd_usb_data_q(chunk);
            (*handler)(offset, chunk, len);
            offset += len;
            ch375_write_cmd(CMD_DISK_RD_GO);
            continue;
        }

        return norm_status(status); /* USB_INT_SUCCESS or USB_INT_DISK_ERR */
    }
}

/* --- MBR parsing, built on ch375disk_read_sector() --- */

/* Capture window for the MBR partition table + signature: sector
 * bytes 446 (0x1BE) through 511 inclusive (4 * 16-byte entries + the
 * 2-byte 0x55AA signature). Also captures byte 0 (boot/jump byte),
 * used by the superfloppy check below - kept in the same small static
 * buffer rather than a full 512-byte sector image. */
#define MBR_CAP_START 446
#define MBR_CAP_LEN   66
static unsigned char mbr_cap[MBR_CAP_LEN];
static unsigned char mbr_byte0;

#define BPB_CAP_START 0x0B
#define BPB_CAP_LEN   13
static unsigned char bpb_cap[BPB_CAP_LEN];
static unsigned char bpb_byte0;

static void parse_bpb(bpb)
struct ch375disk_bpb *bpb;
{
    bpb->bytes_per_sector   = ((unsigned int) bpb_cap[1] << 8) | bpb_cap[0];
    bpb->sectors_per_cluster = bpb_cap[2];
    bpb->reserved_sectors   = ((unsigned int) bpb_cap[4] << 8) | bpb_cap[3];
    bpb->fat_count          = bpb_cap[5];
    bpb->root_entries       = ((unsigned int) bpb_cap[7] << 8) | bpb_cap[6];
    bpb->total_sectors      = ((unsigned int) bpb_cap[9] << 8) | bpb_cap[8];
    bpb->media_descriptor   = bpb_cap[10];
    bpb->sectors_per_fat    = ((unsigned int) bpb_cap[12] << 8) | bpb_cap[11];
}

static mbr_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    unsigned int i, pos;

    for (i = 0; i < (unsigned int) len; i++) {
        pos = offset + i;
        if (pos == 0)
            mbr_byte0 = chunk[i];
        if (pos >= MBR_CAP_START && pos < MBR_CAP_START + MBR_CAP_LEN)
            mbr_cap[pos - MBR_CAP_START] = chunk[i];
        if (pos >= BPB_CAP_START && pos < BPB_CAP_START + BPB_CAP_LEN)
            bpb_cap[pos - BPB_CAP_START] = chunk[i];
    }
}

ch375disk_read_mbr(lba_hi, lba_lo, mbr)
unsigned int lba_hi;
unsigned int lba_lo;
struct ch375disk_mbr *mbr;
{
    int status, n;
    unsigned char *entry;

    status = ch375disk_read_sector(lba_hi, lba_lo, mbr_chunk_handler);
    if (status != USB_INT_SUCCESS)
        return status;

    mbr->has_mbr_sig = (mbr_cap[64] == 0x55 && mbr_cap[65] == 0xAA);
    parse_bpb(&mbr->bpb0);

    /* Superfloppy (sector 0 IS the FAT boot sector): jump opcode
     * 0xEB/0xE9 plus a sane BPB whose size fits the disk (needs
     * ch375disk_size() called first). MBR code typically starts with
     * something else (e.g. 0x33, 0xFA) and has no BPB. */
    mbr->is_superfloppy = (mbr_byte0 == 0xEB || mbr_byte0 == 0xE9)
        && ch375disk_bpb_sane(&mbr->bpb0)
        && mbr->bpb0.total_sectors != 0
        && (ch375disk_sector_count_hi != 0
            || mbr->bpb0.total_sectors <= ch375disk_sector_count_lo);

    for (n = 0; n < CH375DISK_PART_COUNT; n++) {
        entry = mbr_cap + (n * 16);
        mbr->part[n].boot_flag = entry[0];
        mbr->part[n].type = entry[4];
        mbr->part[n].lba_start_lo = ((unsigned int) entry[9] << 8) | entry[8];
        mbr->part[n].lba_start_hi = ((unsigned int) entry[11] << 8) | entry[10];
        mbr->part[n].sectors_lo   = ((unsigned int) entry[13] << 8) | entry[12];
        mbr->part[n].sectors_hi   = ((unsigned int) entry[15] << 8) | entry[14];
    }

    return status;
}

/* --- Boot sector BPB extraction, built on ch375disk_read_sector() --- */

static bpb_chunk_handler(offset, chunk, len)
unsigned int offset;
unsigned char *chunk;
int len;
{
    unsigned int i, pos;

    for (i = 0; i < (unsigned int) len; i++) {
        pos = offset + i;
        if (pos == 0)
            bpb_byte0 = chunk[i];
        if (pos >= BPB_CAP_START && pos < BPB_CAP_START + BPB_CAP_LEN)
            bpb_cap[pos - BPB_CAP_START] = chunk[i];
    }
}

ch375disk_read_bpb(lba_hi, lba_lo, bpb, boot_jump)
unsigned int lba_hi;
unsigned int lba_lo;
struct ch375disk_bpb *bpb;
unsigned char *boot_jump;
{
    int status;

    status = ch375disk_read_sector(lba_hi, lba_lo, bpb_chunk_handler);
    if (status != USB_INT_SUCCESS)
        return status;

    parse_bpb(bpb);

    *boot_jump = bpb_byte0;

    return status;
}

/* 1 if sectors_per_cluster is a non-zero power of 2 - checked by
 * shifting off the lowest set bit and seeing if anything is left. No
 * BCC runtime helpers needed, just shifts. */
static unsigned char is_pow2_nonzero(v)
unsigned char v;
{
    if (v == 0)
        return 0;
    while ((v & 1) == 0)
        v >>= 1;
    return (v == 1);
}

ch375disk_bpb_sane(bpb)
struct ch375disk_bpb *bpb;
{
    if (bpb->bytes_per_sector != 512)
        return 0;
    if (!is_pow2_nonzero(bpb->sectors_per_cluster))
        return 0;
    if (bpb->fat_count == 0)
        return 0;
    return 1;
}

static unsigned char part_valid(p)
struct ch375disk_part *p;
{
    return p->type != 0 && (p->boot_flag == 0x00 || p->boot_flag == 0x80);
}

unsigned char ch375disk_pick_volume(mbr, start_hi, start_lo, sectors_hi, sectors_lo)
struct ch375disk_mbr *mbr;
unsigned int *start_hi;
unsigned int *start_lo;
unsigned int *sectors_hi;
unsigned int *sectors_lo;
{
    int n, found;

    if (mbr->is_superfloppy) {
        *start_hi = 0;
        *start_lo = 0;
        *sectors_hi = 0;
        *sectors_lo = mbr->bpb0.total_sectors;
        return 0;
    }

    if (!mbr->has_mbr_sig)
        return CH375DISK_NO_VOLUME;

    /* First FAT12 entry wins; otherwise the first valid entry, so the
     * caller can still report why it's unusable. */
    found = -1;
    for (n = 0; n < CH375DISK_PART_COUNT; n++) {
        if (!part_valid(&mbr->part[n]))
            continue;
        if (mbr->part[n].type == 0x01) {
            found = n;
            break;
        }
        if (found < 0)
            found = n;
    }
    if (found < 0)
        return CH375DISK_NO_VOLUME;

    *start_hi = mbr->part[found].lba_start_hi;
    *start_lo = mbr->part[found].lba_start_lo;
    *sectors_hi = mbr->part[found].sectors_hi;
    *sectors_lo = mbr->part[found].sectors_lo;
    return mbr->part[found].type;
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
