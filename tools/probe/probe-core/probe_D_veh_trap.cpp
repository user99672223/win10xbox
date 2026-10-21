// probe_D_veh_trap.cpp - category D: exception handling and trap-and-emulate.
//
// The driver-hosting story stands or falls on this module. For a ReactOS-derived
// fake ntoskrnl to run in user mode, we need:
//   1) AddVectoredExceptionHandler to install a VEH;
//   2) a deliberate access violation to be caught, with the context inspectable
//      and the thread allowed to resume;
//   3) a privileged instruction (we use RDMSR) to #GP in user mode and be
//      caught the same way, with the ability to skip past it (adjust RIP) and
//      continue - the mechanism trap-and-emulate rides on;
//   4) Get/SetThreadContext available for other threads.
//
// Every test is designed so a FAILED expectation is captured as a probe status
// without crashing the harness: the deliberate faults are inside a per-probe
// VEH + a wrapper IsBadReadPtr-style guard that resumes execution past the
// faulting instruction. The reporter's SEH wrapper is still active as a last
// line of defense.
#include "probe.h"
#include <cstdio>
#include <atomic>

namespace probe {

namespace {

std::string hex64(uint64_t v) {
    char b[24]; std::snprintf(b, sizeof b, "0x%016llX", (unsigned long long)v); return b;
}
std::string hex32(uint32_t v) {
    char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b;
}

// Per-probe state shared with its VEH. We use a thread-local pointer so the VEH
// can locate the probe it is serving without a global - this keeps multiple
// probes independent even if the harness ever ran them in parallel.
struct VehState {
    std::atomic<int>  caught{0};
    uint32_t          exception_code = 0;
    uint64_t          exception_addr = 0;
    uint64_t          rip_at_fault = 0;
    // When set, the VEH will advance RIP by this many bytes and continue
    // execution instead of unwinding. Used by the AV test (write 1 byte to a
    // guard page -> skip the instruction) and the privileged-instruction test
    // (RDMSR is 2 bytes: 0x0F 0x32 -> skip 2).
    unsigned          skip_bytes = 0;
    // When set, the VEH redirects RIP to this label instead of advancing.
    void*             redirect_to = nullptr;
};

thread_local VehState* tls_veh = nullptr;

LONG CALLBACK veh_handler(EXCEPTION_POINTERS* ep) {
    VehState* s = tls_veh;
    if (!s) return EXCEPTION_CONTINUE_SEARCH;
    s->caught.fetch_add(1);
    s->exception_code = ep->ExceptionRecord->ExceptionCode;
    s->exception_addr = (uint64_t)ep->ExceptionRecord->ExceptionAddress;
#if defined(_M_X64)
    s->rip_at_fault = (uint64_t)ep->ContextRecord->Rip;
    if (s->redirect_to) {
        ep->ContextRecord->Rip = (DWORD64)s->redirect_to;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (s->skip_bytes) {
        ep->ContextRecord->Rip += s->skip_bytes;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

// Executes an inline x64 instruction we expect to fault. We use a 2-byte RDMSR
// (0F 32) built on the stack so the probe is self-contained. If the kernel
// doesn't deliver the #GP for privileged instructions - vanishingly unlikely,
// but one scenario is a lightweight Xbox hypervisor virtualising it - we detect
// that too (caught == 0 means it ran as a no-op or was emulated by someone else).
__declspec(noinline) int try_rdmsr_through_stub(VehState* state) {
    tls_veh = state;
    // Build a tiny stub: xor eax,eax ; rdmsr ; ret   (returns 0 if the VEH
    // skipped the rdmsr, which is what we want). rdmsr is 2 bytes (0F 32).
    unsigned char code[] = {
        0x33, 0xC0,             // xor eax, eax
        0x33, 0xC9,             // xor ecx, ecx  (MSR index 0 - IA32_P5_MC_ADDR)
        0x0F, 0x32,             // rdmsr  <-- expected to #GP
        0xC3                    // ret
    };
    void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p) return -1;
    memcpy(p, code, sizeof code);
    DWORD old = 0;
    if (!VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return -2;
    }
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    using Fn = int (*)();
    int rv = reinterpret_cast<Fn>(p)();
    VirtualFree(p, 0, MEM_RELEASE);
    tls_veh = nullptr;
    return rv;
}

// A guard-page AV demo: install guard, read it, the VEH skips the fault.
__declspec(noinline) int try_touch_guard(VehState* state) {
    tls_veh = state;
    volatile char* page = (volatile char*)VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (!page) { tls_veh = nullptr; return -1; }
    // mov al, [page] -> instruction length depends on how the compiler emits
    // this read; we use the generic skip_bytes=0 + redirect_to approach to be
    // instruction-length agnostic. Set redirect_to to a label after the read.
    volatile char observed = 0;
    __try {
        observed = *page;  // SEH primary; VEH fires first
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // If the VEH didn't continue, SEH mops it up. Still a positive signal.
    }
    (void)observed;
    VirtualFree((void*)page, 0, MEM_RELEASE);
    tls_veh = nullptr;
    return 0;
}

}  // namespace

void register_D_veh_trap(Reporter& r) {
    // D1: install + remove a VEH. Measures availability of
    // AddVectoredExceptionHandler itself.
    r.run("D", "veh-add-remove", [](Record& rec) {
        PVOID h = AddVectoredExceptionHandler(1, veh_handler);
        if (!h) {
            DWORD gle = GetLastError();
            rec.status = Status::DENIED;
            rec.errorcode = gle;
            rec.detail = "AddVectoredExceptionHandler denied: " + error_text(gle);
            return;
        }
        ULONG rc = RemoveVectoredExceptionHandler(h);
        rec.status = Status::OK;
        rec.value = "add=1;remove=" + std::to_string(rc);
        rec.detail = "VEH install/uninstall works";
    });

    // D2: deliberate access violation, caught by VEH, inspected, resumed via
    // __except (not VEH redirect - keeps the test instruction-length agnostic).
    r.run("D", "veh-access-violation", [](Record& rec) {
        VehState s{};
        PVOID h = AddVectoredExceptionHandler(1, veh_handler);
        if (!h) {
            rec.status = Status::DENIED;
            rec.detail = "cannot install VEH for AV test";
            return;
        }
        try_touch_guard(&s);
        RemoveVectoredExceptionHandler(h);
        if (s.caught.load() > 0 &&
            s.exception_code == (uint32_t)EXCEPTION_ACCESS_VIOLATION) {
            rec.status = Status::OK;
            rec.value = "caught=" + std::to_string(s.caught.load()) +
                        ";code=" + hex32(s.exception_code) +
                        ";addr=" + hex64(s.exception_addr);
            rec.detail = "AV delivered to VEH and we can inspect ExceptionRecord/Context";
        } else {
            rec.status = Status::WARN;
            rec.detail = "VEH did not observe the AV before SEH ate it (unusual)";
        }
    });

    // D3: the CRUCIAL one - a privileged instruction (RDMSR) must #GP in user
    // mode, our VEH must catch it, we must be able to adjust RIP past it and
    // continue. If this returns OK, trap-and-emulate for the driver host is on
    // the table.
    r.run("D", "veh-privileged-insn", [](Record& rec) {
        VehState s{};
        s.skip_bytes = 2;  // RDMSR = 0F 32
        PVOID h = AddVectoredExceptionHandler(1, veh_handler);
        if (!h) {
            rec.status = Status::DENIED;
            rec.detail = "cannot install VEH for privileged-insn test";
            return;
        }
        int rv = try_rdmsr_through_stub(&s);
        RemoveVectoredExceptionHandler(h);
        if (rv < 0) {
            rec.status = Status::WARN;
            rec.detail = "could not allocate/flip stub (rv=" + std::to_string(rv) + ")";
            return;
        }
        if (s.caught.load() == 0) {
            rec.status = Status::WARN;
            rec.value = "caught=0;result=" + hex32((uint32_t)rv);
            rec.detail = "RDMSR did NOT fault - a hypervisor is intercepting it, or "
                         "the environment emulates it transparently";
            return;
        }
        // Expected code: EXCEPTION_PRIV_INSTRUCTION (0xC0000096).
        bool is_priv = (s.exception_code == (uint32_t)EXCEPTION_PRIV_INSTRUCTION ||
                        s.exception_code == 0xC0000096u);
        if (is_priv) {
            rec.status = Status::OK;
            rec.value = "caught=1;code=0xC0000096;rip=" + hex64(s.rip_at_fault) +
                        ";resumed=1;result=" + hex32((uint32_t)rv);
            rec.detail = "RDMSR trapped to VEH as EXCEPTION_PRIV_INSTRUCTION, "
                         "we adjusted RIP past it, execution continued - "
                         "TRAP-AND-EMULATE IS FEASIBLE for driver hosting";
        } else {
            rec.status = Status::WARN;
            rec.value = "caught=1;code=" + hex32(s.exception_code);
            rec.detail = "exception caught but not EXCEPTION_PRIV_INSTRUCTION; "
                         "RIP-advance still succeeded";
        }
    });

    // D4: Get/SetThreadContext availability on a child thread we own.
    r.run("D", "thread-context", [](Record& rec) {
        HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!evt) {
            rec.status = Status::DENIED;
            rec.detail = "CreateEvent failed";
            return;
        }
        auto thread_body = [](LPVOID p) -> DWORD {
            WaitForSingleObject((HANDLE)p, 2000);
            return 0;
        };
        HANDLE th = CreateThread(nullptr, 0,
                                 (LPTHREAD_START_ROUTINE)+thread_body, evt, 0, nullptr);
        if (!th) {
            DWORD gle = GetLastError();
            rec.status = Status::DENIED;
            rec.errorcode = gle;
            rec.detail = "CreateThread denied: " + error_text(gle);
            CloseHandle(evt);
            return;
        }
        // Suspend, GetThreadContext, SetThreadContext (no-op modification), resume.
        if (SuspendThread(th) == (DWORD)-1) {
            DWORD gle = GetLastError();
            rec.status = Status::DENIED;
            rec.errorcode = gle;
            rec.detail = "SuspendThread denied: " + error_text(gle);
        } else {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            BOOL got = GetThreadContext(th, &ctx);
            BOOL set = got ? SetThreadContext(th, &ctx) : FALSE;
            ResumeThread(th);
            if (got && set) {
                rec.status = Status::OK;
                rec.value = "get=1;set=1;rip=" + hex64((uint64_t)ctx.Rip);
                rec.detail = "Get/SetThreadContext work on an owned thread";
            } else {
                DWORD gle = GetLastError();
                rec.status = Status::DENIED;
                rec.errorcode = gle;
                rec.detail = std::string("Get=") + (got ? "ok" : "denied") +
                             " Set=" + (set ? "ok" : "denied") + ": " +
                             error_text(gle);
            }
        }
        SetEvent(evt);
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
        CloseHandle(evt);
    });

    // D5: continuation handler path (AddVectoredContinueHandler).
    r.run("D", "veh-continue-handler", [](Record& rec) {
        PVOID h = AddVectoredContinueHandler(1, veh_handler);
        if (!h) {
            DWORD gle = GetLastError();
            rec.status = Status::DENIED;
            rec.errorcode = gle;
            rec.detail = "AddVectoredContinueHandler denied: " + error_text(gle);
            return;
        }
        RemoveVectoredContinueHandler(h);
        rec.status = Status::OK;
        rec.detail = "continue handler install/uninstall works";
    });

    // D6: UnhandledExceptionFilter chainable? We only confirm it is callable
    // and resolve the registered one; we DO NOT actually invoke it on a real
    // exception (that would terminate the host).
    r.run("D", "unhandled-filter-ptr", [](Record& rec) {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(prev);  // restore
        rec.status = Status::OK;
        rec.value = "ptr=" + hex64((uint64_t)prev);
        rec.detail = "SetUnhandledExceptionFilter accepts changes (slot writable)";
    });
}

}  // namespace probe
