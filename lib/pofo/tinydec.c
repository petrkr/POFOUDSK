/* put_udec() - own unit, ld86 links whole objects. */

#include "tinynum.h"

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
