/*
 * win10xbox syscall entry (GAS / clang-mingw). MASM twin: syscall_entry.asm.
 * Keep both in sync; the layout is documented in docs/DISPATCHER.md.
 *
 * Reached by `jmp` from a rewritten guest thunk (`mov r10,rcx; mov eax,id;
 * jmp entry`). On entry:
 *   eax = syscall id, r10 = arg0, rdx/r8/r9 = arg1..3,
 *   [rsp] = return address into the guest caller (the thunk was `call`ed),
 *   [rsp+0x28+8*(i-4)] = stack arg i (i >= 4), above the caller's shadow space.
 *
 * Frame (rsp after the prologue):
 *   +0x00  shadow space for our calls
 *   +0x20  dispatch::Frame { u32 id; u32 nargs; u64* args; u64 return_address; u64 guest_rsp; }
 *   +0x40  (pad)
 *   +0x50  args[20]
 *   +0xf0  saved rdi, rsi, rbx; +0x108 return address (= guest rsp at entry)
 *
 * Thread view switch: when the thread has a ThreadState (TLS slot whose TEB
 * offset is in win10xbox_thread_slot_offset), the TEB's
 * ThreadLocalStoragePointer (gs:0x58) and FlsData (gs:0x17c8) are swapped to
 * the host's values for the duration of the call (nesting counted in
 * ThreadState.depth). ThreadState layout: host_tls 0x00, host_fls 0x08,
 * guest_tls 0x10, guest_fls 0x18, depth 0x20 (u32).
 */
    .text
    .globl win10xbox_syscall_entry
    .def win10xbox_syscall_entry; .scl 2; .type 32; .endef
    .p2align 4
win10xbox_syscall_entry:
    .seh_proc win10xbox_syscall_entry
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    subq $0xf0, %rsp
    .seh_stackalloc 0xf0
    .seh_endprologue
    cld
    movl %eax, 0x20(%rsp)
    movq %r10, 0x50(%rsp)
    movq %rdx, 0x58(%rsp)
    movq %r8, 0x60(%rsp)
    movq %r9, 0x68(%rsp)

    /* switch the TEB to the host view */
    xorl %ebx, %ebx
    movq win10xbox_thread_slot_offset(%rip), %r11
    testq %r11, %r11
    jz 1f
    movq %gs:(%r11), %rbx
    testq %rbx, %rbx
    jz 1f
    incl 0x20(%rbx)
    cmpl $1, 0x20(%rbx)
    jne 1f
    movq %gs:0x58, %rax
    movq %rax, 0x10(%rbx)
    movq (%rbx), %rax
    movq %rax, %gs:0x58
    movq %gs:0x17c8, %rax
    movq %rax, 0x18(%rbx)
    movq 0x8(%rbx), %rax
    movq %rax, %gs:0x17c8
1:
    movl 0x20(%rsp), %ecx
    call win10xbox_syscall_nargs
    movl %eax, 0x24(%rsp)
    leaq 0x50(%rsp), %rcx
    movq %rcx, 0x28(%rsp)
    movq 0x108(%rsp), %rcx
    movq %rcx, 0x30(%rsp)
    leaq 0x108(%rsp), %rcx
    movq %rcx, 0x38(%rsp)
    /* copy stack args 4..nargs-1 */
    movl %eax, %ecx
    subl $4, %ecx
    jle 2f
    leaq 0x130(%rsp), %rsi
    leaq 0x70(%rsp), %rdi
    rep movsq
2:
    leaq 0x20(%rsp), %rcx
    call win10xbox_dispatch

    /* back to the guest view */
    testq %rbx, %rbx
    jz 3f
    decl 0x20(%rbx)
    jnz 3f
    movq %rax, %rsi
    movq %gs:0x58, %rax
    movq %rax, (%rbx)
    movq 0x10(%rbx), %rax
    movq %rax, %gs:0x58
    movq %gs:0x17c8, %rax
    movq %rax, 0x8(%rbx)
    movq 0x18(%rbx), %rax
    movq %rax, %gs:0x17c8
    movq %rsi, %rax
3:
    addq $0xf0, %rsp
    popq %rdi
    popq %rsi
    popq %rbx
    ret
    .seh_endproc
