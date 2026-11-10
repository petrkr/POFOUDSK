/* CH375 transport: Atari Portfolio PID card. Command port 807Dh, data
 * port 807Eh. Each #asm-only body gets no BCC prologue, so each
 * function that uses args sets up its own bp frame; args at 4/6/8[bp]. */

/* PID id of the CH375 card (board jumper) */
#define PID_ID_CH375 0x75

/* INT 61h/1Ah: 1 if a PID card with PID_ID_CH375 is present. */
unsigned char tr_present()
{
#asm
    mov ah,#0x1A
    int 0x61
    or al,al
    jz trpid_absent
    cmp ah,#PID_ID_CH375
    jne trpid_absent
    mov al,#1
    jmp trpid_done
trpid_absent:
    xor al,al
trpid_done:
#endasm
}

void tr_cmd(b)
unsigned char b;
{
#asm
    push bp
    mov bp,sp
    mov dx,#0x807D
    mov al,4[bp]
    out dx,al
    pop bp
#endasm
}

void tr_wdata(b)
unsigned char b;
{
#asm
    push bp
    mov bp,sp
    mov dx,#0x807E
    mov al,4[bp]
    out dx,al
    pop bp
#endasm
}

unsigned char tr_rdata()
{
#asm
    mov dx,#0x807E
    in al,dx
    xor ah,ah
#endasm
}

/* Command port read without a command: bit 7 = INT# (datasheet 6.2.1). */
unsigned char tr_token()
{
#asm
    mov dx,#0x807D
    in al,dx
    xor ah,ah
#endasm
}

/* src_seg:src_off -> data port 807Eh, len bytes. */
void tr_wdata_far(src_seg, src_off, len)
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
    jcxz trpid_wdata_far_done
trpid_wdata_far_loop:
    lodsb
    out dx,al
    loop trpid_wdata_far_loop
trpid_wdata_far_done:
    pop dx
    pop cx
    pop si
    pop ds
    pop bp
#endasm
}
