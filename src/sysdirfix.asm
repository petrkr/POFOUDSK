; DIRFIX in POFOUDSK.SYS. All df_ data is initialized .text - a .SYS's
; .bss is not zeroed and df_armed must start at 0.

        global  _dirfix_install

        section .text

%include "dirfix.inc"

; Returns AX=1 if installed, 0 if the ROM does not match. Preserves
; SI, DI, BP, DS, ES.
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
