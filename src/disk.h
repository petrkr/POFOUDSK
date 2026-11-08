/* Presentation helpers (printf, unsigned long) built on top of the
 * driver-safe ch375disk.c layer - CLI-only, see disk.c.
 */

extern unsigned long disk_hilo_to_long();
extern void disk_print_capacity();
extern void disk_print_mbr_partition();
extern void disk_print_bpb();
extern void disk_print_fat12_verdict();
