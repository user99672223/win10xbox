// probe_B_ntdll_stubs.cpp - category B (second half): raw Nt* stub bytes.
//
// Every Nt* export in ntdll is a short wrapper around a `syscall` instruction.
// To hook ntdll (which the compat layer must do for a handful of calls) we need
// to know the exact byte pattern we are about to replace. This module dumps the
// first 16 bytes of ~20 well-known Nt* syscall stubs and classifies them.
//
// Expected x64 pattern on Windows 10/11:
//     4C 8B D1              mov r10, rcx
//     B8 ?? ?? 00 00        mov eax, imm32         ; syscall number
//     F6 04 25 08 03 FE 7F 01   test byte ptr [0x7FFE0308], 1   (Wow64 check)
//     75 03                 jne short +3
//     0F 05                 syscall
//     C3                    ret
//     CD 2E                 int 2eh                (legacy fallback)
//     C3                    ret
//
// Variants exist (hotpatchable prelude, Wow64 jump, CET endbranch); we report
// the raw bytes verbatim plus a short classification so a reader can see at a
// glance which stubs look standard and which look patched.
#include "probe.h"
#include <cstdio>
#include <cstring>

namespace probe {

namespace {

std::string hexdump(const unsigned char* p, size_t n) {
    std::string s;
    s.reserve(n * 3);
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02X", p[i]);
        if (i) s += ' ';
        s += b;
    }
    return s;
}

// Returns a short classification of the stub shape and the syscall number if
// we can read one.
std::string classify_stub(const unsigned char* p) {
    char b[160];
    // Standard: 4C 8B D1 B8 nn nn 00 00 ...
    if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8) {
        uint32_t sysno = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                         ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        // Look for syscall (0F 05) or int 2E (CD 2E) later in the stub.
        bool has_syscall = false, has_int2e = false;
        for (size_t i = 8; i + 1 < 16; ++i) {
            if (p[i] == 0x0F && p[i + 1] == 0x05) has_syscall = true;
            if (p[i] == 0xCD && p[i + 1] == 0x2E) has_int2e = true;
        }
        std::snprintf(b, sizeof b, "shape=standard;sysno=0x%X;syscall=%d;int2e=%d",
                      sysno, has_syscall ? 1 : 0, has_int2e ? 1 : 0);
        return b;
    }
    // Hotpatchable prelude: 2 nop/int3 bytes at the start, then standard.
    if ((p[0] == 0x66 && p[1] == 0x90) || (p[0] == 0x90 && p[1] == 0x90)) {
        return "shape=hotpatch-prelude";
    }
    if (p[0] == 0xE9) {
        // Someone has already redirected this stub (relative jump).
        return "shape=redirected(JMP)";
    }
    if (p[0] == 0xFF && p[1] == 0x25) {
        return "shape=redirected(indirect-JMP)";
    }
    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) {
        return "shape=cet-endbranch64";
    }
    std::snprintf(b, sizeof b, "shape=unknown;first_byte=0x%02X", p[0]);
    return b;
}

const char* kStubs[] = {
    "NtClose",
    "NtCreateFile",
    "NtOpenFile",
    "NtReadFile",
    "NtWriteFile",
    "NtCreateSection",
    "NtMapViewOfSection",
    "NtUnmapViewOfSection",
    "NtAllocateVirtualMemory",
    "NtFreeVirtualMemory",
    "NtProtectVirtualMemory",
    "NtQueryVirtualMemory",
    "NtCreateUserProcess",
    "NtCreateThreadEx",
    "NtQuerySystemInformation",
    "NtQueryInformationProcess",
    "NtOpenProcessToken",
    "NtQueryInformationToken",
    "NtOpenKey",
    "NtQueryValueKey",
};

}  // namespace

void register_B_ntdll_stubs(Reporter& r) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) nt = LoadLibraryW(L"ntdll.dll");

    for (const char* name : kStubs) {
        r.run("B", std::string("stub:") + name, [nt, name](Record& rec) {
            if (!nt) {
                rec.status = Status::MISSING;
                rec.detail = "ntdll.dll not loadable";
                return;
            }
            FARPROC fp = GetProcAddress(nt, name);
            if (!fp) {
                rec.status = Status::MISSING;
                rec.detail = "export not found in ntdll";
                return;
            }
            const unsigned char* p = reinterpret_cast<const unsigned char*>(fp);
            unsigned char buf[16] = {0};
            memcpy(buf, p, sizeof buf);
            rec.status = Status::OK;
            rec.value = "bytes=" + hexdump(buf, sizeof buf) + ";" + classify_stub(buf);
            rec.detail = "first 16 bytes of ntdll stub recorded";
        });
    }

    // Also inspect the base of the Wow64/USR share page nttly stubs rely on,
    // so we know whether the TEB-based syscall-dispatcher knob is at its usual
    // place. Not strictly necessary for the loader on x64 (we never hit the
    // legacy fallback), but easy to record.
    r.run("B", "wow64-sharedinfo-ptr", [](Record& rec) {
        // KUSER_SHARED_DATA at 0x7FFE0000. Reading its SystemCall field (offset
        // 0x308 on x64) tells us whether the kernel suggests int2e. Guard the
        // read; the harness VEH wrapper will catch an AV if the page is gone.
        volatile uint8_t sys_call = *(volatile uint8_t*)(0x7FFE0000 + 0x308);
        char b[64];
        std::snprintf(b, sizeof b, "SystemCallMode=0x%02X", sys_call);
        rec.status = Status::OK;
        rec.value = b;
        rec.detail = "KUSER_SHARED_DATA.SystemCall - 0=syscall, 1=int2e";
    });
}

}  // namespace probe
