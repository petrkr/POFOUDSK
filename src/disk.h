/* Presentation helpers built on top of the driver-safe disk layer -
 * CLI-only, see disk.c.
 */

extern char out_line[];
extern char *put_udec32();
extern char *put_mb();

extern void disk_print_capacity();
extern void disk_print_mbr_partition();
extern void disk_print_bpb();
extern void disk_print_fat12_verdict();
