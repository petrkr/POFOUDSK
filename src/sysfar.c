/* Asm helpers: far memory, bulk port output, DOS print, PID check. Each
 * sets up its own bp frame (an #asm-only body gets no BCC prologue);
 * args at 4/6/8/10[bp]. */

#include "sys.h"

/* PID id of the CH375 card (board jumper) */
#define PID_ID_CH375 0x75


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

/* src_seg:src_off -> CH375 data port 807Eh, len bytes. */
void far_out(src_seg, src_off, len)
unsigned int src_seg;
unsigned int src_off;
unsigned int len;
{
#asm
    push bp
    mov bp,sp
    push ds
    push si
    push cx
    push dx
    mov ax,4[bp]
    mov ds,ax
    mov si,6[bp]
    mov cx,8[bp]
    mov dx,#0x807E
    cld
    jcxz farout_done
farout_loop:
    lodsb
    out dx,al
    loop farout_loop
farout_done:
    pop dx
    pop cx
    pop si
    pop ds
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

/* INT 61h/1Ah: 1 if a PID card with PID_ID_CH375 is present. */
unsigned char pid_check()
{
#asm
    mov ah,#0x1A
    int 0x61
    or al,al
    jz pid_absent
    cmp ah,#PID_ID_CH375
    jne pid_absent
    mov al,#1
    jmp pid_done
pid_absent:
    xor al,al
pid_done:
#endasm
}
