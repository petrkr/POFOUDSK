/* MBR partition parsing and the "USB device connected" sequence,
 * built on top of the ch375.c/ch375.h chip library.
 */

#include "ch375.h"
#include "disk.h"

/* MBR partition table: 4 entries of 16 bytes starting at offset 446
 * (0x1BE). Layout: +0 boot indicator, +4 partition type, +8 LBA
 * start (little-endian), +12 sector count (little-endian).
 */
disk_print_mbr_partition(sector0, n)
unsigned char *sector0;
int n;
{
    unsigned char *entry;
    unsigned long lba_start, sectors;

    entry = sector0 + 446 + n * 16;
    lba_start = ((unsigned long) entry[11] << 24) | ((unsigned long) entry[10] << 16)
              | ((unsigned long) entry[9] << 8) | entry[8];
    sectors = ((unsigned long) entry[15] << 24) | ((unsigned long) entry[14] << 16)
            | ((unsigned long) entry[13] << 8) | entry[12];

    printf("  partition %d: boot=%02x type=%02x lba_start=%lu sectors=%lu\n",
           n, entry[0], entry[4], lba_start, sectors);
}

/* Runs the full sequence once a USB device connect is seen:
 * DISK_INIT, DISK_SIZE, then DISK_READ sector 0 and print its MBR
 * partition table. Each step bails out (return) on failure instead of
 * nesting further ifs.
 */
disk_on_connect()
{
    int status, n;
    unsigned char sector0[512];

    status = ch375_disk_init();
    printf("  DISK_INIT status: %02x\n", status);
    if (status != USB_INT_SUCCESS)
        return;

    status = ch375_disk_size();
    printf("  DISK_SIZE status: %02x\n", status);
    if (status != USB_INT_SUCCESS)
        return;

    printf("  sectors: %lu\n", sector_count);
    printf("  sector size: %lu\n", sector_size);
    printf("  disk size: %luB\n", sector_size * sector_count);

    status = ch375_disk_read(0L, sector0);
    printf("  DISK_READ LBA0 status: %02x\n", status);
    if (status != USB_INT_SUCCESS)
        return;

    printf("  MBR signature: %02x %02x\n", sector0[510], sector0[511]);
    for (n = 0; n < 4; n++)
        disk_print_mbr_partition(sector0, n);
}
