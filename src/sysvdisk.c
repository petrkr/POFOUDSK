/* CH375.SYS virtual status disk - whenever a unit has no usable real
 * partition mounted, the driver presents this tiny synthetic, empty,
 * read-only FAT12 disk instead of refusing I/O: its volume label
 * tells why (controller/USB/partition state). Details are available
 * through IOCTL (CH375.COM).
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
 * (the synthetic status disk), not a placeholder. DOS 2.x builds its
 * DPB from this immediately at INIT and divides by bytes/sector and
 * sectors/cluster, so neither may ever be zero.
 * Doubles as the boot sector's BPB (sector 0, offset 0x0B). A unit
 * whose mounted partition is CH375DISK_UNIT_REAL gets its own real BPB
 * (unit_bpb[]) via BUILD_BPB instead - whose sector size must never
 * exceed this BPB's 512 (enforced by ch375disk_bpb_sane()). */
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

/* FAT copy head: media byte, no cluster in use. */
static unsigned char fat_head[] = { 0xF8, 0xFF, 0xFF };

/* Root directory: just the volume label, set per request from the
 * unit state (see set_label()). */
#define ROOT_LEN 32
static unsigned char root_dir[ROOT_LEN] = {
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    0x08 /* ATTR_VOLUME_LABEL */
};

static char *put_str(p, s)
char *p;
char *s;
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* Volume label (11 chars, space padded) describing why cur_unit has no
 * usable partition: NO CH375, NO USB, Pn EMPTY, Pn GPT, Pn TOO BIG,
 * Pn BAD BPB or Pn TYPE xx. */
static void set_label()
{
    struct unit_state *u;
    char *p;
    unsigned char i;

    p = (char *) root_dir;
    for (i = 0; i < 11; i++)
        p[i] = ' ';

    if (ctrl_state != STATE_CONTROLLER_OK) {
        put_str(p, "NO CH375");
        return;
    }
    if (!usb_state) {
        put_str(p, "NO USB");
        return;
    }

    u = &units[cur_unit];
    *p++ = 'P';
    *p++ = '1' + cur_unit;
    p++;
    if (u->status == CH375DISK_UNIT_EMPTY)
        put_str(p, "EMPTY");
    else if (u->status == CH375DISK_UNIT_GPT)
        put_str(p, "GPT");
    else if (u->reason == CH375DISK_UNIT_REASON_TOO_BIG)
        put_str(p, "TOO BIG");
    else if (u->reason == CH375DISK_UNIT_REASON_BAD_BPB)
        put_str(p, "BAD BPB");
    else
        put_hex2(put_str(p, "TYPE "), u->part_type);
}

/* Generates one 512-byte virtual disk sector into seg:off. Sector
 * map: 0 boot sector, 1-2 FAT copies, 3 root dir (label only), 4-7
 * zero. */
void generate_sector(seg, off, sector)
unsigned int seg;
unsigned int off;
unsigned int sector;
{
    far_fill(seg, off, 512);

    if (sector == 0) {
        far_copy(seg, off + 0x0B, bpb, sizeof(bpb));
        poke_w(seg, off + 510, 0xAA55);
    } else if (sector <= 2) {
        far_copy(seg, off, fat_head, sizeof(fat_head));
    } else if (sector == 3) {
        set_label();
        far_copy(seg, off, root_dir, ROOT_LEN);
    }
}
