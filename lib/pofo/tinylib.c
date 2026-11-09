#include "tinylib.h"

/* Same as bcc's <stdarg.h>, minus its #warning about the first argument
   living in a register under -C-f: fmt() only ever takes the address of
   its second argument, which is always on the stack. */
typedef char *va_list;
#define va_start(ap, p) (ap = (char *)(&(p) + 1))
#define va_arg(ap, type) ((type *)(ap += sizeof(type)))[-1]

unsigned int str_len(text)
char *text;
{
#asm
#if __FIRST_ARG_IN_AX__
    mov bx,ax
#else
    mov bx,sp
    mov bx,2[bx]
#endif
    mov cx,bx
.str_len_loop:
    cmp byte ptr [bx],#0
    je .str_len_done
    inc bx
    jmp .str_len_loop
.str_len_done:
    mov ax,bx
    sub ax,cx
#endasm
}

char *str_copy(dst, src)
char *dst;
char *src;
{
#asm
    push si
    push di
#if __FIRST_ARG_IN_AX__
    mov di,ax
    mov bx,sp
    mov si,6[bx]
#else
    mov bx,sp
    mov di,6[bx]
    mov si,8[bx]
#endif
    push ds
    pop es
    cld
.str_copy_loop:
    lodsb
    stosb
    or al,al
    jnz .str_copy_loop
    mov ax,di
    dec ax
    pop di
    pop si
#endasm
}

void mem_copy(dst, src, count)
char *dst;
char *src;
unsigned int count;
{
#asm
    push si
    push di
#if __FIRST_ARG_IN_AX__
    mov di,ax
    mov bx,sp
    mov si,6[bx]
    mov cx,8[bx]
#else
    mov bx,sp
    mov di,6[bx]
    mov si,8[bx]
    mov cx,10[bx]
#endif
    push ds
    pop es
    cld
    rep
    movsb
    pop di
    pop si
#endasm
}

void con_write(text, length)
char *text;
unsigned int length;
{
#asm
#if __FIRST_ARG_IN_AX__
    mov dx,ax
    mov bx,sp
    mov cx,2[bx]
#else
    mov bx,sp
    mov dx,2[bx]
    mov cx,4[bx]
#endif
    mov bx,#1
    mov ah,#$40
    int $21
#endasm
}

void con_puts(text)
char *text;
{
    con_write(text, str_len(text));
}

/* put_hex2()/put_udec() - division-free, shared with driver/ROM builds
   that link tinynum.c on its own (see there). */
#include "tinynum.c"

char *fmt(out, format)
char *out;
char *format;
{
    va_list ap;
    char c;
    int value;
    unsigned char *quad;
    int i;

    va_start(ap, format);
    for (;;) {
        c = *format++;
        if (c == 0)
            break;
        if (c != '%') {
            *out++ = c;
            continue;
        }
        c = *format++;
        switch (c) {
        case 's':
            out = str_copy(out, va_arg(ap, char *));
            break;
        case 'u':
            out = put_udec(out, va_arg(ap, unsigned int));
            break;
        case 'd':
            value = va_arg(ap, int);
            if (value < 0) {
                *out++ = '-';
                value = -value;
            }
            out = put_udec(out, value);
            break;
        case 'x':
            out = put_hex2(out, va_arg(ap, int));
            break;
        case 'i':
            quad = va_arg(ap, unsigned char *);
            for (i = 0; i < 4; i++) {
                if (i != 0)
                    *out++ = '.';
                out = put_udec(out, *quad++);
            }
            break;
        default:
            /* Unknown conversion (or "%%"): emit the character itself. */
            *out++ = c;
            break;
        }
    }
    *out = 0;
    return out;
}
