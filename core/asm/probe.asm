; User-pointer probing (MASM / MSVC). GAS twin: probe.S, which documents the contract.
; Keep both in sync.

.code

w10x_probe_copy PROC FRAME
    .endprolog
    xor eax, eax
copy8:
    cmp r8, 8
    jb copy1
    mov r9, qword ptr [rdx]
    mov qword ptr [rcx], r9
    add rdx, 8
    add rcx, 8
    sub r8, 8
    jmp copy8
copy1:
    test r8, r8
    jz copydone
    mov r9b, byte ptr [rdx]
    mov byte ptr [rcx], r9b
    inc rdx
    inc rcx
    dec r8
    jmp copy1
copydone:
    ret
w10x_probe_copy ENDP

w10x_probe_touch_read PROC FRAME
    .endprolog
    xor eax, eax
    mov r9b, byte ptr [rcx]
    ret
w10x_probe_touch_read ENDP

w10x_probe_touch_write PROC FRAME
    .endprolog
    xor eax, eax
    lock or byte ptr [rcx], 0
    ret
w10x_probe_touch_write ENDP

w10x_probe_exit PROC FRAME
    .endprolog
    ret
w10x_probe_exit ENDP

END
