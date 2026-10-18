; vbechk.asm -- VESA BIOS Extension conformance (DOS, 386+).
;
; Logs the controller and every mode's information block (INFO lines the
; host compares with a known-good run), then for every supported packed
; (8 bpp) or direct-colour mode:
;   - sets it (4F02) and reads it back (4F03)
;   - writes a position-dependent pattern over the whole visible frame
;     buffer through the bank window (4F05) and verifies it
;   - checks 4F06 bytes per line against the mode block
;   - sets and reads back the display start (4F07)
;   - with a linear frame buffer: sets the mode with bit 14 and writes and
;     verifies the same pattern through the LFB (unreal mode; skipped
;     under EMM386/V86)
; then MEMSIZE (banks over all of TotalMemory, catches aliasing), DDC
; (4F15: EDID header and checksum) and the power-management and
; protected-mode interface reports.
; Switches: /Q information only, no mode sets; /B no LFB tests.
;
; Build: nasm -f bin -o VBECHK.COM vbechk.asm

        org     100h
        cpu     386

start:
        mov     si, logfile
        call    log_open
        mov     byte [echo], 0
        mov     si, s_banner
        call    linfo
        call    lputs
        call    lend

        ; ---- controller information -----------------------------------
        mov     dword [vbeinfo], 'VBE2'
        mov     di, vbeinfo
        mov     ax, 4F00h
        int     10h
        cmp     ax, 004Fh
        je      .vbeok
        mov     si, s_vbe
        call    lcase
        call    lfail
        mov     si, s_novbe
        call    lputs
        call    lend
        jmp     finish
.vbeok: mov     si, s_vbe
        call    lcase
        call    lpass
        mov     si, s_ver
        call    lputs
        movzx   eax, word [vbeinfo + 4]
        mov     cl, 4
        call    lhex
        mov     si, s_mem
        call    lputs
        movzx   eax, word [vbeinfo + 18]
        shl     eax, 6
        call    ldec
        mov     si, s_kb
        call    lputs
        mov     eax, [vbeinfo + 10]
        mov     cl, 8
        call    lhex
        call    lend
        mov     si, s_oem
        call    linfo
        call    lputs
        mov     eax, [vbeinfo + 6]
        call    lfar
        call    lend
        cmp     word [vbeinfo + 4], 0200h
        jb      .nov2
        mov     si, s_vendor
        call    linfo
        call    lputs
        mov     eax, [vbeinfo + 22]
        call    lfar
        mov     si, s_sep
        call    lputs
        mov     eax, [vbeinfo + 26]
        call    lfar
        mov     si, s_sep
        call    lputs
        mov     eax, [vbeinfo + 30]
        call    lfar
        mov     si, s_swrev
        call    lputs
        movzx   eax, word [vbeinfo + 20]
        mov     cl, 4
        call    lhex
        call    lend
.nov2:
        ; copy the mode list (it may live in the info block we reuse)
        mov     eax, [vbeinfo + 14]
        mov     si, ax
        shr     eax, 16
        mov     gs, ax
        xor     bx, bx
.cp:    mov     ax, [gs:si]
        mov     [modes + bx], ax
        cmp     ax, 0FFFFh
        je      .cpd
        add     si, 2
        add     bx, 2
        cmp     bx, 2 * 250
        jb      .cp
        mov     word [modes + bx], 0FFFFh
.cpd:
        ; can we use unreal mode for the LFB?
        smsw    ax
        test    al, 1
        jz      .real
        mov     byte [can_lfb], 0
        mov     si, s_v86
        call    linfo
        call    lputs
        call    lend
.real:  mov     al, 'B'
        call    has_switch
        jnz     .lfbsw
        mov     byte [can_lfb], 0
.lfbsw: mov     ax, 2401h               ; A20 on
        int     15h
        mov     al, 'Q'
        call    has_switch
        jnz     .tests
        mov     byte [infoonly], 1

.tests: mov     word [mptr], modes
.mode:  mov     bx, [mptr]
        mov     cx, [bx]
        cmp     cx, 0FFFFh
        je      .after
        add     word [mptr], 2
        mov     [curmode], cx
        call    mode_info
        jmp     .mode
.after:
        cmp     byte [infoonly], 0
        jne     .ddc
        call    t_memsize
.ddc:   call    t_ddc
        call    t_pm
finish:
        mov     ax, 3
        int     10h
        mov     byte [echo], 1
        call    ldone
        call    log_close
        mov     dx, s_where
        mov     ah, 9
        int     21h
        mov     al, [nfail]
        or      al, al
        jz      .x
        mov     al, 1
.x:     mov     ah, 4Ch
        int     21h

; ---- far string at eax (seg:off) -> line --------------------------------------
lfar:
        push    gs
        push    si
        push    cx
        mov     si, ax
        shr     eax, 16
        mov     gs, ax
        mov     cx, 80
.l:     mov     al, [gs:si]
        or      al, al
        jz      .x
        cmp     al, ' '
        jb      .n
        call    lputc
.n:     inc     si
        loop    .l
.x:     pop     cx
        pop     si
        pop     gs
        ret

; ---- one mode ------------------------------------------------------------------
mode_info:
        mov     di, minfo
        mov     cx, 128
        xor     ax, ax
        rep     stosw
        mov     cx, [curmode]
        mov     di, minfo
        mov     ax, 4F01h
        int     10h
        cmp     ax, 004Fh
        je      .ok
        mov     si, s_mi
        call    linfo
        call    lputs
        movzx   eax, word [curmode]
        mov     cl, 4
        call    lhex
        mov     si, s_mifail
        call    lputs
        call    lend
        inc     word [nfail]
        ret
.ok:    ; INFO mode MMMM attr AAAA WxHxBPP model M win A/B seg gran size bpl lfb ...
        mov     si, s_mi
        call    linfo
        call    lputs
        movzx   eax, word [curmode]
        mov     cl, 4
        call    lhex
        mov     si, s_attr
        call    lputs
        movzx   eax, word [minfo]
        call    lhex
        mov     al, ' '
        call    lputc
        movzx   eax, word [minfo + 18]
        call    ldec
        mov     al, 'x'
        call    lputc
        movzx   eax, word [minfo + 20]
        call    ldec
        mov     al, 'x'
        call    lputc
        movzx   eax, byte [minfo + 25]
        call    ldec
        mov     si, s_model
        call    lputs
        movzx   eax, byte [minfo + 27]
        call    ldec
        mov     si, s_win
        call    lputs
        movzx   eax, word [minfo + 2]
        mov     cl, 4
        call    lhex
        mov     al, ' '
        call    lputc
        movzx   eax, word [minfo + 8]
        call    lhex
        mov     si, s_gran
        call    lputs
        movzx   eax, word [minfo + 4]
        call    ldec
        mov     al, '/'
        call    lputc
        movzx   eax, word [minfo + 6]
        call    ldec
        mov     si, s_bpl
        call    lputs
        movzx   eax, word [minfo + 16]
        call    ldec
        mov     si, s_pages
        call    lputs
        movzx   eax, byte [minfo + 29]
        call    ldec
        mov     si, s_masks
        call    lputs
        mov     eax, [minfo + 31]
        mov     cl, 8
        call    lhex
        mov     al, ' '
        call    lputc
        movzx   eax, word [minfo + 35]
        mov     cl, 4
        call    lhex
        mov     si, s_lfb
        call    lputs
        mov     eax, [minfo + 40]
        mov     cl, 8
        call    lhex
        call    lend

        cmp     byte [infoonly], 0
        jne     .r
        ; testable: supported, graphics, packed or direct, >= 8 bpp
        test    word [minfo], 1
        jz      .r
        test    word [minfo], 10h
        jz      .r
        cmp     byte [minfo + 25], 8
        jb      .r
        mov     al, [minfo + 27]
        cmp     al, 4
        je      .t
        cmp     al, 6
        jne     .r
.t:     call    mode_test
.r:     ret

; pattern value for byte offset o: ((o * K) ^ seed)
K       equ     9E3779B1h
PATSTEP      equ     (K * 4) & 0FFFFFFFFh

errs    dd      0
bytes   dd      0
fails   db      0

mode_test:
        mov     byte [fails], 0
        mov     si, s_mcase
        call    lcase
        movzx   eax, word [curmode]
        mov     cl, 4
        call    lhex
        push    word [lptr]             ; result keyword goes here later
        mov     si, s_tmp
        call    lputs                   ; placeholder " ---- "
        ; total visible bytes
        movzx   eax, word [minfo + 16]
        movzx   edx, word [minfo + 20]
        imul    eax, edx
        mov     [bytes], eax
        ; set (banked) and read back the mode
        mov     bx, [curmode]
        and     bx, 01FFh
        mov     ax, 4F02h
        int     10h
        cmp     ax, 004Fh
        je      .set
        mov     si, s_setfail
        call    lputs
        mov     byte [fails], 1
        jmp     .res
.set:   mov     ax, 4F03h
        int     10h
        and     bx, 01FFh
        mov     ax, [curmode]
        and     ax, 01FFh
        cmp     bx, ax
        je      .get
        mov     si, s_getfail
        call    lputs
        mov     byte [fails], 1
.get:   ; bytes per scan line
        mov     ax, 4F06h
        mov     bl, 1
        int     10h
        cmp     ax, 004Fh
        jne     .bpl1
        cmp     bx, [minfo + 16]
        je      .bpl1
        mov     si, s_bplbad
        call    lputs
        movzx   eax, bx
        call    ldec
        mov     byte [fails], 1
.bpl1:  ; banked pattern
        call    banked_rw
        mov     si, s_banked
        call    lputs
        call    put_errs
        ; display start
        mov     ax, 4F07h
        xor     bx, bx
        mov     cx, 8
        mov     dx, 3
        int     10h
        cmp     ax, 004Fh
        jne     .nods
        mov     ax, 4F07h
        mov     bx, 1
        int     10h
        cmp     ax, 004Fh
        jne     .nods
        cmp     dx, 3
        jne     .dsbad
        cmp     cx, 8
        je      .dsok
        ; some BIOSes align x down; accept 0..8
        cmp     cx, 8
        jb      .dsok
.dsbad: mov     si, s_dsbad
        call    lputs
        movzx   eax, cx
        call    ldec
        mov     al, ','
        call    lputc
        movzx   eax, dx
        call    ldec
        mov     byte [fails], 1
        jmp     .dsx
.dsok:  mov     si, s_dsok
        call    lputs
.dsx:   mov     ax, 4F07h
        xor     bx, bx
        xor     cx, cx
        xor     dx, dx
        int     10h
        jmp     .lfbt
.nods:  mov     si, s_dsno
        call    lputs
.lfbt:  ; linear frame buffer
        test    word [minfo], 80h
        jz      .res
        cmp     dword [minfo + 40], 0
        je      .res
        cmp     byte [can_lfb], 0
        je      .res
        mov     bx, [curmode]
        and     bx, 01FFh
        or      bx, 4000h
        mov     ax, 4F02h
        int     10h
        cmp     ax, 004Fh
        je      .lset
        mov     si, s_lfbset
        call    lputs
        mov     byte [fails], 1
        jmp     .res
.lset:  call    lfb_rw
        mov     si, s_lfbt
        call    lputs
        call    put_errs
.res:   pop     di
        mov     si, s_passw
        cmp     byte [fails], 0
        je      .pw
        mov     si, s_failw
        inc     word [nfail]
        jmp     .cp
.pw:    inc     word [npass]
.cp:    mov     cx, 6
.cpl:   lodsb
        mov     [di], al
        inc     di
        loop    .cpl
        call    lend
        ret

; " ok" or " N errors" (and mark the case failed)
put_errs:
        cmp     dword [errs], 0
        jne     .e
        mov     si, s_ok
        jmp     lputs
.e:     mov     al, ' '
        call    lputc
        mov     eax, [errs]
        call    ldec
        mov     si, s_errs
        call    lputs
        mov     byte [fails], 1
        ret

; ---- banked access --------------------------------------------------------------
win     dw      0                       ; 0 = A, 1 = B
wseg    dw      0
gran    dd      0
wsize   dd      0
off     dd      0
acc     dd      0
pass    db      0

; set bank for byte offset [off]
setbank:
        mov     eax, [off]
        xor     edx, edx
        div     dword [gran]
        mov     dx, ax
        mov     bx, [win]
        mov     ax, 4F05h
        int     10h
        ret

; pattern write (pass 0) / verify (pass 1) over [bytes] through banks
banked_rw:
        mov     dword [errs], 0
        ; pick a window that is present, readable and writable
        mov     word [win], 0
        mov     ax, [minfo + 8]
        mov     [wseg], ax
        mov     al, [minfo + 2]
        and     al, 7
        cmp     al, 7
        je      .w
        mov     word [win], 1
        mov     ax, [minfo + 10]
        mov     [wseg], ax
.w:     movzx   eax, word [minfo + 4]
        shl     eax, 10
        or      eax, eax
        jnz     .g
        mov     eax, 10000h
.g:     mov     [gran], eax
        movzx   eax, word [minfo + 6]
        shl     eax, 10
        cmp     eax, 10000h
        jbe     .s
        mov     eax, 10000h
.s:     or      eax, eax
        jnz     .s2
        mov     eax, 10000h
.s2:    mov     [wsize], eax
        mov     byte [pass], 0
.pass:  mov     dword [off], 0
        mov     dword [acc], 0
        mov     es, [wseg]
.chunk: mov     eax, [off]
        cmp     eax, [bytes]
        jae     .pd
        call    setbank
        mov     es, [wseg]
        ; bytes in this window: min(wsize, bytes - off)
        mov     ecx, [bytes]
        sub     ecx, [off]
        cmp     ecx, [wsize]
        jbe     .c
        mov     ecx, [wsize]
.c:     add     [off], ecx
        shr     ecx, 2
        xor     di, di
        mov     ebx, [acc]
        cmp     byte [pass], 0
        jne     .rd
.wl:    mov     eax, ebx
        xor     eax, [curmode]
        mov     [es:di], eax
        add     di, 4
        add     ebx, PATSTEP
        dec     ecx
        jnz     .wl
        jmp     .nx
.rd:    mov     eax, ebx
        xor     eax, [curmode]
        cmp     [es:di], eax
        je      .rok
        inc     dword [errs]
.rok:   add     di, 4
        add     ebx, PATSTEP
        dec     ecx
        jnz     .rd
.nx:    mov     [acc], ebx
        jmp     .chunk
.pd:    inc     byte [pass]
        cmp     byte [pass], 2
        jb      .pass
        push    cs
        pop     es
        ret

; ---- linear frame buffer through unreal mode ----------------------------------------
gdt     dq      0
        dw      0FFFFh, 0
        db      0, 92h, 0CFh, 0
gdtr    dw      15
        dd      0

unreal:
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, gdt
        mov     [gdtr + 2], eax
        cli
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     short .pm
.pm:    mov     bx, 8
        mov     fs, bx
        and     al, 0FEh
        mov     cr0, eax
        jmp     short .rm
.rm:    xor     bx, bx
        mov     fs, bx
        sti
        ret

lfb_rw:
        mov     dword [errs], 0
        call    unreal
        mov     byte [pass], 0
.pass:  mov     esi, [minfo + 40]
        mov     ecx, [bytes]
        shr     ecx, 2
        xor     ebx, ebx
        cmp     byte [pass], 0
        jne     .rd
.wl:    mov     eax, ebx
        xor     eax, [curmode]
        not     eax                     ; differ from the banked pass
        mov     [fs:esi], eax
        add     esi, 4
        add     ebx, PATSTEP
        dec     ecx
        jnz     .wl
        jmp     .nx
.rd:    mov     eax, ebx
        xor     eax, [curmode]
        not     eax
        cmp     [fs:esi], eax
        je      .rok
        inc     dword [errs]
.rok:   add     esi, 4
        add     ebx, PATSTEP
        dec     ecx
        jnz     .rd
.nx:    inc     byte [pass]
        cmp     byte [pass], 2
        jb      .pass
        ret

; ---- MEMSIZE: banks across all of TotalMemory in an 8 bpp mode ---------------------
t_memsize:
        ; first supported 8 bpp packed mode with a window
        mov     word [mptr], modes
.f:     mov     bx, [mptr]
        mov     cx, [bx]
        cmp     cx, 0FFFFh
        je      .none
        add     word [mptr], 2
        mov     [curmode], cx
        mov     di, minfo
        mov     ax, 4F01h
        int     10h
        cmp     ax, 004Fh
        jne     .f
        mov     ax, [minfo]
        and     ax, 11h
        cmp     ax, 11h
        jne     .f
        cmp     byte [minfo + 25], 8
        jne     .f
        cmp     byte [minfo + 27], 4
        jne     .f
        mov     bx, cx
        and     bx, 01FFh
        mov     ax, 4F02h
        int     10h
        cmp     ax, 004Fh
        jne     .f
        movzx   eax, word [vbeinfo + 18]
        shl     eax, 16
        mov     [bytes], eax
        call    banked_rw
        mov     si, s_msz
        call    lcase
        cmp     dword [errs], 0
        jne     .bad
        call    lpass
        jmp     .rep
.bad:   call    lfail
.rep:   mov     eax, [bytes]
        shr     eax, 10
        call    ldec
        mov     si, s_kbmode
        call    lputs
        movzx   eax, word [curmode]
        mov     cl, 4
        call    lhex
        cmp     dword [errs], 0
        je      .e
        mov     al, ' '
        call    lputc
        mov     eax, [errs]
        call    ldec
        mov     si, s_errs2
        call    lputs
.e:     call    lend
        ret
.none:  mov     si, s_msz
        call    lcase
        call    lskip
        mov     si, s_no8
        call    lputs
        call    lend
        ret

; ---- DDC ----------------------------------------------------------------------------
t_ddc:
        mov     ax, 4F15h
        xor     bx, bx
        xor     cx, cx
        push    cs
        pop     es
        xor     di, di
        int     10h
        cmp     ax, 004Fh
        jne     .no
        mov     di, edid
        mov     cx, 64
        xor     ax, ax
        rep     stosw
        mov     ax, 4F15h
        mov     bx, 1
        xor     cx, cx
        xor     dx, dx
        mov     di, edid
        int     10h
        cmp     ax, 004Fh
        jne     .rdf
        ; header 00 FF FF FF FF FF FF 00 and a zero byte sum
        cmp     dword [edid], 0FFFFFF00h
        jne     .bad
        cmp     dword [edid + 4], 00FFFFFFh
        jne     .bad
        xor     al, al
        mov     si, edid
        mov     cx, 128
.sum:   add     al, [si]
        inc     si
        loop    .sum
        or      al, al
        jnz     .bad
        mov     si, s_ddc
        call    lcase
        call    lpass
        ; manufacturer: three 5-bit letters, big-endian word at 8
        mov     ax, [edid + 8]
        xchg    al, ah
        mov     bx, ax
        mov     cl, 10
        shr     ax, cl
        and     al, 1Fh
        add     al, '@'
        call    lputc
        mov     ax, bx
        shr     ax, 5
        and     al, 1Fh
        add     al, '@'
        call    lputc
        mov     al, bl
        and     al, 1Fh
        add     al, '@'
        call    lputc
        mov     al, ' '
        call    lputc
        movzx   eax, word [edid + 10]
        mov     cl, 4
        call    lhex
        mov     si, s_pref
        call    lputs
        movzx   eax, byte [edid + 58]
        shr     al, 4
        shl     eax, 8
        mov     al, [edid + 56]
        call    ldec
        mov     al, 'x'
        call    lputc
        movzx   eax, byte [edid + 61]
        shr     al, 4
        shl     eax, 8
        mov     al, [edid + 59]
        call    ldec
        call    lend
        ret
.bad:   mov     si, s_ddc
        call    lcase
        call    lfail
        mov     si, s_ddcbad
        call    lputs
        call    lend
        ret
.rdf:   mov     si, s_ddc
        call    lcase
        call    lfail
        mov     si, s_ddcrd
        call    lputs
        call    lend
        ret
.no:    mov     si, s_ddc
        call    lcase
        call    lskip
        mov     si, s_ddcno
        call    lputs
        call    lend
        ret

; ---- power management / PM interface (INFO) -------------------------------------------
t_pm:
        mov     ax, 4F10h
        xor     bx, bx
        push    cs
        pop     es
        xor     di, di
        int     10h
        mov     si, s_pmi
        call    linfo
        call    lputs
        cmp     ax, 004Fh
        jne     .n1
        movzx   eax, bh
        mov     cl, 2
        call    lhex
        jmp     .p2
.n1:    mov     si, s_unsup
        call    lputs
.p2:    call    lend
        mov     ax, 4F0Ah
        xor     bx, bx
        int     10h
        push    cs
        pop     es
        mov     si, s_pmif
        call    linfo
        call    lputs
        cmp     ax, 004Fh
        jne     .n2
        movzx   eax, cx
        call    ldec
        mov     si, s_bytes
        call    lputs
        jmp     .e
.n2:    mov     si, s_unsup
        call    lputs
.e:     call    lend
        ret

; ---- data ------------------------------------------------------------------------------
can_lfb  db     1
infoonly db     0
curmode  dd     0
mptr     dw     0
logfile  db     'C:\CTRES\VBECHK.LOG', 0
s_banner db     'VBECHK CARDTEST 1.0', 0
s_where  db     'Results in C:\CTRES\VBECHK.LOG', 13, 10, '$'
s_vbe    db     'VBEINFO', 0
s_novbe  db     '4F00 not supported', 0
s_ver    db     'version ', 0
s_mem    db     ' memory ', 0
s_kb     db     ' KB caps ', 0
s_oem    db     'oem ', 0
s_vendor db     'vendor ', 0
s_sep    db     ' | ', 0
s_swrev  db     ' | rev ', 0
s_v86    db     'V86 mode (EMM386?): LFB tests skipped', 0
s_mi     db     'mode ', 0
s_mifail db     ' 4F01 failed', 0
s_attr   db     ' attr ', 0
s_model  db     ' model ', 0
s_win    db     ' win ', 0
s_gran   db     ' gran/size ', 0
s_bpl    db     ' bpl ', 0
s_pages  db     ' pages ', 0
s_masks  db     ' masks ', 0
s_lfb    db     ' lfb ', 0
s_mcase  db     'M', 0
s_tmp    db     ' ---- ', 0
s_passw  db     ' PASS '
s_failw  db     ' FAIL '
s_setfail db    ' set failed', 0
s_getfail db    ' 4F03 mismatch', 0
s_bplbad db     ' 4F06 bpl ', 0
s_banked db     'banked', 0
s_ok     db     ' ok', 0
s_errs   db     ' bad dwords', 0
s_dsbad  db     ' start read back ', 0
s_dsok   db     ' start ok', 0
s_dsno   db     ' start n/a', 0
s_lfbset db     ' lfb set failed', 0
s_lfbt   db     ' lfb', 0
s_msz    db     'MEMSIZE', 0
s_kbmode db     ' KB via mode ', 0
s_errs2  db     ' bad dwords (aliasing or short memory)', 0
s_no8    db     'no 8 bpp banked mode', 0
s_ddc    db     'DDC', 0
s_pref   db     ' preferred ', 0
s_ddcbad db     'EDID header or checksum wrong', 0
s_ddcrd  db     '4F15 read failed', 0
s_ddcno  db     '4F15 not supported', 0
s_pmi    db     'pm 4F10 states ', 0
s_pmif   db     'pm interface 4F0A ', 0
s_bytes  db     ' bytes', 0
s_unsup  db     'not supported', 0

%include "lib.inc"

        align   4
vbeinfo  times 512 db 0
minfo    times 256 db 0
edid     times 128 db 0
modes    times 252 dw 0
