/* put_hex2() - own unit, ld86 links whole objects. */

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
