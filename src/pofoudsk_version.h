#ifndef POFOUDSK_VERSION_H
#define POFOUDSK_VERSION_H

/* Keep in sync with the POFOUDSK.SYS release version. */
#define POFOUDSK_VERSION_MAJOR 0
#define POFOUDSK_VERSION_MINOR 0
#define POFOUDSK_VERSION_PATCH 0

/* Local build identity, printed by POFOUDSK.SYS at INIT. Split into
 * two 16-bit halves (not one unsigned long) because BCC's long
 * shift/compare runtime helpers (laddul/lorul/...) aren't available
 * to link against in this driver build. */
#define POFOUDSK_BUILD_ID_HI 0xFFFF
#define POFOUDSK_BUILD_ID_LO 0x000D

#endif
