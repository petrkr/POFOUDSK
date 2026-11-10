/* POFOUDSK.SYS asm helpers - BCC has no `far` pointers, no inp/outp and
 * no int86, so everything touching another segment, the CH375 data
 * port in bulk, DOS or the Portfolio BIOS lives here as #asm bodies.
 * Each function sets up its own bp frame (a body that is only #asm
 * gets no BCC prologue), cdecl args at 4/6/8/10[bp]. See sys.h.
 */

#include "sys.h"

/* PID identification byte for the CH375 card, set by a jumper on the
 * board. */
#define PID_ID_CH375 0x75

/* --- far memory helpers (seg:off), asm bodies, cdecl args at 4/6[bp] --- */

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

/* Bulk far copy: DS:src (near, our own data/stack segment - always
 * true while the driver runs, since _interrupt sets DS=ES=SS=CS) to
 * dst_seg:dst_off, len bytes. `rep movsb` is an 8088 instruction (no
 * `rep insb`/186+ needed) - used by READ on a real unit to move each
 * 64-byte USB chunk straight into the DOS far transfer buffer instead
 * of a poke_b-per-byte loop. */
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

/* Far memory fill with zero: dst_seg:dst_off, len bytes (`rep stosb`,
 * 8088-safe) - used to clear each virtual disk sector before the few
 * template bytes are copied in. */
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

/* Bulk far-to-port output: DS:src (far DOS buffer) -> port 807Eh
 * (CH375 data port), len bytes. Mirrors far_copy() in spirit, but
 * drives the data port instead of ES:DI - `lodsb`/`out dx,al` are both
 * 8088-safe. Used by the WRITE supplier (real_chunk_supplier() below)
 * to hand each 64-byte chunk straight from the DOS far transfer buffer
 * to the chip, no intermediate peek_b-per-byte loop. */
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

/* PID presence check (Atari Portfolio BIOS, int 61h/ah=1Ah):
 * AL=0 -> no PID card present; AL!=0 -> present, AH = PID id byte.
 * Returns 1 only if a card with PID_ID_CH375 is present. */
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
