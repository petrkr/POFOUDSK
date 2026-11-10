/* Driver <-> CLI protocol: unit status codes and IOCTL constants. */

/* IOCTL v0 block: signature, version x3, ctrl_state, last IOCTL byte */
#define POFOUDSK_IOCTL_SIG_LEN 8
#define POFOUDSK_IOCTL_V0_LEN  13

#define CH375DISK_UNIT_NO_CONTROLLER 0
#define CH375DISK_UNIT_NO_USB        1
#define CH375DISK_UNIT_REAL          2
#define CH375DISK_UNIT_EMPTY         3
#define CH375DISK_UNIT_UNSUPPORTED   4
#define CH375DISK_UNIT_GPT           5

#define CH375DISK_UNIT_REASON_BAD_TYPE 0
#define CH375DISK_UNIT_REASON_TOO_BIG  1
#define CH375DISK_UNIT_REASON_BAD_BPB  2
