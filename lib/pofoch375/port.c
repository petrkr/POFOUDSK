/* Port I/O for BCC (no inp/outp). An #asm-only body gets no BCC
 * prologue, so each function sets up its own bp frame. */

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
