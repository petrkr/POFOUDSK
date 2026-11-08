#ifndef CH375SYS_VERSION_H
#define CH375SYS_VERSION_H

/* Keep in sync with the CH375.SYS release version. */
#define CH375SYS_VERSION_MAJOR 0
#define CH375SYS_VERSION_MINOR 0
#define CH375SYS_VERSION_PATCH 0

/* Local build identity. Release CI replaces this with the commit hash.
 * Split into two 16-bit halves (not one unsigned long) because BCC's
 * long shift/compare runtime helpers (laddul/lorul/...) aren't
 * available to link against in this driver build - see ch375sys.c's
 * print_version(). */
#define CH375SYS_BUILD_ID_HI 0xFFFF
#define CH375SYS_BUILD_ID_LO 0x0006

#endif
