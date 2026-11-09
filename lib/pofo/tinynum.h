#ifndef TINYNUM_H
#define TINYNUM_H

/* Division-free number formatting, see tinynum.c. Both write into out
   (no terminator) and return the position just past the last digit. */

char *put_hex2();
char *put_udec();

#endif
