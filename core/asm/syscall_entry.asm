; win10xbox syscall entry (MASM / MSVC). GAS twin: syscall_entry.S.
; Keep both in sync; the contract and frame layout are documented in
; syscall_entry.S and docs/DISPATCHER.md.

EXTERN win10xbox_syscall_nargs:PROC
EXTERN win10xbox_dispatch:PROC
EXTERN win10xbox_thread_slot_offset:QWORD

.code

win10xbox_syscall_entry PROC FRAME
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    sub rsp, 0F0h
    .allocstack 0F0h
    .endprolog
    cld
    mov dword ptr [rsp+20h], eax
    mov qword ptr [rsp+50h], r10
    mov qword ptr [rsp+58h], rdx
    mov qword ptr [rsp+60h], r8
    mov qword ptr [rsp+68h], r9

    ; switch the TEB to the host view
    xor ebx, ebx
    mov r11, qword ptr [win10xbox_thread_slot_offset]
    test r11, r11
    jz entry_view_done
    mov rbx, qword ptr gs:[r11]
    test rbx, rbx
    jz entry_view_done
    inc dword ptr [rbx+20h]
    cmp dword ptr [rbx+20h], 1
    jne entry_view_done
    mov rax, qword ptr gs:[58h]
    mov qword ptr [rbx+10h], rax
    mov rax, qword ptr [rbx]
    mov qword ptr gs:[58h], rax
    mov rax, qword ptr gs:[17C8h]
    mov qword ptr [rbx+18h], rax
    mov rax, qword ptr [rbx+8]
    mov qword ptr gs:[17C8h], rax
entry_view_done:
    mov ecx, dword ptr [rsp+20h]
    call win10xbox_syscall_nargs
    mov dword ptr [rsp+24h], eax
    lea rcx, [rsp+50h]
    mov qword ptr [rsp+28h], rcx
    mov rcx, qword ptr [rsp+108h]
    mov qword ptr [rsp+30h], rcx
    lea rcx, [rsp+108h]
    mov qword ptr [rsp+38h], rcx
    ; copy stack args 4..nargs-1
    mov ecx, eax
    sub ecx, 4
    jle args_done
    lea rsi, [rsp+130h]
    lea rdi, [rsp+70h]
    rep movsq
args_done:
    lea rcx, [rsp+20h]
    call win10xbox_dispatch

    ; back to the guest view
    test rbx, rbx
    jz exit_view_done
    dec dword ptr [rbx+20h]
    jnz exit_view_done
    mov rsi, rax
    mov rax, qword ptr gs:[58h]
    mov qword ptr [rbx], rax
    mov rax, qword ptr [rbx+10h]
    mov qword ptr gs:[58h], rax
    mov rax, qword ptr gs:[17C8h]
    mov qword ptr [rbx+8], rax
    mov rax, qword ptr [rbx+18h]
    mov qword ptr gs:[17C8h], rax
    mov rax, rsi
exit_view_done:
    add rsp, 0F0h
    pop rdi
    pop rsi
    pop rbx
    ret
win10xbox_syscall_entry ENDP

END
