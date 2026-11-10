/* CH375.SYS internal interface, shared by its units:
 *   - ch375sys.c: device header, strategy/interrupt trampolines, own
 *     stack, request dispatch and the DOS command handlers,
 *   - sysfar.c:   asm helpers (far memory peek/poke/copy/fill, far
 *     buffer -> data port, DOS $-string print, PID check),
 *   - sysmount.c: controller/USB state machine (guard), mount/unmount
 *     and per-unit state,
 *   - sysvdisk.c: virtual status disk (sector generator, volume label).
 * ch375sys.c must stay first on the ld86 command line - its #asm
 * device header has to land at file offset 0.
 */

#define MAX_UNITS     4

/* guard() state machine, see sysmount.c guard(). */
#define STATE_NO_CONTROLLER 0
#define STATE_CONTROLLER_OK 1

struct unit_state {
    unsigned char status;   /* CH375DISK_UNIT_* (ch375disk.h) */
    unsigned char reason;   /* CH375DISK_UNIT_REASON_* - valid only if status == UNSUPPORTED */
    unsigned char part_type; /* MBR partition type byte, 0 for superfloppy/none */
    unsigned int start_hi;
    unsigned int start_lo;
    unsigned int size_hi;
    unsigned int size_lo;
    unsigned char mount_gen; /* snapshot of mount_gen at the time this unit's state was set */
};

/* --- ch375sys.c --- */
extern unsigned char cur_unit;
extern unsigned char unit_count;
extern unsigned char ioctl_last_cmd;
extern char msg_version[];

/* --- sysfar.c --- */
extern unsigned char peek_b();
extern unsigned int peek_w();
extern void poke_b();
extern void poke_w();
extern void far_copy();
extern void far_fill();
extern void far_out();
extern unsigned int get_cs();
extern void print_dollar_string();
extern unsigned char pid_check();

/* --- sysdirfix.asm --- */
extern dirfix_install();

/* --- sysmount.c --- */
extern unsigned char ctrl_state;
extern unsigned char usb_state;
extern unsigned char mount_gen;
extern char vendor[];
extern char product[];
extern unsigned int cap_sectors_hi;
extern unsigned int cap_sectors_lo;
extern struct unit_state units[];
extern unsigned char unit_bpb[MAX_UNITS][13];

extern void init_mount_state();
extern void drop_controller();
extern void unmount_all();
extern void guard();
extern void test_connect_resync();
extern unsigned char unit_is_real();
extern unsigned int current_content_id();

/* --- sysvdisk.c --- */
#define VDISK_TOTAL_SECTORS 8
extern unsigned char bpb[];
extern void generate_sector();
