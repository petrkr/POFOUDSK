/* Number-to-text core of tinylib, safe to link where no libc is
   available at all (POFOUDSK.SYS device driver, future ROM code): no
   division/modulo - BCC's unsigned div/mod helpers (imodu/idiv_u) live
   in libc - and no varargs. Plain C, so it works with either calling
   convention (-C-f or not).

   tinylib.c #includes this file, so programs linking tinylib.c get it
   automatically and must NOT also link tinynum.c. A driver/ROM build
   links tinynum.c alone, without the rest of tinylib - or just
   tinyhex.c / tinydec.c if it needs only one of them. */

#include "tinyhex.c"
#include "tinydec.c"
