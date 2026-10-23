// Handle routing, memory/process/thread queries, termination, context
// switches and hard errors.
#include "internal.h"
#include "nt_spawn.h"
#include "vprocess.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_AFD
#include "win10xbox/afd.h"
#endif
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
constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
constexpr ULONG kThreadAmILastThread = 12;
// Private class (wine-pe patch 0005, alloc_tls_slot): where is the GUEST's static-TLS vector of this thread? See below.
constexpr ULONG kThreadGuestTlsLocation = 0x10020;
// SYSTEM_INFORMATION_CLASS
#if WIN10XBOX_HAVE_CLOCK
constexpr ULONG kSystemTimeOfDayInformation = 3;
#endif
constexpr ULONG kSystemCpuInformation = 1;
constexpr ULONG kSystemEmulationProcessorInformation = 63;
constexpr ULONG kSystemFirmwareTableInformation = 76;
constexpr ULONG kSystemWineVersionInformation = 1000;

// A UNICODE_STRING followed by its characters, written into the caller's buffer (the string points into it).
NTSTATUS WriteUnicodeStringInfo(const std::wstring& s, PVOID buffer, ULONG length, PULONG ret_len) {
    const ULONG bytes = static_cast<ULONG>(s.size() * sizeof(wchar_t));
    const ULONG need = static_cast<ULONG>(sizeof(UNICODE_STRING)) + bytes + sizeof(wchar_t);
    if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
    if (ret_len)
        if (NTSTATUS ps = probe::Store<ULONG>(ret_len, need)) return ps;
    if (length < need) return st::InfoLengthMismatch;
    std::vector<std::uint8_t> image(need);
    auto* us = reinterpret_cast<UNICODE_STRING*>(image.data());
    std::memcpy(image.data() + sizeof(UNICODE_STRING), s.c_str(), bytes + sizeof(wchar_t));
    us->Buffer = reinterpret_cast<wchar_t*>(static_cast<std::uint8_t*>(buffer) + sizeof(UNICODE_STRING));
    us->Length = static_cast<USHORT>(bytes);
    us->MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
    return probe::CopyToUser(buffer, image.data(), image.size());
}

std::wstring StripModuleName(std::wstring_view name) {
    std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
    return std::wstring(name);
}

}  // namespace

std::string ModuleExportName(const void* image_base) {
    // `image_base` is a guest pointer (MemoryWineLoadUnixLib hands over whatever the caller passed): every read is
    // guarded, and anything that is not a PE image with an export name gives an empty string.
    auto* base = static_cast<const std::uint8_t*>(image_base);
    IMAGE_DOS_HEADER dos;
    if (probe::Load<IMAGE_DOS_HEADER>(reinterpret_cast<const IMAGE_DOS_HEADER*>(base), &dos) != st::Success ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return {};
    IMAGE_NT_HEADERS64 nt;
    if (probe::Load<IMAGE_NT_HEADERS64>(reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos.e_lfanew), &nt) != st::Success ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return {};
    const auto& d = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return {};
    IMAGE_EXPORT_DIRECTORY exp;
    if (probe::Load<IMAGE_EXPORT_DIRECTORY>(reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress),
                                            &exp) != st::Success || !exp.Name)
        return {};
    std::string name;
    for (std::size_t i = 0; i < 256; ++i) {   // a module name is short; stop at the NUL or at 256 characters
        char c;
        if (probe::CopyFromUser(&c, base + exp.Name + i, 1) != st::Success || !c) break;
        name.push_back(c);
    }
    return name;
}

// ---- Handles ----------------------------------------------------------------------

NTSTATUS NTAPI Xl_NtClose(HANDLE h) {
#if WIN10XBOX_HAVE_REGISTRY
    if (registry::IsKeyHandle(h)) return registry::Close(h);
#endif
#if WIN10XBOX_HAVE_TOKEN
    if (token::IsTokenHandle(h)) return token::Close(h);
#endif
#if WIN10XBOX_HAVE_AFD
    if (afd::IsSocketHandle(h)) return afd::Close(h);
#endif
    EmulatedObject o;
    if (LookupEmulatedObject(h, &o)) {
        if (o.kind == ObjectKind::ImageSection) CloseImageSection(o);
        if (o.kind == ObjectKind::Directory || o.kind == ObjectKind::SymbolicLink) CloseEmulatedDirectory(o);
        UnregisterEmulatedObject(h);
        return st::Success;
    }
    ForgetNamedHandle(h);
    ForgetVirtualProcessHandle(h);
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
    if (!emulated) {
        NTSTATUS s = Host().NtDuplicateObject(src_process, src, dst_process, dst, access, attributes, options);
        if (NtSuccess(s) && IsCurrentProcess(src_process) && IsCurrentProcess(dst_process) && dst) {
            NoteDuplicatedNamedHandle(src, *dst);
            NoteDuplicatedVirtualProcessHandle(src, *dst);
            if (options & DUPLICATE_CLOSE_SOURCE) { ForgetNamedHandle(src); ForgetVirtualProcessHandle(src); }
        }
        return s;
    }
    if (IsCurrentProcess(dst_process) && o.extra && (o.kind == ObjectKind::Directory || o.kind == ObjectKind::SymbolicLink)) {
        HANDLE h = DuplicateEmulatedDirectory(o, access, options);
        if (!h) return st::NoMemory;
        if (dst) *dst = h;
        if (options & DUPLICATE_CLOSE_SOURCE) Xl_NtClose(src);
        return st::Success;
    }
    if (!IsCurrentProcess(dst_process) || o.kind != ObjectKind::ImageSection || !o.extra) {
        FIXME(dispatch, "duplicating emulated handle %p (kind %d)", src, static_cast<int>(o.kind));
        return st::NotImplemented;
    }
    if (dst)
        if (NTSTATUS ps = probe::ProbeWrite(dst, sizeof(HANDLE))) return ps;
    HANDLE h = DuplicateImageSection(o);
    if (!h) return st::NoMemory;
    if (dst)
        if (NTSTATUS ws = probe::Store<HANDLE>(dst, h)) {
            Xl_NtClose(h);   // the caller's slot vanished: do not leak the duplicate
            return ws;
        }
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
        if (NTSTATUS ws = probe::CopyToUser(buffer, &h, sizeof(h))) return ws;
        return probe::StoreIfPresent<SIZE_T>(ret_len, sizeof(h));
    }
    case kMemoryWineLoadUnixLibByName: {
        std::wstring name;
        if (addr) {
            probe::String module;
            if (NTSTATUS cs = probe::CaptureString(static_cast<const UNICODE_STRING*>(addr), &module)) return cs;
            name = StripModuleName(module.view());
        }
        std::uint64_t h = name.empty() ? 0 : UnixLibHandleByName(name);
        TRACE(dispatch, "MemoryWineLoadUnixLibByName %ls -> %llx", name.c_str(), static_cast<unsigned long long>(h));
        if (!h) return st::DllNotFound;
        std::uint64_t res[2] = {h, h};
        if (length < sizeof(res[0])) return st::InfoLengthMismatch;
        return probe::CopyToUser(buffer, res, length >= sizeof(res) ? sizeof(res) : sizeof(res[0]));
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
        MEMORY_BASIC_INFORMATION mbi;
        if (probe::Load<MEMORY_BASIC_INFORMATION>(static_cast<const MEMORY_BASIC_INFORMATION*>(buffer), &mbi) != st::Success)
            return s;   // the host has just filled it, so this only happens to a buffer unmapped meanwhile
        if (mbi.State != MEM_FREE) {
            mbi.Type = MEM_IMAGE;
            mbi.AllocationBase = v.image.base;
            mbi.AllocationProtect = PAGE_EXECUTE_WRITECOPY;
            if (NTSTATUS ws = probe::Store<MEMORY_BASIC_INFORMATION>(static_cast<MEMORY_BASIC_INFORMATION*>(buffer), mbi))
                return ws;
        }
    } else if (klass == kMemoryMappedFilenameInformation && FindView(addr, &v)) {
        std::wstring guest = v.section ? v.section->guest_path : std::wstring();
        if (guest.empty()) return s;
        ULONG rl = 0;
        s = WriteUnicodeStringInfo(GuestWin32ToDevicePath(guest), buffer, static_cast<ULONG>(length), &rl);
        if (s != st::AccessViolation && ret_len)
            if (NTSTATUS ws = probe::Store<SIZE_T>(ret_len, rl)) return ws;
        if (s == st::InfoLengthMismatch) s = st::BufferOverflow;
    }
    return s;
}

// ---- Process / thread / system information ------------------------------------------------

// ProcessBasicInformation of a virtual process: the host's answer for the real fields (affinity, priority), with the
// virtual process's own exit status, PEB and ids.
static NTSTATUS VirtualBasicInformation(VirtualProcess* vp, PVOID buffer, ULONG length, PULONG ret_len) {
    struct Basic { NTSTATUS exit_status; PVOID peb; ULONG_PTR affinity; LONG priority; ULONG_PTR pid, parent; };
    if (length != sizeof(Basic)) return st::InfoLengthMismatch;
    if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
    Basic b{};
    if (NTSTATUS hs = Host().NtQueryInformationProcess(reinterpret_cast<HANDLE>(-1), 0, &b, sizeof b, nullptr)) return hs;
    b.exit_status = vp->exited ? static_cast<NTSTATUS>(vp->exit_code) : 259;   // STATUS_PENDING while it runs
    b.peb = vp->peb;
    b.pid = vp->pid;
    b.parent = vp->parent_pid;
    if (ret_len)
        if (NTSTATUS ps = probe::Store<ULONG>(ret_len, sizeof b)) return ps;
    return probe::CopyToUser(buffer, &b, sizeof b);
}

static NTSTATUS NTAPI Xl_NtQueryInformationProcess(HANDLE process, ULONG klass, PVOID buffer, ULONG length,
                                                   PULONG ret_len) {
    // A virtual process answers for itself, whether it asks about the pseudo handle or another process holds its handle.
    VirtualProcess* const vp = IsCurrentProcess(process) ? CurrentVirtualProcess() : VirtualProcessFromHandle(process);
    if (vp && klass == 0 /*ProcessBasicInformation*/) return VirtualBasicInformation(vp, buffer, length, ret_len);
    if (vp && !IsCurrentProcess(process) && klass != kProcessImageInformation && klass != kProcessImageFileName &&
        klass != kProcessImageFileNameWin32)
        return st::InvalidInfoClass;   // the handle names an event, not a host process
    if (IsCurrentProcess(process) || vp) {
        const std::wstring guest = vp ? vp->image_path : process::GuestImagePath();
        switch (klass) {
        case kProcessImageInformation:
            if (const std::uint8_t* info = vp ? (vp->image ? vp->image->info : nullptr) : MainImageInfo()) {
                if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
                if (ret_len)
                    if (NTSTATUS ps = probe::Store<ULONG>(ret_len, 0x40)) return ps;
                if (length != 0x40) return st::InfoLengthMismatch;
                return probe::CopyToUser(buffer, info, 0x40);
            }
            break;
        case kProcessImageFileName:
            if (!guest.empty()) return WriteUnicodeStringInfo(GuestWin32ToDevicePath(guest), buffer, length, ret_len);
            break;
        case kProcessImageFileNameWin32:
            if (!guest.empty()) return WriteUnicodeStringInfo(guest, buffer, length, ret_len);
            break;
        case kProcessWow64Information: {
            // The length is checked first and the caller's memory is untouched when it is wrong; a NULL buffer is an
            // access violation, as when the host kernel answers.
            if (length != sizeof(ULONG_PTR)) return st::InfoLengthMismatch;
            if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
            if (ret_len)
                if (NTSTATUS ps = probe::ProbeWrite(ret_len, sizeof(ULONG), sizeof(ULONG))) return ps;
            if (NTSTATUS ws = probe::Store<ULONG_PTR>(static_cast<ULONG_PTR*>(buffer), 0)) return ws;
            return probe::StoreIfPresent<ULONG>(ret_len, sizeof(ULONG_PTR));
        }
        default:
            break;
        }
    }
    return Host().NtQueryInformationProcess(process, klass, buffer, length, ret_len);
}

// ThreadBasicInformation (class 0), the layout the kernel returns.
struct ThreadBasicInfo {
    NTSTATUS exit_status;
    PVOID teb;
    CLIENT_ID client_id;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
};

// Class kThreadGuestTlsLocation: the address of the pointer that holds the guest's static-TLS vector of `thread`.
// That is the TEB field (TEB+0x58) of a thread that is running guest code, or of the CALLING thread (it is in the
// host view only while this call runs and is back in the guest view when the caller reads the answer); for another
// guest thread that is inside the dispatcher (blocked in a system call: depth > 0) the TEB shows the HOST's vector
// and the guest's is parked in its ThreadState. Wine's alloc_tls_slot replaces slot i of every thread's vector when
// a DLL with a TLS directory is loaded after startup; without this it replaced (and freed) slots of the host's
// vector of every thread that was waiting in a system call: heap corruption on the first such load (dxcore.dll
// after d3d11 had made thread-pool and timer-queue threads). docs/DISPATCHER.md, "Guest view and host view".
static NTSTATUS GuestTlsLocation(HANDLE thread, PVOID buffer, ULONG length, PULONG ret_len) {
    if (length != sizeof(void*)) return st::InfoLengthMismatch;
    ThreadBasicInfo tbi{};
    const NTSTATUS s = Host().NtQueryInformationThread(thread, 0, &tbi, sizeof tbi, nullptr);
    if (!NtSuccess(s)) return s;
    if (!tbi.teb) return st::InvalidParameter;
    void* location = static_cast<std::uint8_t*>(tbi.teb) + teb::ThreadLocalStoragePointer;
    const DWORD tid = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(tbi.client_id.UniqueThread));
    if (tid != GetCurrentThreadId()) {
        for (ThreadState* t : GuestThreads())
            if (t->tid == tid) {
                if (t->depth) location = &t->guest_tls;
                break;
            }
    }
    *static_cast<void**>(buffer) = location;
    if (ret_len) *ret_len = sizeof(void*);
    return st::Success;
}

static NTSTATUS NTAPI Xl_NtQueryInformationThread(HANDLE thread, ULONG klass, PVOID buffer, ULONG length,
                                                  PULONG ret_len) {
    if (klass == kThreadGuestTlsLocation) return GuestTlsLocation(thread, buffer, length, ret_len);
    if (klass == kThreadAmILastThread) {
        // Wine's RtlExitUserThread leaves its out value uninitialised when this fails, and a nonzero stack leftover
        // ends the whole process with the thread's exit code. Trace every query so the answer is on record.
        const bool current = IsCurrentThreadHandle(thread);
        ThreadState* self = CurrentThreadState();
        TRACE(process, "thread %04lx: NtQueryInformationThread(ThreadAmILastThread) current=%d state=%p flags=%x guests=%zu",
              GetCurrentThreadId(), current ? 1 : 0, static_cast<void*>(self), self ? static_cast<unsigned>(self->flags) : 0u,
              GuestThreads().size());
        if (current && self && (self->flags & kThreadGuest)) {
            if (length != sizeof(ULONG)) return st::InfoLengthMismatch;
            if (NTSTATUS ps = probe::ProbeWrite(buffer, length, sizeof(ULONG))) return ps;
            if (ret_len)
                if (NTSTATUS ps = probe::ProbeWrite(ret_len, sizeof(ULONG), sizeof(ULONG))) return ps;
            const std::size_t guests = CountGuestThreads(self->vp);
            TRACE(process, "thread %04lx: ThreadAmILastThread -> %lu (%zu guest threads)", GetCurrentThreadId(),
                  static_cast<unsigned long>(guests <= 1 ? 1 : 0), guests);
            if (NTSTATUS ws = probe::Store<ULONG>(static_cast<ULONG*>(buffer), guests <= 1 ? 1 : 0))
                return ws;
            return probe::StoreIfPresent<ULONG>(ret_len, sizeof(ULONG));
        }
    }
    if (klass == kThreadQuerySetWin32StartAddress && IsCurrentThreadHandle(thread)) {
        // The host thread started in GuestThreadProc; a guest sees the address it asked for.
        ThreadState* self = CurrentThreadState();
        if (self && (self->flags & kThreadGuest) && self->win32_start) {
            if (length != sizeof(PVOID)) return st::InfoLengthMismatch;
            if (NTSTATUS ps = probe::ProbeWrite(buffer, length, sizeof(PVOID))) return ps;
            if (ret_len)
                if (NTSTATUS ps = probe::ProbeWrite(ret_len, sizeof(ULONG), sizeof(ULONG))) return ps;
            if (NTSTATUS ws = probe::Store<PVOID>(static_cast<PVOID*>(buffer), self->win32_start)) return ws;
            return probe::StoreIfPresent<ULONG>(ret_len, sizeof(PVOID));
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
        if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
        if (ret_len)
            if (NTSTATUS ps = probe::Store<ULONG>(ret_len, sizeof(kInfo))) return ps;
        if (length < sizeof(kInfo)) return st::InfoLengthMismatch;
        return probe::CopyToUser(buffer, kInfo, sizeof(kInfo));
    }
    if (klass == kSystemFirmwareTableInformation) {   // D18: firmware tables are denied on the console
        NTSTATUS answer = st::Success;
        if (QuerySystemFirmwareTable(buffer, length, ret_len, &answer)) return answer;
    }
    const NTSTATUS host = Host().NtQuerySystemInformation(klass, buffer, length, ret_len);
    if ((klass == kSystemCpuInformation || klass == kSystemEmulationProcessorInformation) && NtSuccess(host) && buffer &&
        length >= 6) {
        // SYSTEM_CPU_INFORMATION: ProcessorLevel is the family, ProcessorRevision is model << 8 | stepping (what
        // GetSystemInfo reports); the CPUID patch serves the same machine identity. The emulation class (63, the x86
        // view of the same processor) reports the same two words.
        const auto& id = config::Current().machine_identity;
        auto* words = static_cast<USHORT*>(buffer);
        words[1] = static_cast<USHORT>(id.cpu_family);
        words[2] = static_cast<USHORT>((id.cpu_model << 8) | id.cpu_stepping);
    }
    return host;
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
    TRACE(process, "thread %04lx: NtTerminateProcess(%p, %08lx)", GetCurrentThreadId(), process,
          static_cast<unsigned long>(code));
    if (!process) {
        // Wine's RtlExitUserProcess: kill every other thread of the (guest) process.
        TerminateOtherGuestThreads(code);
        return st::Success;
    }
    if (IsCurrentProcess(process)) {
        if (VirtualProcess* vp = CurrentVirtualProcess()) ExitVirtualProcess(vp, code);
        process::GuestExit(static_cast<std::uint32_t>(code));
    }
    if (VirtualProcess* vp = VirtualProcessFromHandle(process)) {
        if (vp == CurrentVirtualProcess()) ExitVirtualProcess(vp, code);
        return TerminateVirtualProcess(vp, code);
    }
    return Host().NtTerminateProcess(process, code);
}

static NTSTATUS NTAPI Xl_NtTerminateThread(HANDLE thread, NTSTATUS code) {
    TRACE(process, "thread %04lx: NtTerminateThread(%p, %08lx)", GetCurrentThreadId(), thread,
          static_cast<unsigned long>(code));
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
    if (response)
        if (NTSTATUS ps = probe::ProbeWrite(response, sizeof(ULONG), sizeof(ULONG))) return ps;
    // The parameter array and the strings in it are the caller's: copied before they are printed.
    ULONG_PTR values[8] = {};
    const ULONG shown = nparams < 8 ? nparams : 8;
    if (params && shown)
        if (NTSTATUS cs = probe::CopyFromUser(values, params, shown * sizeof(ULONG_PTR))) return cs;
    std::string text;
    for (ULONG i = 0; i < shown && params; ++i) {
        char buf[600];
        if (string_mask & (1u << i)) {
            probe::String us;
            if (values[i])
                if (NTSTATUS cs = probe::CaptureString(reinterpret_cast<const UNICODE_STRING*>(values[i]), &us)) return cs;
            std::snprintf(buf, sizeof buf, " %.*ls", int(us.text.size()), us.text.c_str());
        } else {
            std::snprintf(buf, sizeof buf, " %llx", static_cast<unsigned long long>(values[i]));
        }
        text += buf;
    }
    ERR(process, "hard error %08lx (option %lu):%s", static_cast<unsigned long>(error), options, text.c_str());
    return probe::StoreIfPresent<ULONG>(response, 6);   // ResponseOk
}

// ---- Context switches: the TEB must show the guest view when guest code resumes ----

// NtContinue reads a whole CONTEXT, aligned to 16 bytes: Windows says so before it switches anything.
static NTSTATUS CheckContext(const CONTEXT* ctx) {
    return probe::ProbeRead(ctx, sizeof(CONTEXT), 16);
}

static NTSTATUS NTAPI Xl_NtContinue(PCONTEXT ctx, BOOLEAN alert) {
    if (NTSTATUS ps = CheckContext(ctx)) return ps;
    NoteGuestEntry(ctx);   // dumps the guest module list once, at the launch continuation
    return CallInGuestView(Host().NtContinue, ctx, alert);
}

static NTSTATUS NTAPI Xl_NtContinueEx(PCONTEXT ctx, PVOID arg) {
    if (NTSTATUS ps = CheckContext(ctx)) return ps;
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
            if (handle) {
                if (NTSTATUS ws = probe::Store<HANDLE>(handle, next)) {
                    Host().NtClose(next);
                    return ws;
                }
            } else {
                Host().NtClose(next);
            }
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
        // nt_spawn.cpp: child processes and the refusals around them
        {"NtCreateUserProcess", reinterpret_cast<void*>(&Xl_NtCreateUserProcess)},
        {"NtCreateProcessEx", reinterpret_cast<void*>(&Xl_NtCreateProcessEx)},
        {"NtCreateThread", reinterpret_cast<void*>(&Xl_NtCreateThread)},
        {"NtCreateDebugObject", reinterpret_cast<void*>(&Xl_NtCreateDebugObject)},
        {"NtDebugActiveProcess", reinterpret_cast<void*>(&Xl_NtDebugActiveProcess)},
        {"NtSetSystemInformation", reinterpret_cast<void*>(&Xl_NtSetSystemInformation)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
