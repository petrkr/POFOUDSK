/* Presentation helpers (printf, `unsigned long`) for the ch375disk.c
 * disk-level layer. CLI-only - uses printf and unsigned long, neither
 * available in the device driver build, so this file is never linked
 * into CH375.SYS. Takes the hi/lo structures ch375disk.c/.h produce
 * and prints them in human-readable form; no CH375 protocol logic of
 * its own.
 */

#include "ch375.h"
#include "ch375disk.h"
#include "disk.h"

/* Combines an hi/lo unsigned int pair (ch375disk.c's 32-bit
 * representation) into an `unsigned long`, for printf("%lu", ...)
 * display only - never used by the driver-safe layer itself. */
unsigned long disk_hilo_to_long(hi, lo)
unsigned int hi;
unsigned int lo;
{
    return (((unsigned long) hi) << 16) | (unsigned long) lo;
}

void disk_print_capacity()
{
    unsigned long sectors, size, bytes;

    sectors = disk_hilo_to_long(ch375disk_sector_count_hi, ch375disk_sector_count_lo);
    size = disk_hilo_to_long(ch375disk_sector_size_hi, ch375disk_sector_size_lo);
    bytes = sectors * size;

    printf("sect:%lu sz:%lu\n", sectors, size);
    printf("%luMB\n", bytes / 1048576UL);
}

void disk_print_mbr_partition(part, n)
struct ch375disk_part *part;
int n;
{
    unsigned long start, sectors, mb;

    start = disk_hilo_to_long(part->lba_start_hi, part->lba_start_lo);
    sectors = disk_hilo_to_long(part->sectors_hi, part->sectors_lo);
    mb = (sectors * 512UL) / 1048576UL;

    if (mb > 0)
        printf("%d t%02x s%lu %luMB\n", n, part->type, start, mb);
    else
        printf("%d t%02x s%lu %lusec\n", n, part->type, start, sectors);
}

void disk_print_bpb(bpb)
struct ch375disk_bpb *bpb;
{
    printf("bps%u spc%u rs%u\n", bpb->bytes_per_sector,
           (unsigned int) bpb->sectors_per_cluster, bpb->reserved_sectors);
    printf("fat%u re%u ts%u\n", (unsigned int) bpb->fat_count,
           bpb->root_entries, bpb->total_sectors);
    printf("md%02x spf%u\n", (unsigned int) bpb->media_descriptor,
           bpb->sectors_per_fat);
}

void disk_print_fat12_verdict(reason)
int reason;
{
    switch (reason) {
    case CH375DISK_FAT12_OK:
        printf("OK FAT12\n");
        break;
    case CH375DISK_FAT12_BAD_TYPE:
        printf("unsupported: type\n");
        break;
    case CH375DISK_FAT12_BAD_BPB:
        printf("unsupported: BPB\n");
        break;
    case CH375DISK_FAT12_TOO_BIG:
        printf("unsupported: >32MB\n");
        break;
    default:
        printf("unsupported\n");
        break;
    }
}
