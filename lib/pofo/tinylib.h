#ifndef TINYLIB_H
#define TINYLIB_H

/* Minimal libc replacement. These are the only string/console routines
   the program links, so nothing from libdos's stdio, printf or malloc
   gets pulled into the .COM. They follow whatever calling convention
   the rest of the program is compiled with (see the __FIRST_ARG_IN_AX__
   blocks in tinylib.c), unlike libdos.a which only knows the plain
   stack convention - so never call libc string functions from C code
   compiled with -C-f, use these. */

unsigned int str_len();
/* Copies src including its terminator; returns a pointer to the
   terminator written in dst, so appends chain naturally. */
char *str_copy();
void mem_copy();

/* DOS handle 1 (stdout) write - exactly what printf/fwrite ended up
   doing through stdio, minus the buffering. */
void con_write();
void con_puts();

/* Tiny sprintf. Conversions: %s string, %u unsigned int, %d signed int,
   %x byte as two uppercase hex digits, %i IPv4 dotted quad from an
   unsigned char[4]. Writes the terminator and returns a pointer to it. */
char *fmt();

#endif
