// User-pointer probing: the vectored handler and the C++ side of the guarded accesses. See
// core/include/win10xbox/probe.h for the contract and core/asm/probe.{S,asm} for the routines it guards.
//
// Everything here is reachable from a guest syscall, from the host view of the TEB and, for the handler, from
// whichever view the faulting thread has: no function-local statics, no thread_local, no logging in the handler.
#include "win10xbox/probe.h"
#include <intrin.h>

extern "C" {
// core/asm/probe.{S,asm}. Defined in this order and adjacent, so [w10x_probe_copy, w10x_probe_exit) is exactly
// the code allowed to fault.
NTSTATUS w10x_probe_copy(void* to, const void* from, std::size_t length);
NTSTATUS w10x_probe_touch_read(const void* address);
NTSTATUS w10x_probe_touch_write(void* address);
NTSTATUS w10x_probe_exit();
}

namespace win10xbox::probe {
namespace {

constexpr DWORD kInPageError = 0xC0000006;       // EXCEPTION_IN_PAGE_ERROR
constexpr DWORD kGuardPageViolation = 0x80000001;   // EXCEPTION_GUARD_PAGE

// Namespace scope and constant-initialised, written once by Install before the handler is registered.
volatile std::uintptr_t g_begin = 0;
volatile std::uintptr_t g_end = 0;
volatile std::uintptr_t g_exit = 0;
volatile LONG g_state = 0;   // 0 = not installed, 1 = being installed, 2 = installed

// An incremental-link (MSVC Debug) build gives a function's address as a `jmp rel32` stub: follow it to the code.
std::uintptr_t Resolve(const void* fn) {
    auto p = static_cast<const std::uint8_t*>(fn);
    for (int i = 0; i < 4 && p[0] == 0xE9; ++i) p = p + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
    return reinterpret_cast<std::uintptr_t>(p);
}

LONG CALLBACK Handler(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    NTSTATUS status;
    switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: status = kAccessViolation; break;
    case kGuardPageViolation: status = static_cast<NTSTATUS>(kGuardPageViolation); break;
    case kInPageError:
        // ExceptionInformation[2] is the NTSTATUS of the failed page-in (STATUS_IN_PAGE_ERROR, a device error...).
        status = rec->NumberParameters >= 3 ? static_cast<NTSTATUS>(rec->ExceptionInformation[2])
                                             : static_cast<NTSTATUS>(kInPageError);
        break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    if (ctx->Rip < g_begin || ctx->Rip >= g_end) return EXCEPTION_CONTINUE_SEARCH;   // not one of our accesses
    ctx->Rax = static_cast<DWORD>(status);
    ctx->Rip = g_exit;
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline bool InUserRange(const void* address, std::size_t length) {
    const auto a = reinterpret_cast<std::uintptr_t>(address);
    return a < kUserProbeAddress && length <= kUserProbeAddress - a;
}

}  // namespace

void Install() {
    if (g_state == 2) return;
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) {
        while (g_state != 2) YieldProcessor();   // another thread is registering the handler
        return;
    }
    g_begin = Resolve(reinterpret_cast<const void*>(&w10x_probe_copy));
    g_end = Resolve(reinterpret_cast<const void*>(&w10x_probe_exit));
    g_exit = g_end;
    AddVectoredExceptionHandler(1, &Handler);   // first: ahead of the host's last-chance handler
    InterlockedExchange(&g_state, 2);
}

NTSTATUS ProbeRead(const void* address, std::size_t length, std::size_t align) {
    if (length == 0) return 0;
    Install();
    const auto a = reinterpret_cast<std::uintptr_t>(address);
    if (a & (align - 1)) return kDatatypeMisalignment;
    if (!InUserRange(address, length)) return kAccessViolation;
    // First byte, then the first byte of every following page, then the last byte.
    const std::uintptr_t last = a + length - 1;
    for (std::uintptr_t p = a;;) {
        if (NTSTATUS s = w10x_probe_touch_read(reinterpret_cast<const void*>(p))) return s;
        const std::uintptr_t next = (p | 0xFFF) + 1;
        if (next > last) break;
        p = next;
    }
    return w10x_probe_touch_read(reinterpret_cast<const void*>(last));
}

NTSTATUS ProbeWrite(void* address, std::size_t length, std::size_t align) {
    if (length == 0) return 0;
    Install();
    const auto a = reinterpret_cast<std::uintptr_t>(address);
    if (a & (align - 1)) return kDatatypeMisalignment;
    if (!InUserRange(address, length)) return kAccessViolation;
    const std::uintptr_t last = a + length - 1;
    for (std::uintptr_t p = a;;) {
        if (NTSTATUS s = w10x_probe_touch_write(reinterpret_cast<void*>(p))) return s;
        const std::uintptr_t next = (p | 0xFFF) + 1;
        if (next > last) break;
        p = next;
    }
    return w10x_probe_touch_write(reinterpret_cast<void*>(last));
}

NTSTATUS CopyFromUser(void* to, const void* user_from, std::size_t length) {
    if (length == 0) return 0;
    Install();
    if (!InUserRange(user_from, length)) return kAccessViolation;
    return w10x_probe_copy(to, user_from, length);
}

NTSTATUS CopyToUser(void* user_to, const void* from, std::size_t length) {
    if (length == 0) return 0;
    Install();
    if (!InUserRange(user_to, length)) return kAccessViolation;
    return w10x_probe_copy(user_to, from, length);
}

NTSTATUS CaptureString(const UNICODE_STRING* user, String* out) {
    UNICODE_STRING header;
    if (NTSTATUS s = Load<UNICODE_STRING>(user, &header)) return s;
    out->us = header;
    if (header.Length && (reinterpret_cast<std::uintptr_t>(header.Buffer) & (sizeof(wchar_t) - 1)))
        return kDatatypeMisalignment;   // ProbeForRead(Buffer, Length, sizeof(WCHAR))
    out->text.assign(header.Length / sizeof(wchar_t), L'\0');
    if (!out->text.empty())
        if (NTSTATUS s = CopyFromUser(out->text.data(), header.Buffer, out->text.size() * sizeof(wchar_t))) return s;
    out->us.Buffer = out->text.data();
    return 0;
}

NTSTATUS CaptureObjectAttributes(const OBJECT_ATTRIBUTES* user, ObjectAttributes* out) {
    ULONG length;
    if (NTSTATUS s = Load<ULONG>(&user->Length, &length)) return s;
    if (length != sizeof(OBJECT_ATTRIBUTES)) return static_cast<NTSTATUS>(0xC000000D);   // STATUS_INVALID_PARAMETER
    if (NTSTATUS s = Load<OBJECT_ATTRIBUTES>(user, &out->oa)) return s;
    const UNICODE_STRING* name = out->oa.ObjectName;
    out->oa.ObjectName = nullptr;
    if (!name) return 0;
    if (NTSTATUS s = CaptureString(name, &out->name)) return s;
    out->oa.ObjectName = &out->name.us;
    return 0;
}

}  // namespace win10xbox::probe
