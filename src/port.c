/* 8086 I/O port access for BCC (K&R C)
 *
 * BCC has no built-in inp()/outp() - implemented here with the #asm
 * keyword instead. A function whose body is only an #asm block gets
 * no bp-frame prologue from BCC, so bp here is still the caller's -
 * push/set up our own frame first, then the usual 4[bp]/6[bp] arg
 * offsets apply.
 */

port_out(port, val)
unsigned port;
unsigned char val;
{
#asm
	push	bp
	mov	bp, sp
	mov	dx, 4[bp]
	mov	al, 6[bp]
	out	dx, al
	pop	bp
#endasm
}

port_in(port)
unsigned port;
{
#asm
	push	bp
	mov	bp, sp
	mov	dx, 4[bp]
	in	al, dx
	xor	ah, ah
	pop	bp
#endasm
}
