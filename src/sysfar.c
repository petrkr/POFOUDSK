/* Asm helpers: far memory, DOS print. Each sets up its own bp frame
 * (an #asm-only body gets no BCC prologue); args at 4/6/8/10[bp]. */

#include "sys.h"

unsigned char peek_b(seg, off)
unsigned int seg;
unsigned int off;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    seg es
    mov al,[bx]
    xor ah,ah
    pop bx
    pop es
    pop bp
#endasm
}

void poke_b(seg, off, val)
unsigned int seg;
unsigned int off;
unsigned char val;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    mov al,8[bp]
    seg es
    mov [bx],al
    pop bx
    pop es
    pop bp
#endasm
}

void poke_w(seg, off, val)
unsigned int seg;
unsigned int off;
unsigned int val;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    mov ax,8[bp]
    seg es
    mov [bx],ax
    pop bx
    pop es
    pop bp
#endasm
}

unsigned int peek_w(seg, off)
unsigned int seg;
unsigned int off;
{
#asm
    push bp
    mov bp,sp
    push es
    push bx
    mov ax,4[bp]
    mov es,ax
    mov bx,6[bp]
    seg es
    mov ax,[bx]
    pop bx
    pop es
    pop bp
#endasm
}

/* DS:src -> dst_seg:dst_off, len bytes. */
void far_copy(dst_seg, dst_off, src, len)
unsigned int dst_seg;
unsigned int dst_off;
unsigned char *src;
unsigned int len;
{
#asm
    push bp
    mov bp,sp
    push es
    push si
    push di
    push cx
    mov ax,4[bp]
    mov es,ax
    mov di,6[bp]
    mov si,8[bp]
    mov cx,10[bp]
    cld
    rep
    movsb
    pop cx
    pop di
    pop si
    pop es
    pop bp
#endasm
}

/* Zero len bytes at dst_seg:dst_off. */
void far_fill(dst_seg, dst_off, len)
unsigned int dst_seg;
unsigned int dst_off;
unsigned int len;
{
#asm
    push bp
    mov bp,sp
    push es
    push di
    push cx
    mov ax,4[bp]
    mov es,ax
    mov di,6[bp]
    mov cx,8[bp]
    xor al,al
    cld
    rep
    stosb
    pop cx
    pop di
    pop es
    pop bp
#endasm
}

unsigned int get_cs()
{
#asm
    mov ax,cs
#endasm
}

void print_dollar_string(s)
char *s;
{
#asm
    push bp
    mov bp,sp
    push dx
    mov dx,4[bp]
    mov ah,#0x09
    int 0x21
    pop dx
    pop bp
#endasm
}
