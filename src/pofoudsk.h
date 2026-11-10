/* Driver<->CLI protocol shared by src/ch375sys.c (POFOUDSK.SYS) and
 * src/ch375cli.c (POFOUDSK.COM): per-unit mount status codes, the
 * UNSUPPORTED reason byte, and the IOCTL reply signature/v0 block
 * length - so both sides agree on the wire values without
 * duplicating them.
 */

/* IOCTL "v0" block (cmd_ioctl_in()'s reply, src/ch375sys.c): 8-byte
 * ASCII signature + version major/minor/patch + ctrl_state +
 * ioctl_last_cmd. */
#define POFOUDSK_IOCTL_SIG_LEN 8
#define POFOUDSK_IOCTL_V0_LEN  13

/* Per-unit mount status codes. */
#define CH375DISK_UNIT_NO_CONTROLLER 0
#define CH375DISK_UNIT_NO_USB        1
#define CH375DISK_UNIT_REAL          2
#define CH375DISK_UNIT_EMPTY         3
#define CH375DISK_UNIT_UNSUPPORTED   4
#define CH375DISK_UNIT_GPT           5

/* CH375DISK_UNIT_UNSUPPORTED reason byte. */
#define CH375DISK_UNIT_REASON_BAD_TYPE 0
#define CH375DISK_UNIT_REASON_TOO_BIG  1
#define CH375DISK_UNIT_REASON_BAD_BPB  2
