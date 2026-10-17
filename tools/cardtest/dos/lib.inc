; lib.inc -- shared code for the CARDTEST DOS tools (.COM, 386+ real mode).
;
; Logging: build a line with the l* routines, then lend writes it with CRLF
; to C:\CTRES\<TOOL>.LOG and, while 'echo' is set, to the screen.
; Line keywords match the Windows tools (INFO / CASE name PASS|FAIL|SKIP|
; VISUAL / DONE).

        cpu     386

; ---- data -------------------------------------------------------------
logname     times 32 db 0
loghandle   dw 0
echo        db 1
npass       dw 0
nfail       dw 0
nskip       dw 0
lptr        dw linebuf
linebuf     times 300 db 0
ctresdir    db 'C:\CTRES', 0
s_case      db 'CASE  ', 0
s_info      db 'INFO  ', 0
s_pass      db ' PASS ', 0
s_fail      db ' FAIL ', 0
s_skip      db ' SKIP ', 0
s_done      db 'DONE  pass=', 0
s_dfail     db ' fail=', 0
s_dskip     db ' skip=', 0
hexdig      db '0123456789ABCDEF'

; ---- log file -----------------------------------------------------------
; si = file name ("C:\CTRES\X.LOG", 0)
log_open:
        push    si
        mov     dx, ctresdir
        mov     ah, 39h                 ; mkdir (fails harmlessly if present)
        int     21h
        pop     dx
        xor     cx, cx
        mov     ah, 3Ch                 ; create/truncate
        int     21h
        jnc     .ok
        xor     ax, ax
.ok:    mov     [loghandle], ax
        ret

log_close:
        mov     bx, [loghandle]
        or      bx, bx
        jz      .r
        mov     ah, 3Eh
        int     21h
.r:     ret

; ---- line building ----------------------------------------------------------
lreset:
        mov     word [lptr], linebuf
        ret

; append the zero-terminated string at ds:si
lputs:
        push    ax
        push    di
        mov     di, [lptr]
.l:     lodsb
        or      al, al
        jz      .e
        mov     [di], al
        inc     di
        cmp     di, linebuf + 290
        jb      .l
.e:     mov     [lptr], di
        pop     di
        pop     ax
        ret

; append the character in al
lputc:
        push    di
        mov     di, [lptr]
        mov     [di], al
        inc     di
        mov     [lptr], di
        pop     di
        ret

; append eax as cl hex digits
lhex:
        pushad
        mov     di, [lptr]
        movzx   cx, cl
        mov     bx, cx
.l:     mov     edx, eax
        and     dx, 0Fh
        push    bx
        mov     bx, dx
        mov     dl, [hexdig + bx]
        pop     bx
        mov     [di + bx - 1], dl
        shr     eax, 4
        dec     bx
        jnz     .l
        add     di, cx
        mov     [lptr], di
        popad
        ret

; append eax as unsigned decimal
ldec:
        pushad
        mov     ebx, 10
        xor     cx, cx
.d:     xor     edx, edx
        div     ebx
        push    dx
        inc     cx
        or      eax, eax
        jnz     .d
.o:     pop     ax
        add     al, '0'
        call    lputc
        loop    .o
        popad
        ret

; terminate the line: write it (+CRLF) to the log and maybe the screen
lend:
        pushad
        mov     di, [lptr]
        mov     word [di], 0A0Dh
        add     di, 2
        mov     cx, di
        sub     cx, linebuf
        mov     bx, [loghandle]
        or      bx, bx
        jz      .scr
        push    cx
        mov     dx, linebuf
        mov     ah, 40h
        int     21h
        pop     cx
.scr:   cmp     byte [echo], 0
        je      .x
        mov     si, linebuf
.p:     lodsb
        mov     dl, al
        mov     ah, 02h
        int     21h
        loop    .p
.x:     call    lreset
        popad
        ret

; "CASE  <name> " -- si = name
lcase:
        push    si
        call    lreset
        mov     si, s_case
        call    lputs
        pop     si
        call    lputs
        ret

; "INFO  "
linfo:
        push    si
        call    lreset
        mov     si, s_info
        call    lputs
        pop     si
        ret

; append " PASS " / " FAIL " / " SKIP " and count it
lpass:  inc     word [npass]
        push    si
        mov     si, s_pass
        call    lputs
        pop     si
        ret
lfail:  inc     word [nfail]
        push    si
        mov     si, s_fail
        call    lputs
        pop     si
        ret
lskip:  inc     word [nskip]
        push    si
        mov     si, s_skip
        call    lputs
        pop     si
        ret

ldone:
        call    lreset
        mov     si, s_done
        call    lputs
        movzx   eax, word [npass]
        call    ldec
        mov     si, s_dfail
        call    lputs
        movzx   eax, word [nfail]
        call    ldec
        mov     si, s_dskip
        call    lputs
        movzx   eax, word [nskip]
        call    ldec
        call    lend
        ret

; ---- CRC-32 -------------------------------------------------------------
crctab  times 256 dd 0

crc_init:
        xor     bx, bx
.n:     movzx   eax, bx
        mov     cx, 8
.b:     shr     eax, 1
        jnc     .z
        xor     eax, 0EDB88320h
.z:     loop    .b
        push    bx
        shl     bx, 2
        mov     [crctab + bx], eax
        pop     bx
        inc     bx
        cmp     bx, 256
        jb      .n
        ret

; edx = running CRC (start with 0FFFFFFFFh), es:si = data, cx = length
crc_update:
        push    ax
        push    bx
        push    si
        push    cx
        jcxz    .x
.l:     mov     al, [es:si]
        inc     si
        xor     al, dl
        movzx   bx, al
        shl     bx, 2
        shr     edx, 8
        xor     edx, [crctab + bx]
        loop    .l
.x:     pop     cx
        pop     si
        pop     bx
        pop     ax
        ret

; ---- misc ---------------------------------------------------------------
; command line switch test: al = upper-case letter after '/', ZF=1 if present
has_switch:
        push    si
        push    cx
        mov     si, 81h
        movzx   cx, byte [80h]
.l:     jcxz    .no
        cmp     byte [si], '/'
        jne     .n
        mov     ah, [si + 1]
        and     ah, 0DFh
        cmp     ah, al
        je      .yes
.n:     inc     si
        dec     cx
        jmp     .l
.no:    or      sp, sp                  ; ZF=0
        jmp     .r
.yes:   cmp     al, al                  ; ZF=1
.r:     pop     cx
        pop     si
        ret

; wait for a key (BIOS)
getkey:
        xor     ax, ax
        int     16h
        ret

; set a BIOS video mode (al)
setmode:
        xor     ah, ah
        int     10h
        ret

; BIOS tick count -> eax
ticks:
        push    es
        push    word 40h
        pop     es
        mov     eax, [es:6Ch]
        pop     es
        ret
