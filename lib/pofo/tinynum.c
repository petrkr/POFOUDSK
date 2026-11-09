/* Number-to-text core of tinylib, safe to link where no libc is
   available at all (CH375.SYS device driver, future ROM code): no
   division/modulo - BCC's unsigned div/mod helpers (imodu/idiv_u) live
   in libc - and no varargs. Plain C, so it works with either calling
   convention (-C-f or not).

   tinylib.c #includes this file, so programs linking tinylib.c get it
   automatically and must NOT also link tinynum.c. A driver/ROM build
   links tinynum.c alone, without the rest of tinylib. */

#include "tinynum.h"

static char hex_digits[] = "0123456789ABCDEF";

/* Byte as two uppercase hex digits. */
char *put_hex2(out, value)
char *out;
unsigned char value;
{
    *out++ = hex_digits[value >> 4];
    *out++ = hex_digits[value & 15];
    return out;
}

/* Unsigned decimal, no leading zeros ("0" for zero) - by repeated
   subtraction of each power of ten instead of value % 10 / value / 10. */
char *put_udec(out, value)
char *out;
unsigned int value;
{
    static unsigned int pow10[5] = { 10000, 1000, 100, 10, 1 };
    unsigned char i, started, d;

    started = 0;
    for (i = 0; i < 5; i++) {
        d = 0;
        while (value >= pow10[i]) {
            value -= pow10[i];
            d++;
        }
        if (d != 0 || started || i == 4) {
            *out++ = '0' + d;
            started = 1;
        }
    }
    return out;
}
