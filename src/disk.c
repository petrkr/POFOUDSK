/* Presentation helpers for the ch375disk.c/ch375part.c disk-level
 * layer. CLI-only (CH375.COM) - prints through lib/pofo tinylib
 * (fmt() + con_puts()), never linked into CH375.SYS. Takes the hi/lo
 * structures the disk layer produces and prints them in human-readable
 * form; no CH375 protocol logic of its own.
 */

#include "ch375.h"
#include "ch375disk.h"
#include "disk.h"
#include "tinylib.h"
#include "tinynum.h"

/* Line buffer shared by all CLI output - longest line is the
 * vendor/product one (8 + 1 + 16 + " TC1 xx" + CRLF). */
char out_line[64];

/* Unsigned 32-bit decimal of hi:lo, by repeated subtraction of each
 * power of ten (hi/lo pairs, manual borrow) - no long helpers. */
char *put_udec32(out, hi, lo)
char *out;
unsigned int hi;
unsigned int lo;
{
    static unsigned int pow10[10][2] = {
        { 0x3B9A, 0xCA00 }, { 0x05F5, 0xE100 }, { 0x0098, 0x9680 },
        { 0x000F, 0x4240 }, { 0x0001, 0x86A0 }, { 0, 10000 },
        { 0, 1000 }, { 0, 100 }, { 0, 10 }, { 0, 1 }
    };
    unsigned char i, started, d;
    unsigned int ph, pl;

    started = 0;
    for (i = 0; i < 10; i++) {
        ph = pow10[i][0];
        pl = pow10[i][1];
        d = 0;
        while (hi > ph || (hi == ph && lo >= pl)) {
            if (lo < pl)
                hi--;
            lo -= pl;
            hi -= ph;
            d++;
        }
        if (d != 0 || started || i == 9) {
            *out++ = '0' + d;
            started = 1;
        }
    }
    return out;
}

/* "<n>MB" for a 512-byte-sector count hi:lo - sectors / 2048 as a
 * 32-bit right shift by 11 across the pair, same as CH375.SYS's
 * INFO.TXT. No terminator. */
char *put_mb(out, hi, lo)
char *out;
unsigned int hi;
unsigned int lo;
{
    out = put_udec(out, (hi << 5) | (lo >> 11));
    *out++ = 'M';
    *out++ = 'B';
    return out;
}

void disk_print_capacity()
{
    char *p;

    p = put_udec32(fmt(out_line, "sect:"), ch375disk_sector_count_hi, ch375disk_sector_count_lo);
    p = put_udec32(fmt(p, " sz:"), ch375disk_sector_size_hi, ch375disk_sector_size_lo);
    p = put_mb(fmt(p, "\r\n"), ch375disk_sector_count_hi, ch375disk_sector_count_lo);
    fmt(p, "\r\n");
    con_puts(out_line);
}

void disk_print_mbr_partition(part, n)
struct ch375disk_part *part;
int n;
{
    char *p;

    p = fmt(out_line, "%u t%x s", n, part->type);
    p = put_udec32(p, part->lba_start_hi, part->lba_start_lo);
    *p++ = ' ';
    if (part->sectors_hi != 0 || part->sectors_lo >= 2048)
        fmt(put_mb(p, part->sectors_hi, part->sectors_lo), "\r\n");
    else
        fmt(p, "%usec\r\n", part->sectors_lo);
    con_puts(out_line);
}

void disk_print_bpb(bpb)
struct ch375disk_bpb *bpb;
{
    fmt(out_line, "bps%u spc%u rs%u\r\nfat%u re%u ts%u\r\nmd%x spf%u\r\n",
        bpb->bytes_per_sector, bpb->sectors_per_cluster, bpb->reserved_sectors,
        bpb->fat_count, bpb->root_entries, bpb->total_sectors,
        bpb->media_descriptor, bpb->sectors_per_fat);
    con_puts(out_line);
}

void disk_print_fat12_verdict(reason)
int reason;
{
    if (reason == CH375DISK_FAT12_OK)
        con_puts("OK FAT12\r\n");
    else if (reason == CH375DISK_FAT12_BAD_TYPE)
        con_puts("unsupported: type\r\n");
    else if (reason == CH375DISK_FAT12_BAD_BPB)
        con_puts("unsupported: BPB\r\n");
    else if (reason == CH375DISK_FAT12_TOO_BIG)
        con_puts("unsupported: >65535 sect\r\n");
    else
        con_puts("unsupported\r\n");
}
