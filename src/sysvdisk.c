/* Virtual status disk: an empty read-only FAT12 disk for units without
 * a usable partition; the volume label tells why. Sectors are generated
 * straight into the DOS buffer. */

#include "pofoudsk.h"
#include "tinynum.h"
#include "sys.h"

/* BPB of the virtual disk, also handed to DOS at INIT - bytes/sector
 * and sectors/cluster must never be 0 (DOS 2.x divides by them). Real
 * partitions must not exceed its 512-byte sector. */
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

/* media byte, no cluster in use */
static unsigned char fat_head[] = { 0xF8, 0xFF, 0xFF };

/* root directory: volume label only */
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

/* NO CH375, NO USB, Pn EMPTY, Pn GPT, Pn TOO BIG, Pn BAD BPB, Pn TYPE xx */
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

/* Sector map: 0 boot, 1-2 FAT, 3 root dir, 4-7 zero. */
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
