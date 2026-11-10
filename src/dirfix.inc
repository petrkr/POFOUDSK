; DIRFIX - fix for the DIP DOS directory cluster bug (spc > 1).
;
; When DOS adds a cluster to a directory (MKDIR, or a new entry in a full
; subdirectory), ROM E52D:18C0 zeroes a whole cluster with `rep stosb`
; into the 512-byte work area SS:0CC2 and overruns DOS data, then writes
; it with one call to E52D:3F23 (DX = dir start cluster, BX:AX = byte
; offset, CX = length, ES:DI = source).
;
; INT 21h 39h/3Ch/16h/56h first look up the target drive's sectors per
; cluster (INT 21h AH=1Ch) - only spc > 1 can hit the bug, so a drive
; with spc <= 1 (or invalid) chains to the old INT 21h untraced. Only
; for spc > 1 do they run with TF set; the INT 1 handler
;   A) at the fill cuts the fill and the saved write length to one sector,
;   B) after DOS's write calls E52D:3F23 for the remaining spc-1 sectors.
; INT 61h returns with `retf 2`, dropping TF - it is hooked to hand the
; caller's TF back (code that cleared TF, e.g. a driver, stays unstepped).
; df_check verifies the ROM code first. All data is CS-relative.

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
df_count        db      0               ; sectors still to write after the first
df_bps          dw      0
df_offlo        dw      0               ; byte offset of the sector just written
df_offhi        dw      0
df_clus         dw      0               ; directory start cluster
df_saveax       dw      0
df_pip          dw      0               ; CS:IP of the previous trap (lds)
df_pcs          dw      0

df_int21:
        cmp     ah, 39h                 ; MKDIR
        je      .chkdrv
        cmp     ah, 3Ch                 ; create file
        je      .chkdrv
        cmp     ah, 16h                 ; FCB create
        je      .chkdrv
        cmp     ah, 56h                 ; rename (may move to another dir)
        je      .chkdrv
        jmp     far [cs:df_old21]
.chkdrv:
; Only spc > 1 can hit the bug and single-stepping DOS is slow: look up
; the drive's sectors per cluster (INT 21h AH=1Ch) and chain untraced
; for spc <= 1 or an invalid drive. INT 21h takes no input flags, so
; only the registers are restored (pops keep the flags of the test).
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    ds
        mov     si, dx                  ; DX cannot address memory
        mov     dl, 0                   ; default drive
        cmp     ah, 16h
        je      .fcb
        cmp     byte [si+1], ':'        ; ASCIIZ path with drive letter?
        jne     .havedrv
        mov     dl, [si]
        and     dl, 0DFh                ; uppercase
        sub     dl, 'A' - 1
        jmp     .havedrv
.fcb:
        mov     dl, [si]
        cmp     dl, 0FFh                ; extended FCB
        jne     .havedrv
        mov     dl, [si+7]
.havedrv:
        mov     ah, 1Ch                 ; -> AL = sectors per cluster
        int     21h
        sub     al, 2                   ; spc 2..254 -> 0..252 (trace)
        cmp     al, 0FDh                ; spc 0/1 -> FE/FF, invalid FFh -> FD
        pop     ds
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        jb      .arm
        jmp     far [cs:df_old21]
.arm:
        mov     [cs:df_saveax], ax
        mov     byte [cs:df_count], 0
        mov     word [cs:df_pcs], 0
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
        pushf                           ; DOS's result flags (CF) ...
        push    bp
        mov     bp, sp
        and     byte [bp+3], 0FEh       ; ... without TF - stepping ends
        pop     bp
        popf
        retf    2

df_int61:
        pushf
        call    far [cs:df_old61]       ; BIOS returns with retf 2
        pushf                           ; result flags for the caller ...
        push    bp
        mov     bp, sp
        test    byte [bp+9], 1          ; ... plus the caller's own TF
        jz      .out
        or      byte [bp+3], 1
.out:
        pop     bp
        popf
        retf    2

df_int1:
        push    bp
        mov     bp, sp                  ; [bp+2] IP, [bp+4] CS, [bp+6] flags
; The 8088 resumes an interrupted multi-prefix string instruction (e.g.
; ES: REP CMPSB) at its last prefix, dropping the others - with TF that
; happens after every repetition. If only prefixes lie between the
; previous trap's IP and this one, go back to the instruction start.
        push    ax
        push    si
        push    ds
        lds     si, [cs:df_pip]         ; DS:SI = previous trap CS:IP
        mov     ax, ds
        cmp     ax, [bp+4]
        jne     .newcs
        cmp     si, [bp+2]
        jae     .track
.prefix:
        lodsb
        mov     ah, al
        and     al, 0E7h
        cmp     al, 26h                 ; 26/2E/36/3E segment override
        je      .isprefix
        and     ah, 0FEh
        cmp     ah, 0F2h                ; F2/F3 rep
        jne     .track
.isprefix:
        cmp     si, [bp+2]
        jb      .prefix
        mov     ax, [cs:df_pip]         ; only prefixes - rewind
        mov     [bp+2], ax
        jmp     .track
.newcs:
        mov     ax, [bp+4]
        mov     [cs:df_pcs], ax
.track:
        mov     ax, [bp+2]
        mov     [cs:df_pip], ax
        cmp     ax, DF_FILL_IP          ; IP first - the CS test is rarely needed
        je      .rom
        cmp     ax, DF_AFTER_IP
        je      .rom
.pop:
        pop     ds
        pop     si
        pop     ax
.out:
        pop     bp
        iret
.rom:
        cmp     word [bp+4], DF_ROM_SEG
        jne     .pop
        cmp     ax, DF_FILL_IP
        pop     ds
        pop     si
        pop     ax
        je      .fill
        jmp     .after

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
        and     byte [bp+7], 0FEh       ; DOS continues unstepped
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

; ROM bytes at FILL_IP - 8 (mov di,0CC2h .. rep stosb .. call WRITE_IP)
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
