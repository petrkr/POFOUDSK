/* CH375.SYS virtual INFO disk - whenever a unit has no usable real
 * partition mounted, the driver presents this tiny synthetic
 * read-only FAT12 disk instead of refusing I/O: its root directory
 * holds INFO.TXT, explaining the current state (controller/USB/
 * partition status) for that unit.
 *
 * Sectors are generated on the fly straight into the far DOS transfer
 * buffer: far_fill() zeroes the sector, then the few non-zero bytes
 * are copied in from small resident templates via far_copy() - no
 * 512-byte image or sector buffer kept resident.
 */

#include "ch375disk.h"
#include "tinynum.h"
#include "sys.h"

/* Virtual disk BPB - real geometry of a disk that actually exists
 * (the synthetic INFO disk), not a placeholder. DOS 2.x builds its
 * DPB from this immediately at INIT and divides by bytes/sector and
 * sectors/cluster, so neither may ever be zero.
 * Doubles as the boot sector's BPB (sector 0, offset 0x0B). A unit
 * whose mounted partition is CH375DISK_UNIT_REAL gets its own real BPB
 * (unit_bpb[]) via BUILD_BPB instead - whose sector size must never
 * exceed this BPB's 512 (enforced by ch375disk_bpb_sane()). Sector
 * map: 0 boot sector, 1-2 FAT copies, 3 root dir, 4 INFO.TXT data
 * (cluster 2), 5-7 unused/zero. */
unsigned char bpb[] = {
    0x00, 0x02, /* bytes per sector = 512 */
    0x01,       /* sectors per cluster = 1 */
    0x01, 0x00, /* reserved sectors = 1 */
    0x02,       /* number of FATs = 2 */
    0x10, 0x00, /* root dir entries = 16 */
    VDISK_TOTAL_SECTORS, 0x00, /* total sectors */
    0xF8,       /* media byte */
    0x01, 0x00  /* sectors per FAT = 1 */
};

/* FAT copy head: media byte + cluster 2 = EOF (F8 FF FF FF 0F). */
static unsigned char fat_head[] = { 0xF8, 0xFF, 0xFF, 0xFF, 0x0F };

/* Root directory: volume label "NO USB Pn" (n patched per unit) +
 * read-only INFO.TXT at cluster 2 (size patched per request). */
#define ROOT_LABEL_UNIT 8
#define ROOT_INFO_SIZE  (32 + 28)
#define ROOT_LEN        64
static unsigned char root_dir[ROOT_LEN] = {
    'N', 'O', ' ', 'U', 'S', 'B', ' ', 'P', '1', ' ', ' ',
    0x08, /* ATTR_VOLUME_LABEL */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    'I', 'N', 'F', 'O', ' ', ' ', ' ', ' ', 'T', 'X', 'T',
    0x01, /* ATTR_READONLY */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x02, 0x00, /* start cluster */
    0, 0, 0, 0  /* size, patched */
};

/* INFO.TXT body - built by build_info_text() into a static buffer
 * (never on the stack: the driver's own stack is small and shared
 * with the mount path). 160 bytes covers the worst case (vendor+
 * product+capacity+one unit verdict line+version line, ~137). */
#define INFO_BUF_MAX 160
static char info_buf[INFO_BUF_MAX];
static char *info_p;

static void info_str(s)
char *s;
{
    while (*s)
        *info_p++ = *s++;
}

/* Appends the "Pn: ..." partition verdict line for cur_unit. Only
 * called for a unit that is NOT CH375DISK_UNIT_REAL (a REAL unit never
 * shows the virtual disk/INFO.TXT at all). */
static void append_unit_line()
{
    struct unit_state *u;

    u = &units[cur_unit];

    info_str("P");
    *info_p++ = '1' + cur_unit;
    info_str(": ");

    if (u->status == CH375DISK_UNIT_GPT)
        info_str("GPT disk - not supported, use MBR");
    else if (u->status == CH375DISK_UNIT_EMPTY)
        info_str("empty");
    else if (u->status != CH375DISK_UNIT_UNSUPPORTED)
        info_str("unknown");
    else if (u->reason == CH375DISK_UNIT_REASON_TOO_BIG)
        info_str("FAT12 too big");
    else if (u->reason == CH375DISK_UNIT_REASON_BAD_BPB)
        info_str("bad BPB");
    else {
        info_str("type ");
        info_p = put_hex2(info_p, u->part_type);
        info_str(" - unsupported, need FAT12 32MB (65535 sectors)");
    }
    info_str("\r\n");
}

/* Builds the current INFO.TXT body for cur_unit into info_buf and
 * returns its length. State-dependent (CRLF lines):
 *   - no controller / no USB: single state line,
 *   - USB mounted: vendor+product line, capacity line, then this
 *     unit's own "Pn: ..." verdict line (skipped if the unit is
 *     actually REAL - no virtual disk is ever shown for it),
 *   - always: the version/build line last (msg_version minus '$').
 */
static unsigned int build_info_text()
{
    unsigned char i;

    info_p = info_buf;

    if (ctrl_state != STATE_CONTROLLER_OK) {
        info_str("CH375 controller not present\r\n");
    } else if (!usb_state) {
        info_str("No USB disk connected\r\n");
    } else {
        info_str(vendor);
        info_str(" ");
        info_str(product);
        info_str("\r\nDisk ");
        /* MB = sectors * 512 / 1048576 = sectors / 2048 - as a 32-bit
         * right shift by 11 across the hi/lo pair without any 32-bit
         * long helper: lo>>11 keeps lo's top 5 bits, hi<<5 feeds in
         * the bits that crossed from hi. Truncates for disks far
         * beyond any real USB flash drive's size - cosmetic only. */
        info_p = put_udec(info_p, (cap_sectors_hi << 5) | (cap_sectors_lo >> 11));
        info_str("MB\r\n");

        if (!unit_is_real(cur_unit))
            append_unit_line();
    }

    for (i = 0; i < MSG_VERSION_LEN; i++)
        *info_p++ = msg_version[i];
    return info_p - info_buf;
}

/* Generates one 512-byte virtual disk sector into seg:off. */
void generate_sector(seg, off, sector)
unsigned int seg;
unsigned int off;
unsigned int sector;
{
    unsigned int len;

    far_fill(seg, off, 512);

    if (sector == 0) {
        far_copy(seg, off + 0x0B, bpb, sizeof(bpb));
        poke_w(seg, off + 510, 0xAA55);
    } else if (sector <= 2) {
        far_copy(seg, off, fat_head, sizeof(fat_head));
    } else if (sector == 3) {
        root_dir[ROOT_LABEL_UNIT] = '1' + cur_unit;
        len = build_info_text();
        root_dir[ROOT_INFO_SIZE] = (unsigned char) len;
        root_dir[ROOT_INFO_SIZE + 1] = (unsigned char) (len >> 8);
        far_copy(seg, off, root_dir, ROOT_LEN);
    } else if (sector == 4) {
        far_copy(seg, off, info_buf, build_info_text());
    }
    /* sectors 5-7: left zero. */
}
