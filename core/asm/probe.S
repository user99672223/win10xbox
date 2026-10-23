/*
 * User-pointer probing: the only code that touches a caller's memory on behalf of a syscall implementation.
 * GAS (clang-mingw) twin; MASM twin: probe.asm. Keep both in sync.
 *
 * Every routine here is a leaf: no stack use, volatile registers only (rax, rcx, rdx, r8, r9). That is what lets
 * the vectored handler in core/dispatch/probe.cpp recover from a fault by changing nothing but rip and rax: it
 * resumes at w10x_probe_exit, which returns to the C++ caller with the NTSTATUS the handler put in rax. The
 * routines return 0 when they complete. They are laid out back to back, copy first and exit last; the handler
 * treats [w10x_probe_copy, w10x_probe_exit) as the code that is allowed to fault.
 *
 *   NTSTATUS w10x_probe_copy(void* to /rcx/, const void* from /rdx/, size_t length /r8/)
 *   NTSTATUS w10x_probe_touch_read(const void* address /rcx/)    reads one byte
 *   NTSTATUS w10x_probe_touch_write(void* address /rcx/)         atomic `or byte, 0`: needs write access,
 *                                                                changes nothing, cannot lose a concurrent store
 */
    .text
    .globl w10x_probe_copy
    .def w10x_probe_copy; .scl 2; .type 32; .endef
    .p2align 4
w10x_probe_copy:
    .seh_proc w10x_probe_copy
    .seh_endprologue
    xorl %eax, %eax
.Lcopy8:
    cmpq $8, %r8
    jb .Lcopy1
    movq (%rdx), %r9
    movq %r9, (%rcx)
    addq $8, %rdx
    addq $8, %rcx
    subq $8, %r8
    jmp .Lcopy8
.Lcopy1:
    testq %r8, %r8
    jz .Lcopydone
    movb (%rdx), %r9b
    movb %r9b, (%rcx)
    incq %rdx
    incq %rcx
    decq %r8
    jmp .Lcopy1
.Lcopydone:
    ret
    .seh_endproc

    .globl w10x_probe_touch_read
    .def w10x_probe_touch_read; .scl 2; .type 32; .endef
    .p2align 4
w10x_probe_touch_read:
    .seh_proc w10x_probe_touch_read
    .seh_endprologue
    xorl %eax, %eax
    movb (%rcx), %r9b
    ret
    .seh_endproc

    .globl w10x_probe_touch_write
    .def w10x_probe_touch_write; .scl 2; .type 32; .endef
    .p2align 4
w10x_probe_touch_write:
    .seh_proc w10x_probe_touch_write
    .seh_endprologue
    xorl %eax, %eax
    lock orb $0, (%rcx)
    ret
    .seh_endproc

    .globl w10x_probe_exit
    .def w10x_probe_exit; .scl 2; .type 32; .endef
    .p2align 4
w10x_probe_exit:
    .seh_proc w10x_probe_exit
    .seh_endprologue
    ret
    .seh_endproc
