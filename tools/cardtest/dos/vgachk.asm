; vgachk.asm -- VGA conformance for any VGA-compatible card (DOS, 386+).
;
; Self-checking cases (PASS/FAIL against the IBM VGA specification):
;   REGS     read-back of every writable bit in CRTC/SEQ/GC/ATTR
;   DAC      256-entry palette write/read, auto-increment, PEL mask, state
;   ALU      4000 random planar writes (write modes 0-3, rotate, set/reset,
;            function, bit mask, map mask) checked plane by plane
;   RDMODE1  read mode 1 colour compare / don't care
;   TEXTMEM  text-mode memory and the cursor location registers
;   RETRACE  vertical refresh rate in modes 3, 12h, 13h
; Golden-comparable (INFO lines the host compares with a reference card):
;   the register state the BIOS programs for every standard mode, the
;   8x16 font CRC.
; /V adds VISUAL screens (palette, split screen + panning, text
; attributes) that wait for a key -- take a host screenshot of each.
;
; Build: nasm -f bin -o VGACHK.COM vgachk.asm

        org     100h
        cpu     386

start:
        call    crc_init
        mov     si, logfile
        call    log_open
        mov     byte [echo], 0
        mov     si, s_banner
        call    linfo
        call    lputs
        call    lend

        call    t_regs
        call    t_dac
        call    t_alu
        call    t_text
        call    t_retrace
        call    t_modedump
        mov     al, 'V'
        call    has_switch
        jnz     .novis
        call    t_visual
.novis:
        mov     al, 3
        call    setmode
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

; ---- register helpers -------------------------------------------------------
; dx = index port, al = index -> al = value
rdidx:
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        ret
; dx = index port, al = index, ah = value
wridx:
        out     dx, ax
        ret

; attribute controller: al = index -> al = value (PAS cleared)
rdattr:
        push    dx
        push    ax
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        pop     ax
        out     dx, al
        inc     dx
        in      al, dx
        pop     dx
        ret
; al = index, ah = value
wrattr:
        push    dx
        push    ax
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        pop     ax
        out     dx, al
        mov     al, ah
        out     dx, al
        pop     dx
        ret
attr_on:
        push    dx
        mov     dx, 3DAh
        in      al, dx
        mov     dx, 3C0h
        mov     al, 20h
        out     dx, al
        pop     dx
        ret

; ---- REGS -------------------------------------------------------------------
; table: port, index, mask (port 0 = attribute controller), end = ffff
regtab:
%assign i 0
%rep 25
        dw      3D4h
        db      i, 0FFh
%assign i i+1
%endrep
        dw      3C4h
        db      1, 3Dh
        dw      3C4h
        db      2, 0Fh
        dw      3C4h
        db      3, 3Fh
        dw      3C4h
        db      4, 0Eh
        dw      3CEh
        db      0, 0Fh
        dw      3CEh
        db      1, 0Fh
        dw      3CEh
        db      2, 0Fh
        dw      3CEh
        db      3, 1Fh
        dw      3CEh
        db      4, 03h
        dw      3CEh
        db      5, 7Bh
        dw      3CEh
        db      6, 0Fh
        dw      3CEh
        db      7, 0Fh
        dw      3CEh
        db      8, 0FFh
%assign i 0
%rep 16
        dw      0
        db      i, 3Fh
%assign i i+1
%endrep
        dw      0
        db      10h, 0EFh
        dw      0
        db      11h, 0FFh
        dw      0
        db      12h, 3Fh
        dw      0
        db      13h, 0Fh
        dw      0
        db      14h, 0Fh
        dw      0FFFFh

pats    db      55h, 0AAh, 0FFh, 00h, 0A5h
errs    dw      0
regorig db      0
regmask db      0
regpat  db      0

; read register (bx -> table entry) into al
reg_read:
        mov     dx, [bx]
        mov     al, [bx + 2]
        or      dx, dx
        jz      .a
        jmp     rdidx
.a:     jmp     rdattr
; write al to register at table entry bx
reg_write:
        mov     ah, al
        mov     dx, [bx]
        mov     al, [bx + 2]
        or      dx, dx
        jz      .a
        jmp     wridx
.a:     jmp     wrattr

t_regs:
        mov     al, 12h
        call    setmode
        mov     word [errs], 0
        ; unlock CRTC 0-7
        mov     dx, 3D4h
        mov     al, 11h
        call    rdidx
        and     al, 7Fh
        mov     ah, al
        mov     al, 11h
        call    wridx
        mov     bx, regtab
.ent:   cmp     word [bx], 0FFFFh
        je      .done
        call    reg_read
        mov     [regorig], al
        mov     al, [bx + 3]
        mov     [regmask], al
        ; CR11: never set the protect bit or enable the vertical interrupt
        cmp     word [bx], 3D4h
        jne     .np
        cmp     byte [bx + 2], 11h
        jne     .np
        mov     byte [regmask], 4Fh
.np:    xor     si, si
.pat:   mov     al, [pats + si]
        and     al, [regmask]
        mov     ah, [regmask]
        not     ah
        and     ah, [regorig]
        or      al, ah
        mov     [regpat], al
        call    reg_write
        call    reg_read
        xor     al, [regpat]
        and     al, [regmask]
        jz      .ok
        call    reg_error
.ok:    inc     si
        cmp     si, 5
        jb      .pat
        mov     al, [regorig]
        cmp     word [bx], 3D4h
        jne     .rst
        cmp     byte [bx + 2], 11h
        jne     .rst
        and     al, 7Fh
.rst:   call    reg_write
        add     bx, 4
        jmp     .ent
.done:  call    attr_on
        mov     si, s_regs
        call    lcase
        cmp     word [errs], 0
        jne     .f
        call    lpass
        jmp     .e
.f:     call    lfail
        movzx   eax, word [errs]
        call    ldec
        mov     si, s_mism
        call    lputs
.e:     call    lend
        ret

; log one register mismatch (first 12 only); bx = entry, al = bad bits
reg_error:
        pushad
        inc     word [errs]
        cmp     word [errs], 12
        ja      .x
        push    ax
        mov     si, s_regerr
        call    linfo
        call    lputs
        movzx   eax, word [bx]
        mov     cl, 3
        call    lhex
        mov     al, '.'
        call    lputc
        movzx   eax, byte [bx + 2]
        mov     cl, 2
        call    lhex
        mov     si, s_wrote
        call    lputs
        movzx   eax, byte [regpat]
        call    lhex
        mov     si, s_badbits
        call    lputs
        pop     ax
        movzx   eax, al
        mov     cl, 2
        call    lhex
        call    lend
.x:     popad
        ret

; ---- DAC --------------------------------------------------------------------
dacval:                                 ; bl = index, cl = component -> al
        mov     al, bl
        mov     ah, cl
        shl     ah, 2
        add     al, ah
        imul    ax, ax, 13
        xor     al, bl
        and     al, 3Fh
        ret

t_dac:
        mov     word [errs], 0
        mov     dx, 3C8h
        xor     al, al
        out     dx, al
        inc     dx
        xor     bx, bx
.w:     xor     cl, cl
.wc:    call    dacval
        out     dx, al
        inc     cl
        cmp     cl, 3
        jb      .wc
        inc     bl
        jnz     .w
        ; state register after a write-index: 00b (write mode)
        mov     dx, 3C7h
        in      al, dx
        and     al, 3
        jz      .s1
        inc     word [errs]
.s1:    mov     dx, 3C7h
        xor     al, al
        out     dx, al
        in      al, dx                  ; after a read-index: 11b
        and     al, 3
        cmp     al, 3
        je      .s2
        inc     word [errs]
.s2:    mov     dx, 3C9h
        xor     bx, bx
.r:     xor     cl, cl
.rc:    in      al, dx
        and     al, 3Fh
        mov     ch, al
        call    dacval
        cmp     al, ch
        je      .ok
        inc     word [errs]
.ok:    inc     cl
        cmp     cl, 3
        jb      .rc
        inc     bl
        jnz     .r
        ; read starting mid-table
        mov     dx, 3C7h
        mov     al, 0A7h
        out     dx, al
        mov     dx, 3C9h
        mov     bl, 0A7h
        xor     cl, cl
.m:     in      al, dx
        and     al, 3Fh
        mov     ch, al
        call    dacval
        cmp     al, ch
        je      .mo
        inc     word [errs]
.mo:    inc     cl
        cmp     cl, 3
        jb      .m
        ; PEL mask
        mov     dx, 3C6h
        mov     al, 5Ah
        out     dx, al
        in      al, dx
        cmp     al, 5Ah
        je      .pm
        inc     word [errs]
.pm:    mov     al, 0FFh
        out     dx, al
        mov     si, s_dac
        call    lcase
        cmp     word [errs], 0
        jne     .f
        call    lpass
        jmp     .e
.f:     call    lfail
        movzx   eax, word [errs]
        call    ldec
        mov     si, s_mism
        call    lputs
.e:     call    lend
        ret

; ---- ALU / RDMODE1 ----------------------------------------------------------
seed    dd      12345678h
pv      db      0, 0, 0, 0              ; plane values before the write
got     db      0, 0, 0, 0
p_sr    db      0
p_esr   db      0
p_fn    db      0
p_rot   db      0
p_bm    db      0
p_wm    db      0
p_mm    db      0
p_cpu   db      0
p_addr  dw      0
errs2   dw      0

rand:                                   ; -> eax (xorshift32)
        mov     eax, [seed]
        mov     edx, eax
        shl     edx, 13
        xor     eax, edx
        mov     edx, eax
        shr     edx, 17
        xor     eax, edx
        mov     edx, eax
        shl     edx, 5
        xor     eax, edx
        mov     [seed], eax
        ret

gc:                                     ; al = index, ah = value
        push    dx
        mov     dx, 3CEh
        out     dx, ax
        pop     dx
        ret
sq:
        push    dx
        mov     dx, 3C4h
        out     dx, ax
        pop     dx
        ret

; expected value of plane cl (0-3) -> al
expect:
        push    bx
        push    cx
        movzx   bx, cl
        mov     bh, [pv + bx]           ; bh = latch
        mov     ch, 1
        shl     ch, cl                  ; ch = plane bit
        test    [p_mm], ch
        jz      .keep
        mov     dl, [p_bm]              ; dl = effective bit mask
        mov     al, [p_wm]
        cmp     al, 1
        je      .keep
        cmp     al, 2
        je      .m2
        cmp     al, 3
        je      .m3
        ; mode 0
        mov     al, [p_cpu]
        push    cx
        mov     cl, [p_rot]
        ror     al, cl
        pop     cx
        test    [p_esr], ch
        jz      .alu
        xor     al, al
        test    [p_sr], ch
        jz      .alu
        mov     al, 0FFh
        jmp     .alu
.m2:    xor     al, al
        test    [p_cpu], ch
        jz      .alu
        mov     al, 0FFh
        jmp     .alu
.m3:    mov     al, [p_cpu]
        push    cx
        mov     cl, [p_rot]
        ror     al, cl
        pop     cx
        and     dl, al
        xor     al, al
        test    [p_sr], ch
        jz      .alu
        mov     al, 0FFh
.alu:   mov     ah, [p_fn]
        cmp     ah, 1
        jne     .f2
        and     al, bh
        jmp     .bm
.f2:    cmp     ah, 2
        jne     .f3
        or      al, bh
        jmp     .bm
.f3:    cmp     ah, 3
        jne     .bm
        xor     al, bh
.bm:    and     al, dl
        not     dl
        and     dl, bh
        or      al, dl
        jmp     .x
.keep:  mov     al, bh
.x:     pop     cx
        pop     bx
        ret

t_alu:
        mov     al, 12h
        call    setmode
        push    word 0A000h
        pop     es
        mov     word [errs], 0
        mov     word [errs2], 0
        mov     bp, 4000
.it:    call    rand
        mov     [pv], eax
        call    rand
        mov     [p_sr], al
        and     byte [p_sr], 0Fh
        mov     [p_esr], ah
        and     byte [p_esr], 0Fh
        shr     eax, 16
        mov     [p_bm], al
        mov     [p_cpu], ah
        call    rand
        mov     dl, al
        and     dl, 3
        mov     [p_fn], dl
        mov     dl, al
        shr     dl, 2
        and     dl, 7
        mov     [p_rot], dl
        mov     dl, al
        shr     dl, 5
        and     dl, 3
        mov     [p_wm], dl
        mov     [p_mm], ah
        and     byte [p_mm], 0Fh
        shr     eax, 16
        and     ax, 3FFh
        add     ax, 8000h
        mov     [p_addr], ax
        mov     di, ax
        ; plain writes of the four plane values
        mov     ax, 0005h
        call    gc
        mov     ax, 0003h
        call    gc
        mov     ax, 0001h
        call    gc
        mov     ax, 0FF08h
        call    gc
        xor     bx, bx
.pl:    mov     ah, 1
        mov     cl, bl
        shl     ah, cl
        mov     al, 2
        call    sq
        mov     al, [pv + bx]
        mov     [es:di], al
        inc     bx
        cmp     bx, 4
        jb      .pl
        ; load the latches, program the operation, write
        mov     ax, 0F02h
        call    sq
        mov     al, [es:di]
        mov     al, 0
        mov     ah, [p_sr]
        call    gc
        mov     al, 1
        mov     ah, [p_esr]
        call    gc
        mov     ah, [p_fn]
        shl     ah, 3
        or      ah, [p_rot]
        mov     al, 3
        call    gc
        mov     al, 8
        mov     ah, [p_bm]
        call    gc
        mov     al, 5
        mov     ah, [p_wm]
        call    gc
        mov     al, 2
        mov     ah, [p_mm]
        call    sq
        mov     al, [p_cpu]
        mov     [es:di], al
        ; read back each plane
        mov     ax, 0005h
        call    gc
        mov     ax, 0003h
        call    gc
        mov     ax, 0FF08h
        call    gc
        xor     bx, bx
.rb:    mov     ah, bl
        mov     al, 4
        call    gc
        mov     al, [es:di]
        mov     [got + bx], al
        inc     bx
        cmp     bx, 4
        jb      .rb
        xor     cx, cx
.cmp:   call    expect
        mov     bx, cx
        cmp     al, [got + bx]
        je      .cok
        call    alu_error
        jmp     .rm1
.cok:   inc     cx
        cmp     cx, 4
        jb      .cmp
        ; read mode 1 on what is now in memory
.rm1:   mov     eax, [got]
        mov     [pv], eax
        call    rand
        and     al, 0Fh
        and     ah, 0Fh
        mov     [p_sr], al              ; colour compare
        mov     [p_esr], ah             ; colour don't care
        mov     al, 2
        mov     ah, [p_sr]
        call    gc
        mov     al, 7
        mov     ah, [p_esr]
        call    gc
        mov     ax, 0805h
        call    gc
        mov     al, [es:di]
        mov     dh, al
        ; expected: bit set where every cared plane matches its compare bit
        mov     dl, 0FFh
        xor     bx, bx
.e1:    mov     ah, 1
        mov     cl, bl
        shl     ah, cl
        test    [p_esr], ah
        jz      .e1n
        mov     al, [pv + bx]
        test    [p_sr], ah
        jnz     .e1k
        not     al
.e1k:   and     dl, al
.e1n:   inc     bx
        cmp     bx, 4
        jb      .e1
        cmp     dl, dh
        je      .r1ok
        inc     word [errs2]
.r1ok:  mov     ax, 0005h
        call    gc
        mov     ax, 0F07h
        call    gc
        dec     bp
        jnz     .it
        push    cs
        pop     es
        mov     si, s_alu
        mov     bx, errs
        call    report
        mov     si, s_rm1
        mov     bx, errs2
        call    report
        ret

; CASE si PASS / FAIL [bx] mismatches
report:
        call    lcase
        cmp     word [bx], 0
        jne     .f
        call    lpass
        jmp     .e
.f:     call    lfail
        movzx   eax, word [bx]
        call    ldec
        mov     si, s_mism
        call    lputs
.e:     call    lend
        ret

alu_error:
        pushad
        inc     word [errs]
        cmp     word [errs], 10
        ja      .x
        mov     si, s_aluerr
        call    linfo
        call    lputs
        movzx   eax, byte [p_wm]
        mov     cl, 1
        call    lhex
        mov     si, s_fn
        call    lputs
        movzx   eax, byte [p_fn]
        call    lhex
        mov     si, s_rot
        call    lputs
        movzx   eax, byte [p_rot]
        call    lhex
        mov     si, s_srv
        call    lputs
        movzx   eax, byte [p_sr]
        call    lhex
        mov     al, '/'
        call    lputc
        movzx   eax, byte [p_esr]
        call    lhex
        mov     si, s_bmv
        call    lputs
        movzx   eax, byte [p_bm]
        mov     cl, 2
        call    lhex
        mov     si, s_mmv
        call    lputs
        movzx   eax, byte [p_mm]
        mov     cl, 1
        call    lhex
        mov     si, s_cpuv
        call    lputs
        movzx   eax, byte [p_cpu]
        mov     cl, 2
        call    lhex
        mov     si, s_latch
        call    lputs
        mov     eax, [pv]
        mov     cl, 8
        call    lhex
        mov     si, s_gotv
        call    lputs
        mov     eax, [got]
        call    lhex
        call    lend
.x:     popad
        ret

; ---- TEXTMEM + font -----------------------------------------------------------
t_text:
        mov     al, 3
        call    setmode
        push    word 0B800h
        pop     es
        mov     word [errs], 0
        xor     di, di
        mov     cx, 4000h
        mov     ax, 1357h
.w:     stosw
        add     ax, 0F3A1h
        loop    .w
        xor     si, si
        mov     cx, 4000h
        mov     ax, 1357h
.r:     cmp     [es:si], ax
        je      .ok
        inc     word [errs]
.ok:    add     si, 2
        add     ax, 0F3A1h
        loop    .r
        ; cursor position through the BIOS, read back from CR0E/CR0F
        mov     ah, 2
        xor     bh, bh
        mov     dx, 0C27h               ; row 12, column 39
        int     10h
        mov     dx, 3D4h
        mov     al, 0Eh
        call    rdidx
        mov     ah, al
        mov     al, 0Fh
        call    rdidx
        cmp     ax, 12 * 80 + 39
        je      .cok
        inc     word [errs]
.cok:   push    cs
        pop     es
        mov     si, s_text
        mov     bx, errs
        call    report
        ; font CRC: plane 2, 256 chars x 32 bytes
        mov     ax, 0402h
        call    sq
        mov     ax, 0704h
        call    sq
        mov     ax, 0204h
        call    gc
        mov     ax, 0005h
        call    gc
        mov     ax, 0406h
        call    gc
        push    word 0A000h
        pop     es
        mov     edx, 0FFFFFFFFh
        xor     si, si
        mov     cx, 8192
        call    crc_update
        not     edx
        push    cs
        pop     es
        push    edx
        mov     al, 3
        call    setmode
        mov     si, s_font
        call    linfo
        call    lputs
        pop     eax
        mov     cl, 8
        call    lhex
        call    lend
        ret

; ---- RETRACE ------------------------------------------------------------------
rt_modes db     3, 70, 12h, 60, 13h, 70, 0
t_retrace:
        mov     bx, rt_modes
.m:     mov     al, [bx]
        or      al, al
        jz      .x
        push    bx
        call    setmode
        call    ticks                   ; sync to a tick edge
        mov     ebx, eax
.t0:    call    ticks
        cmp     eax, ebx
        je      .t0
        mov     ebx, eax
        add     ebx, 18
        xor     ecx, ecx                ; rising edges of vertical retrace
        mov     dx, 3DAh
        in      al, dx
        mov     ah, al
.p:     in      al, dx
        mov     bp, ax
        not     ah
        and     ah, al
        test    ah, 8
        jz      .ne
        inc     ecx
.ne:    mov     ax, bp
        mov     ah, al
        push    eax
        call    ticks
        cmp     eax, ebx
        pop     eax
        jb      .p
        ; Hz x10 = edges * 182065 / 18000
        mov     eax, ecx
        imul    eax, eax, 10115
        xor     edx, edx
        mov     ecx, 1000
        div     ecx
        mov     [rt_hz], eax
        pop     bx
        push    bx
        mov     si, s_rt
        call    lcase
        movzx   eax, byte [bx]
        mov     cl, 2
        call    lhex
        ; pass when within 4 Hz of the expected rate
        movzx   eax, byte [bx + 1]
        imul    eax, eax, 10
        sub     eax, [rt_hz]
        jns     .abs
        neg     eax
.abs:   cmp     eax, 40
        ja      .bad
        call    lpass
        jmp     .rep
.bad:   call    lfail
.rep:   mov     eax, [rt_hz]
        xor     edx, edx
        mov     ecx, 10
        div     ecx
        call    ldec
        mov     al, '.'
        call    lputc
        mov     eax, edx
        call    ldec
        mov     si, s_hz
        call    lputs
        movzx   eax, byte [bx + 1]
        call    ldec
        call    lend
        pop     bx
        add     bx, 2
        jmp     .m
.x:     ret
rt_hz   dd      0

; ---- per-mode register dump ------------------------------------------------------
md_modes db     0, 1, 2, 3, 4, 5, 6, 0Dh, 0Eh, 10h, 11h, 12h, 13h, 0FFh
t_modedump:
        mov     bx, md_modes
.m:     cmp     byte [bx], 0FFh
        je      .x
        push    bx
        mov     al, [bx]
        call    setmode
        mov     si, s_regsm
        call    linfo
        call    lputs
        pop     bx
        push    bx
        movzx   eax, byte [bx]
        mov     cl, 2
        call    lhex
        mov     al, ' '
        call    lputc
        mov     dx, 3CCh
        in      al, dx
        call    hex2
        mov     al, ' '
        call    lputc
        mov     dx, 3C4h
        xor     cx, cx
.s:     mov     al, cl
        call    rdidx
        call    hex2
        inc     cx
        cmp     cx, 5
        jb      .s
        mov     al, ' '
        call    lputc
        mov     dx, 3D4h
        xor     cx, cx
.c:     mov     al, cl
        call    rdidx
        call    hex2
        inc     cx
        cmp     cx, 25
        jb      .c
        mov     al, ' '
        call    lputc
        mov     dx, 3CEh
        xor     cx, cx
.g:     mov     al, cl
        call    rdidx
        call    hex2
        inc     cx
        cmp     cx, 9
        jb      .g
        mov     al, ' '
        call    lputc
        xor     cx, cx
.a:     mov     al, cl
        call    rdattr
        call    hex2
        inc     cx
        cmp     cx, 21
        jb      .a
        call    attr_on
        call    lend
        pop     bx
        inc     bx
        jmp     .m
.x:     ret

hex2:
        push    cx
        movzx   eax, al
        mov     cl, 2
        call    lhex
        pop     cx
        ret

; ---- VISUAL ---------------------------------------------------------------------
t_visual:
        ; 1: mode 13h, 256 colours in a 16x16 grid with a custom DAC ramp
        mov     al, 13h
        call    setmode
        mov     dx, 3C8h
        xor     al, al
        out     dx, al
        inc     dx
        xor     bx, bx
.pal:   mov     al, bl
        and     al, 0Fh
        shl     al, 2
        out     dx, al
        mov     al, bl
        shr     al, 2
        and     al, 3Ch
        out     dx, al
        mov     al, bl
        xor     al, 0FFh
        shr     al, 2
        out     dx, al
        inc     bl
        jnz     .pal
        push    word 0A000h
        pop     es
        xor     di, di
        xor     dx, dx                  ; y
.y:     xor     cx, cx                  ; x
.x:     mov     ax, dx
        shl     ax, 1                   ; row = y * 16 / 200 = 2y / 25
        mov     bl, 25
        div     bl
        shl     al, 4
        mov     bl, al
        mov     ax, cx
        shr     ax, 2
        mov     bh, 5
        div     bh
        add     al, bl
        stosb
        inc     cx
        cmp     cx, 320
        jb      .x
        inc     dx
        cmp     dx, 200
        jb      .y
        push    cs
        pop     es
        mov     si, s_v1
        call    visual
        ; 2: mode 12h stripes, split screen at line 300, pel panning
        mov     al, 12h
        call    setmode
        push    word 0A000h
        pop     es
        mov     ax, 0F02h
        call    sq
        xor     di, di
        mov     cx, 480
.row:   push    cx
        mov     ax, cx              ; 30-line colour bands
        mov     dl, 30
        div     dl
        and     al, 0Fh
        mov     ah, al
        xor     al, al
        mov     dx, 3CEh
        mov     al, 0
        out     dx, ax                  ; set/reset = row colour
        mov     ax, 0F01h
        out     dx, ax                  ; enable set/reset
        mov     cx, 80
        mov     al, 0FFh
        rep     stosb
        pop     cx
        loop    .row
        mov     ax, 0001h
        call    gc
        push    cs
        pop     es
        ; line compare = 300: CR18 low 8, CR07 bit4 = bit8, CR09 bit6 = bit9
        mov     dx, 3D4h
        mov     ax, 2C18h               ; 300 = 12Ch
        out     dx, ax
        mov     al, 7
        call    rdidx
        or      al, 10h
        mov     ah, al
        mov     al, 7
        out     dx, ax
        mov     al, 9
        call    rdidx
        and     al, 0BFh
        mov     ah, al
        mov     al, 9
        out     dx, ax
        ; start address 80*40 (top part scrolled 40 lines), pel pan 3
        mov     ax, 0C0Ch               ; 3200 = 0C80h
        out     dx, ax
        mov     ax, 800Dh
        out     dx, ax
        mov     ax, 0313h
        call    wrattr
        call    attr_on
        mov     si, s_v2
        call    visual
        ; 3: text attributes, all 256 characters, blink on
        mov     al, 3
        call    setmode
        push    word 0B800h
        pop     es
        xor     di, di
        xor     bx, bx
.ch:    mov     ax, bx
        mov     ah, bl
        xor     ah, 5Ah
        stosw
        mov     ax, 0720h
        stosw
        inc     bx
        test    bl, 0Fh
        jnz     .nl
        add     di, 160 - 64
.nl:    cmp     bx, 256
        jb      .ch
        push    cs
        pop     es
        mov     ah, 1                   ; block cursor
        mov     cx, 000Fh
        int     10h
        mov     si, s_v3
        call    visual
        ret

; log a VISUAL case (si = name + text), then wait for a key
visual:
        call    lcase
        call    lend
        call    getkey
        ret

; ---- strings --------------------------------------------------------------
logfile  db     'C:\CTRES\VGACHK.LOG', 0
s_banner db     'VGACHK CARDTEST 1.0', 0
s_where  db     'Results in C:\CTRES\VGACHK.LOG', 13, 10, '$'
s_regs   db     'REGS', 0
s_dac    db     'DAC', 0
s_alu    db     'ALU', 0
s_rm1    db     'RDMODE1', 0
s_text   db     'TEXTMEM', 0
s_rt     db     'RETRACE', 0
s_mism   db     ' mismatches', 0
s_regerr db     'reg ', 0
s_wrote  db     ' wrote ', 0
s_badbits db    ' bad bits ', 0
s_aluerr db     'alu wm=', 0
s_fn     db     ' fn=', 0
s_rot    db     ' rot=', 0
s_srv    db     ' sr/esr=', 0
s_bmv    db     ' bm=', 0
s_mmv    db     ' mm=', 0
s_cpuv   db     ' cpu=', 0
s_latch  db     ' planes=', 0
s_gotv   db     ' got=', 0
s_font   db     'font8x16 crc ', 0
s_hz     db     ' Hz, expected ', 0
s_regsm  db     'regs mode ', 0
s_v1     db     'V1PAL13 VISUAL 16x16 grid of 256 colours: red ramps left-right, green top-bottom, blue the inverse', 0
s_v2     db     'V2SPLIT VISUAL 30-line colour bands: above line 300 scrolled up 40 lines and 3 px left; below it the bands restart from the top, unscrolled', 0
s_v3     db     'V3TEXT VISUAL 256 characters 16 per row, varied attributes, blinking cells, block cursor', 0

%include "lib.inc"
