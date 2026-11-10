/* partpick.c - CLI-only "first usable MBR volume" selection
 * (ch375disk_pick_volume()). MBR logic, not chip logic, so it lives
 * next to part.c/part.h rather than lib/pofoch375 - but ld86 links
 * whole objects, and POFOUDSK.SYS evaluates every partition per unit
 * instead of picking just one (see src/sysmount.c), so this stays a
 * separate translation unit linked only into POFOUDSK.COM.
 */

#include "part.h"

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
