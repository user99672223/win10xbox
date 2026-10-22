// Handle routing, memory/process/thread queries, termination, context
// switches and hard errors.
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif
#if WIN10XBOX_HAVE_TOKEN
#include "win10xbox/token.h"
#endif
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#include <algorithm>
#include <cstring>
#include <vector>

namespace win10xbox::dispatch::detail {
namespace {

// MEMORY_INFORMATION_CLASS values used by Wine's PE side (include/winternl.h).
constexpr ULONG kMemoryBasicInformation = 0;
constexpr ULONG kMemoryMappedFilenameInformation = 2;
constexpr ULONG kMemoryWineLoadUnixLib = 1000;
constexpr ULONG kMemoryWineLoadUnixLibWow64 = 1001;
constexpr ULONG kMemoryWineLoadUnixLibByName = 1002;
constexpr ULONG kMemoryWineLoadUnixLibByNameWow64 = 1003;
constexpr ULONG kMemoryWineUnloadUnixLib = 1004;
// PROCESSINFOCLASS
constexpr ULONG kProcessWow64Information = 26;
constexpr ULONG kProcessImageFileName = 27;
constexpr ULONG kProcessImageInformation = 37;
constexpr ULONG kProcessImageFileNameWin32 = 43;
// THREADINFOCLASS
constexpr ULONG kThreadAmILastThread = 12;
// SYSTEM_INFORMATION_CLASS
#if WIN10XBOX_HAVE_CLOCK
constexpr ULONG kSystemTimeOfDayInformation = 3;
#endif
constexpr ULONG kSystemWineVersionInformation = 1000;

NTSTATUS WriteUnicodeStringInfo(const std::wstring& s, PVOID buffer, ULONG length, PULONG ret_len) {
    const ULONG bytes = static_cast<ULONG>(s.size() * sizeof(wchar_t));
    const ULONG need = static_cast<ULONG>(sizeof(UNICODE_STRING)) + bytes + sizeof(wchar_t);
    if (ret_len) *ret_len = need;
    if (length < need) return st::InfoLengthMismatch;
    auto* us = static_cast<UNICODE_STRING*>(buffer);
    auto* text = reinterpret_cast<wchar_t*>(us + 1);
    std::memcpy(text, s.c_str(), bytes + sizeof(wchar_t));
    us->Buffer = text;
    us->Length = static_cast<USHORT>(bytes);
    us->MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
    return st::Success;
}

std::wstring StripModuleName(std::wstring_view name) {
    std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
    return std::wstring(name);
}

}  // namespace

std::string ModuleExportName(const void* image_base) {
    auto* base = static_cast<const std::uint8_t*>(image_base);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return {};
    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
    return exp->Name ? std::string(reinterpret_cast<const char*>(base + exp->Name)) : std::string();
}

// ---- Handles ----------------------------------------------------------------------

NTSTATUS NTAPI Xl_NtClose(HANDLE h) {
#if WIN10XBOX_HAVE_REGISTRY
    if (registry::IsKeyHandle(h)) return registry::Close(h);
#endif
#if WIN10XBOX_HAVE_TOKEN
    if (token::IsTokenHandle(h)) return token::Close(h);
#endif
    EmulatedObject o;
    if (LookupEmulatedObject(h, &o)) {
        if (o.kind == ObjectKind::ImageSection) CloseImageSection(o);
        UnregisterEmulatedObject(h);
        return st::Success;
    }
    return Host().NtClose(h);
}

static NTSTATUS NTAPI Xl_NtDuplicateObject(HANDLE src_process, HANDLE src, HANDLE dst_process, PHANDLE dst,
                                           ACCESS_MASK access, ULONG attributes, ULONG options) {
    EmulatedObject o;
    bool emulated = IsCurrentProcess(src_process) && LookupEmulatedObject(src, &o);
#if WIN10XBOX_HAVE_REGISTRY
    emulated = emulated || (IsCurrentProcess(src_process) && registry::IsKeyHandle(src));
#endif
#if WIN10XBOX_HAVE_TOKEN
    emulated = emulated || (IsCurrentProcess(src_process) && token::IsTokenHandle(src));
#endif
    if (!emulated)
        return Host().NtDuplicateObject(src_process, src, dst_process, dst, access, attributes, options);
    if (!IsCurrentProcess(dst_process) || o.kind != ObjectKind::ImageSection || !o.extra) {
        FIXME(dispatch, "duplicating emulated handle %p (kind %d)", src, static_cast<int>(o.kind));
        return st::NotImplemented;
    }
    HANDLE h = DuplicateImageSection(o);
    if (!h) return st::NoMemory;
    if (dst) *dst = h;
    if (options & DUPLICATE_CLOSE_SOURCE) Xl_NtClose(src);
    return st::Success;
}

// ---- Memory ---------------------------------------------------------------------------

static NTSTATUS NTAPI Xl_NtQueryVirtualMemory(HANDLE process, PVOID addr, ULONG klass, PVOID buffer, SIZE_T length,
                                              PSIZE_T ret_len) {
    switch (klass) {
    case kMemoryWineLoadUnixLib: {
        std::string name = ModuleExportName(addr);
        std::wstring wname(name.begin(), name.end());
        std::uint64_t h = wname.empty() ? 0 : UnixLibHandleByName(wname);
        TRACE(dispatch, "MemoryWineLoadUnixLib %p (%s) -> %llx", addr, name.c_str(), static_cast<unsigned long long>(h));
        if (!h) return st::DllNotFound;
        if (length < sizeof(h)) return st::InfoLengthMismatch;
        std::memcpy(buffer, &h, sizeof(h));
        if (ret_len) *ret_len = sizeof(h);
        return st::Success;
    }
    case kMemoryWineLoadUnixLibByName: {
        auto* us = static_cast<const UNICODE_STRING*>(addr);
        std::wstring name = us && us->Buffer ? StripModuleName(std::wstring_view(us->Buffer, us->Length / 2)) : L"";
        std::uint64_t h = name.empty() ? 0 : UnixLibHandleByName(name);
        TRACE(dispatch, "MemoryWineLoadUnixLibByName %ls -> %llx", name.c_str(), static_cast<unsigned long long>(h));
        if (!h) return st::DllNotFound;
        std::uint64_t res[2] = {h, h};
        if (length < sizeof(res[0])) return st::InfoLengthMismatch;
        std::memcpy(buffer, res, length >= sizeof(res) ? sizeof(res) : sizeof(res[0]));
        return st::Success;
    }
    case kMemoryWineLoadUnixLibWow64:
    case kMemoryWineLoadUnixLibByNameWow64:
        return st::DllNotFound;
    case kMemoryWineUnloadUnixLib:
        return st::Success;
    default:
        break;
    }
    NTSTATUS s = Host().NtQueryVirtualMemory(process, addr, klass, buffer, length, ret_len);
    if (!IsCurrentProcess(process)) return s;
    ImageView v;
    if (klass == kMemoryBasicInformation && s == st::Success && length >= sizeof(MEMORY_BASIC_INFORMATION) &&
        FindView(addr, &v)) {
        // Our views are private memory; report them like SEC_IMAGE views
        // (Wine's build_ntdll_module takes AllocationBase as the module base).
        auto* mbi = static_cast<MEMORY_BASIC_INFORMATION*>(buffer);
        if (mbi->State != MEM_FREE) {
            mbi->Type = MEM_IMAGE;
            mbi->AllocationBase = v.image.base;
            mbi->AllocationProtect = PAGE_EXECUTE_WRITECOPY;
        }
    } else if (klass == kMemoryMappedFilenameInformation && FindView(addr, &v)) {
        std::wstring guest = v.section ? v.section->guest_path : std::wstring();
        if (guest.empty()) return s;
        ULONG rl = 0;
        s = WriteUnicodeStringInfo(GuestWin32ToDevicePath(guest), buffer, static_cast<ULONG>(length), &rl);
        if (ret_len) *ret_len = rl;
        if (s == st::InfoLengthMismatch) s = st::BufferOverflow;
    }
    return s;
}

// ---- Process / thread / system information ------------------------------------------------

static NTSTATUS NTAPI Xl_NtQueryInformationProcess(HANDLE process, ULONG klass, PVOID buffer, ULONG length,
                                                   PULONG ret_len) {
    if (IsCurrentProcess(process)) {
        const std::wstring guest = process::GuestImagePath();
        switch (klass) {
        case kProcessImageInformation:
            if (const std::uint8_t* info = MainImageInfo()) {
                if (ret_len) *ret_len = 0x40;
                if (length != 0x40) return st::InfoLengthMismatch;
                std::memcpy(buffer, info, 0x40);
                return st::Success;
            }
            break;
        case kProcessImageFileName:
            if (!guest.empty()) return WriteUnicodeStringInfo(GuestWin32ToDevicePath(guest), buffer, length, ret_len);
            break;
        case kProcessImageFileNameWin32:
            if (!guest.empty()) return WriteUnicodeStringInfo(guest, buffer, length, ret_len);
            break;
        case kProcessWow64Information:
            if (ret_len) *ret_len = sizeof(ULONG_PTR);
            if (length != sizeof(ULONG_PTR)) return st::InfoLengthMismatch;
            *static_cast<ULONG_PTR*>(buffer) = 0;
            return st::Success;
        default:
            break;
        }
    }
    return Host().NtQueryInformationProcess(process, klass, buffer, length, ret_len);
}

static NTSTATUS NTAPI Xl_NtQueryInformationThread(HANDLE thread, ULONG klass, PVOID buffer, ULONG length,
                                                  PULONG ret_len) {
    if (klass == kThreadAmILastThread && IsCurrentThreadHandle(thread)) {
        ThreadState* self = CurrentThreadState();
        if (self && (self->flags & kThreadGuest)) {
            if (length != sizeof(ULONG)) return st::InfoLengthMismatch;
            *static_cast<ULONG*>(buffer) = GuestThreads().size() <= 1 ? 1 : 0;
            if (ret_len) *ret_len = sizeof(ULONG);
            return st::Success;
        }
    }
    return Host().NtQueryInformationThread(thread, klass, buffer, length, ret_len);
}

static NTSTATUS NTAPI Xl_NtQuerySystemInformation(ULONG klass, PVOID buffer, ULONG length, PULONG ret_len) {
#if WIN10XBOX_HAVE_CLOCK
    if (klass == kSystemTimeOfDayInformation) return clock::Clk_NtQuerySystemInformation_TimeOfDay(buffer, length, ret_len);
#endif
    if (klass == kSystemWineVersionInformation) {
        // Wine's layout: version\0build id\0sysname\0release\0
        static const char kInfo[] = "11.9\0win10xbox\0Windows\0" "10.0";
        if (ret_len) *ret_len = sizeof(kInfo);
        if (length < sizeof(kInfo)) return st::InfoLengthMismatch;
        std::memcpy(buffer, kInfo, sizeof(kInfo));
        return st::Success;
    }
    return Host().NtQuerySystemInformation(klass, buffer, length, ret_len);
}

static NTSTATUS NTAPI Xl_NtQuerySecurityObject(HANDLE h, ULONG info, PVOID sd, ULONG length, PULONG ret_len) {
#if WIN10XBOX_HAVE_TOKEN
    if (token::IsTokenHandle(h))
        return token::Tok_NtQuerySecurityObject(h, info, static_cast<PSECURITY_DESCRIPTOR>(sd), length, ret_len);
#endif
    return Host().NtQuerySecurityObject(h, info, sd, length, ret_len);
}

// ---- Termination ---------------------------------------------------------------------------

static NTSTATUS NTAPI Xl_NtTerminateProcess(HANDLE process, NTSTATUS code) {
    if (!process) {
        // Wine's RtlExitUserProcess: kill every other thread of the (guest) process.
        TerminateOtherGuestThreads(code);
        return st::Success;
    }
    if (IsCurrentProcess(process)) process::GuestExit(static_cast<std::uint32_t>(code));
    return Host().NtTerminateProcess(process, code);
}

static NTSTATUS NTAPI Xl_NtTerminateThread(HANDLE thread, NTSTATUS code) {
    if (!thread || IsCurrentThreadHandle(thread)) {
        ThreadState* self = CurrentThreadState();
        if (self && (self->flags & kThreadGuest)) GuestThreadExit(code);
        return Host().NtTerminateThread(thread, code);
    }
    DWORD tid = GetThreadId(thread);
    NTSTATUS s = Host().NtTerminateThread(thread, code);
    if (NtSuccess(s) && tid) ForgetGuestThread(tid);
    return s;
}

static NTSTATUS NTAPI Xl_NtRaiseHardError(NTSTATUS error, ULONG nparams, ULONG string_mask, PULONG_PTR params,
                                          ULONG options, PULONG response) {
    std::string text;
    for (ULONG i = 0; i < nparams && i < 8 && params; ++i) {
        char buf[600];
        if (string_mask & (1u << i)) {
            auto* us = reinterpret_cast<const UNICODE_STRING*>(params[i]);
            std::snprintf(buf, sizeof buf, " %.*ls", us && us->Buffer ? int(us->Length / 2) : 0,
                          us && us->Buffer ? us->Buffer : L"");
        } else {
            std::snprintf(buf, sizeof buf, " %llx", static_cast<unsigned long long>(params[i]));
        }
        text += buf;
    }
    ERR(process, "hard error %08lx (option %lu):%s", static_cast<unsigned long>(error), options, text.c_str());
    if (response) *response = 6;   // ResponseOk
    return st::Success;
}

// ---- Context switches: the TEB must show the guest view when guest code resumes ----

static NTSTATUS NTAPI Xl_NtContinue(PCONTEXT ctx, BOOLEAN alert) {
    NoteGuestEntry(ctx);   // dumps the guest module list once, at the launch continuation
    return CallInGuestView(Host().NtContinue, ctx, alert);
}

static NTSTATUS NTAPI Xl_NtContinueEx(PCONTEXT ctx, PVOID arg) {
    NoteGuestEntry(ctx);
    return CallInGuestView(Host().NtContinueEx, ctx, arg);
}

static NTSTATUS NTAPI Xl_NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx, BOOLEAN first) {
    return CallInGuestView(Host().NtRaiseException, rec, ctx, first);
}

static NTSTATUS NTAPI Xl_NtSetContextThread(HANDLE thread, PCONTEXT ctx) {
    if (!IsCurrentThreadHandle(thread)) return Host().NtSetContextThread(thread, ctx);
    return CallInGuestView(Host().NtSetContextThread, thread, ctx);
}

static NTSTATUS NTAPI Xl_NtCreateThreadEx(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, HANDLE process,
                                          PVOID start, PVOID arg, ULONG flags, SIZE_T zero_bits, SIZE_T stack,
                                          SIZE_T max_stack, PVOID attr_list) {
    if (!IsCurrentProcess(process))
        return Host().NtCreateThreadEx(handle, access, attr, process, start, arg, flags, zero_bits, stack, max_stack,
                                       attr_list);
    return CreateGuestThread(handle, access, attr, start, arg, flags, zero_bits, stack, max_stack, attr_list);
}

// NtGetNextThread on the current process enumerates the GUEST's threads only. Wine's loader
// (alloc_tls_slot, loader.c) walks every thread NtGetNextThread returns when a DLL with a TLS
// directory is loaded after startup, and replaces each thread's TEB.ThreadLocalStoragePointer with a
// block sized for the guest's module count: done to a host thread (the host's main thread, the crash
// thread...) that is a heap block the host's ntdll later frees as its own -> 0xC0000374 on real
// Windows the first time a guest loaded native msvcrt.dll (via user32 -> advapi32).
using PFN_NtGetNextThread = NTSTATUS(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

NTSTATUS NTAPI Xl_NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags,
                                  PHANDLE handle) {
    auto next_fn = reinterpret_cast<PFN_NtGetNextThread>(HostExport("NtGetNextThread"));
    if (!next_fn) return st::NotImplemented;
    if (!IsCurrentProcess(process)) return next_fn(process, thread, access, attributes, flags, handle);
    std::vector<DWORD> guests;
    for (ThreadState* t : GuestThreads()) guests.push_back(t->tid);
    HANDLE cur = thread;
    bool cur_is_ours = false;   // a handle we opened while skipping (the caller's own one is never closed)
    for (;;) {
        HANDLE next = nullptr;
        const NTSTATUS s = next_fn(process, cur, access | THREAD_QUERY_LIMITED_INFORMATION, attributes, flags, &next);
        if (cur_is_ours) Host().NtClose(cur);
        if (!NtSuccess(s)) return s;
        const DWORD tid = GetThreadId(next);
        if (std::find(guests.begin(), guests.end(), tid) != guests.end()) {
            if (handle) *handle = next;
            else Host().NtClose(next);
            return st::Success;
        }
        cur = next;
        cur_is_ours = true;
    }
}

const ImplDef* ProcessImpls() {
    static const ImplDef k[] = {
        {"NtClose", reinterpret_cast<void*>(&Xl_NtClose)},
        {"NtDuplicateObject", reinterpret_cast<void*>(&Xl_NtDuplicateObject)},
        {"NtQueryVirtualMemory", reinterpret_cast<void*>(&Xl_NtQueryVirtualMemory)},
        {"NtQueryInformationProcess", reinterpret_cast<void*>(&Xl_NtQueryInformationProcess)},
        {"NtQueryInformationThread", reinterpret_cast<void*>(&Xl_NtQueryInformationThread)},
        {"NtQuerySystemInformation", reinterpret_cast<void*>(&Xl_NtQuerySystemInformation)},
        {"NtQuerySecurityObject", reinterpret_cast<void*>(&Xl_NtQuerySecurityObject)},
        {"NtTerminateProcess", reinterpret_cast<void*>(&Xl_NtTerminateProcess)},
        {"NtTerminateThread", reinterpret_cast<void*>(&Xl_NtTerminateThread)},
        {"NtRaiseHardError", reinterpret_cast<void*>(&Xl_NtRaiseHardError)},
        {"NtContinue", reinterpret_cast<void*>(&Xl_NtContinue)},
        {"NtContinueEx", reinterpret_cast<void*>(&Xl_NtContinueEx)},
        {"NtRaiseException", reinterpret_cast<void*>(&Xl_NtRaiseException)},
        {"NtSetContextThread", reinterpret_cast<void*>(&Xl_NtSetContextThread)},
        {"NtCreateThreadEx", reinterpret_cast<void*>(&Xl_NtCreateThreadEx)},
        {"NtGetNextThread", reinterpret_cast<void*>(&Xl_NtGetNextThread)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
