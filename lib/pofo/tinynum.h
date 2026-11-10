#ifndef TINYNUM_H
#define TINYNUM_H

/* Division-free number formatting. Both write into out (no terminator)
   and return the position past the last digit. */

char *put_hex2();
char *put_udec();

#endif
