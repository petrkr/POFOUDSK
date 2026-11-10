; DIRFIX glue for CH375.SYS - links the shared fix (dirfix.inc) into
; the driver. Tiny model (CS=DS for the whole .SYS), so all df_ data
; stays in .text as initialized bytes - a .SYS's .bss is never zeroed,
; and df_armed must start out 0 (see sys.h / ch375sys.c).
;
; Build: nasm -f as86 -I src/ -o sysdirfix.o sysdirfix.asm

        global  _dirfix_install

        section .text

%include "dirfix.inc"

; _dirfix_install - called from ch375sys.c's cmd_init(). Returns AX=0
; if the ROM signature does not match (df_check failed, nothing
; installed), else installs the fix (df_hook) and returns AX=1.
; Preserves SI, DI, BP, DS, ES (BCC calling convention).
_dirfix_install:
        push    si
        push    di
        push    bp
        push    ds
        push    es

        call    df_check
        jne     .no
        call    df_hook
        mov     ax, 1
        jmp     .done
.no:
        xor     ax, ax
.done:
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        ret
