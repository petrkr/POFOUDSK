; DIRFIX core - resident fix for the Atari Portfolio DIP DOS directory
; cluster bug on drives with more than one sector per cluster.
;
; The bug (see ch375driver.md "Chyba MKDIR v DIP DOS"): whenever DOS
; adds a cluster to a directory - MKDIR (new directory) or a file /
; directory created in a full subdirectory (directory extension) - ROM
; routine E52D:189D zeroes a whole cluster (spc * bytes/sector) with
; `rep stosb` into a fixed 512-byte DOS work area at SS:0CC2
; (E52D:18C0), overrunning DOS data (DPBs, buffer headers, file table,
; ...) when spc > 1, then writes it with one call to E52D:3F23
; (DX = directory start cluster, BX:AX = byte offset, CX = length,
; ES:DI = source).
;
; Fix: the INT 21h calls that can add a directory cluster run with the
; trap flag set. The INT 1 handler:
;   A) at the `rep stosb` cuts the fill - and the length DOS saved for
;      the following write - down to one sector, so DOS zeroes 512
;      bytes and writes (and, for an extension, allocates) only the
;      first sector of the new cluster;
;   B) right after that write calls the same DOS write routine for the
;      remaining spc-1 sectors (offset + n * bytes/sector) from the
;      same zeroed work area, exactly as if DOS wrote sector by sector.
; Only DPB fields +2 (bytes/sector) and +4 (cluster mask) are used,
; both as the ROM itself uses them.
;
; INT 61h (Portfolio BIOS) returns with `retf 2`, which drops TF for
; its caller - the CH375 driver calls it on every request, DOS may too.
; INT 61h is therefore hooked as well and re-arms TF on its way back.
;
; ROM-version specific: df_check verifies the expected code is present
; before df_hook installs the handlers.
;
; Everything here is CS-relative (cs: overrides, no DS assumptions) -
; this file is included unchanged into both the standalone DIRFIX.COM
; (src/dirfix.asm) and the CH375.SYS driver glue (src/sysdirfix.asm).

DF_ROM_SEG      equ     0E52Dh          ; CS of the DOS routines below
DF_FILL_IP      equ     18C0h           ; rep stosb (cluster zero fill)
DF_AFTER_IP     equ     18C8h           ; just after call WRITE_IP
DF_WRITE_IP     equ     3F23h           ; DOS directory write routine
DF_RETF_IP      equ     03F3h           ; a CBh (retf) byte in ROM_SEG
DF_WORK_OFS     equ     0CC2h           ; DOS work area (DI at the fill)

DF_DPB_BPS      equ     2               ; bytes per sector
DF_DPB_MASK     equ     4               ; sectors per cluster - 1

; ---------------------------------------------------------------- resident

df_old21        dd      0
df_old61        dd      0
df_dosw         dw      DF_WRITE_IP, DF_ROM_SEG ; far pointer to the DOS write
df_retfip       dw      DF_RETF_IP
df_backip       dw      df_int1.ret
df_armed        db      0               ; 1 while the DOS call runs single-stepped
df_count        db      0               ; sectors still to write after the first
df_bps          dw      0
df_offlo        dw      0               ; byte offset of the sector just written
df_offhi        dw      0
df_clus         dw      0               ; directory start cluster
df_saveax       dw      0

df_int21:
        cmp     ah, 39h                 ; MKDIR
        je      .arm
        cmp     ah, 3Ch                 ; create file
        je      .arm
        cmp     ah, 16h                 ; FCB create
        je      .arm
        cmp     ah, 56h                 ; rename (may move to another dir)
        je      .arm
        jmp     far [cs:df_old21]
.arm:
        mov     [cs:df_saveax], ax
        mov     byte [cs:df_count], 0
        mov     byte [cs:df_armed], 1
        pushf                           ; DOS irets/retf 2s back to .back
        push    cs
        mov     ax, .back
        push    ax
        pushf                           ; enter DOS with TF set
        pop     ax
        or      ah, 1
        push    ax
        push    word [cs:df_old21+2]
        push    word [cs:df_old21]
        mov     ax, [cs:df_saveax]
        iret
.back:
        mov     byte [cs:df_armed], 0   ; next trap clears TF
        retf    2                       ; keep DOS's result flags (CF)

df_int61:
        cmp     byte [cs:df_armed], 0
        jne     .keep
        jmp     far [cs:df_old61]
.keep:
        pushf
        call    far [cs:df_old61]       ; BIOS returns with retf 2
        pushf                           ; result flags + TF for the caller
        push    bp
        mov     bp, sp
        or      byte [bp+3], 1
        pop     bp
        popf
        retf    2

df_int1:
        push    bp
        mov     bp, sp                  ; [bp+2] IP, [bp+4] CS, [bp+6] flags
        cmp     byte [cs:df_armed], 0
        je      .stop
        cmp     word [bp+4], DF_ROM_SEG
        jne     .out
        cmp     word [bp+2], DF_FILL_IP
        je      .fill
        cmp     word [bp+2], DF_AFTER_IP
        je      .after
        jmp     .out
.stop:
        and     byte [bp+7], 0FEh       ; clear TF
.out:
        pop     bp
        iret

; A) DS:SI = DPB, DX = dir start cluster, BX = offset high,
;    saved [bp+12] = offset low, saved [bp+10] = length, CX = length
.fill:
        cmp     di, DF_WORK_OFS         ; first trap at the fill only
        jne     .out
        cmp     cx, [si+DF_DPB_BPS]
        jbe     .out                    ; one sector per cluster - no bug
        push    ax
        mov     al, [si+DF_DPB_MASK]
        mov     [cs:df_count], al
        mov     ax, [si+DF_DPB_BPS]
        mov     [cs:df_bps], ax
        mov     cx, ax                  ; fill one sector only ...
        mov     [bp+10], ax             ; ... and write one
        mov     ax, [bp+12]
        mov     [cs:df_offlo], ax
        mov     [cs:df_offhi], bx
        mov     [cs:df_clus], dx
        pop     ax
        jmp     .out

; B) DOS wrote (and allocated) the first sector; write the rest.
.after:
        cmp     byte [cs:df_count], 0
        je      .out
        test    byte [bp+6], 1          ; DOS write failed - leave it
        jnz     .out
        mov     byte [cs:df_armed], 0   ; no more stepping / re-arming
        sti                             ; INT 1 entry cleared IF
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
.next:
        mov     ax, [cs:df_bps]
        add     [cs:df_offlo], ax
        adc     word [cs:df_offhi], 0
        push    cs                      ; far return to .ret via the
        push    word [cs:df_backip]     ; retf byte in ROM_SEG, since
        push    word [cs:df_retfip]     ; the DOS routine returns near
        mov     cx, ax
        mov     ax, [cs:df_offlo]
        mov     bx, [cs:df_offhi]
        mov     dx, [cs:df_clus]
        mov     di, ss
        mov     es, di
        mov     di, DF_WORK_OFS         ; still zeroed (first sector)
        jmp     far [cs:df_dosw]
.ret:
        jc      .fail
        dec     byte [cs:df_count]
        jnz     .next
        jmp     .done
.fail:
        or      byte [bp+6], 1          ; report the error as DOS's own
.done:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        jmp     .out

df_resident_end:

; ---------------------------------------------------------------- install

; Expected ROM bytes at FILL_IP - 8: mov di,0CC2h / push ax / push cx /
; push di / xor al,al / rep stosb / pop di / pop cx / pop ax /
; call WRITE_IP
df_sig  db      0BFh, 0C2h, 0Ch, 50h, 51h, 57h, 32h, 0C0h, 0F3h, 0AAh
        db      5Fh, 59h, 58h, 0E8h, 5Bh, 26h
DF_SIG_LEN equ  $ - df_sig

; df_check - ZF=1 iff the ROM signature at ROM_SEG:FILL_IP-8 matches
; and the retf byte at ROM_SEG:RETF_IP is CBh. Preserves all registers
; except flags; sets ES=CS internally (repe cmpsb needs ES:DI).
df_check:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     es
        mov     ax, DF_ROM_SEG
        mov     ds, ax
        mov     si, DF_FILL_IP - 8
        mov     di, df_sig
        mov     cx, DF_SIG_LEN
        cld
        repe    cmpsb
        jne     .done
        cmp     byte [DF_RETF_IP], 0CBh
.done:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

; df_hook - saves the old INT 21h/61h vectors and installs INT 1h/21h/
; 61h by writing the IVT directly at 0000:xxxx (cli/sti around it) -
; not via INT 21h/25h, since DOS 2.x does not allow most INT 21h
; functions during device driver INIT. Preserves all registers.
df_hook:
        push    ax
        push    es
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, [es:21h*4]
        mov     [cs:df_old21], ax
        mov     ax, [es:21h*4+2]
        mov     [cs:df_old21+2], ax
        mov     ax, [es:61h*4]
        mov     [cs:df_old61], ax
        mov     ax, [es:61h*4+2]
        mov     [cs:df_old61+2], ax
        mov     word [es:1*4], df_int1
        mov     word [es:1*4+2], cs
        mov     word [es:21h*4], df_int21
        mov     word [es:21h*4+2], cs
        mov     word [es:61h*4], df_int61
        mov     word [es:61h*4+2], cs
        sti
        pop     es
        pop     ax
        ret
