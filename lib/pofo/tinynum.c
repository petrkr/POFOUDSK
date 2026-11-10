/* Number-to-text without libc: no division (BCC's div/mod helpers are in
   libc), no varargs, either calling convention. tinylib.c includes this
   file - never link both. Drivers may link tinyhex.c / tinydec.c alone. */

#include "tinyhex.c"
#include "tinydec.c"
