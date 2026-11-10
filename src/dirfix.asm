; DIRFIX.COM - standalone resident DIRFIX.

        org     100h

        jmp     install

%include "dirfix.inc"

msg_ok  db      "DIRFIX installed", 13, 10, "$"
msg_bad db      "DIRFIX: unsupported ROM", 13, 10, "$"

install:
        call    df_check
        jne     .bad

        call    df_hook

        mov     es, [2Ch]               ; free the environment
        mov     ah, 49h
        int     21h

        mov     dx, msg_ok
        mov     ah, 09h
        int     21h
        mov     dx, (df_resident_end - $$ + 100h + 15) / 16
        mov     ax, 3100h
        int     21h
.bad:
        mov     dx, msg_bad
        mov     ah, 09h
        int     21h
        mov     ax, 4C01h
        int     21h
