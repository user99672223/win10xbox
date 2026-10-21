// ntoskrnl Ob*/Ps*/Mm*/Se*/Etw*/Cm* surface and the data exports
// (PsLoadedModuleList, MmUserProbeAddress, *ObjectType ...). MIT.
//
// Process and thread objects are pseudo objects: PEPROCESS is one blob for
// the guest process (and one for "System"), PETHREAD a PseudoThread per host
// thread. They are enough for the common driver uses (ids, current
// process/thread, waiting on a system thread you created).
#include "internal.h"
#include "win10xbox/dispatch.h"
#include <vector>

namespace win10xbox::drivers::detail {

// Implemented in host/.
PFILE_OBJECT HostFileObjectForHandle(HANDLE h);
DRIVER_OBJECT* DriverByName(std::wstring_view name);

namespace {
struct PseudoProcess {
    DISPATCHER_HEADER hdr;
    DWORD pid;
    char image_name[16];
};
PseudoProcess g_guest_process{};
PseudoProcess g_system_process{};
bool g_proc_init = false;

void InitProcesses() {
    if (g_proc_init) return;
    g_proc_init = true;
    g_guest_process.hdr.Type = NTK_TYPE_PROCESS;
    g_guest_process.hdr.Size = sizeof(DISPATCHER_HEADER) / 4;
    InitializeListHead(&g_guest_process.hdr.WaitListHead);
    g_guest_process.pid = GetCurrentProcessId();
    std::string name = "win10xbox";
    std::wstring img = win10xbox::process::GuestImagePath();
    if (!img.empty()) {
        size_t slash = img.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? img : img.substr(slash + 1);
        name.assign(base.begin(), base.end());
    }
    std::strncpy(g_guest_process.image_name, name.c_str(), sizeof(g_guest_process.image_name) - 1);
    g_system_process = g_guest_process;
    g_system_process.pid = 4;
    std::strcpy(g_system_process.image_name, "System");
    ObjRegister(&g_guest_process, ObjType::Process, nullptr, 1);
    ObjRegister(&g_system_process, ObjType::Process, nullptr, 1);
}
}  // namespace

PVOID CurrentPseudoProcess() { InitProcesses(); return &g_guest_process; }
PVOID SystemPseudoProcess() { InitProcesses(); return &g_system_process; }

namespace {
struct ThreadStart {
    PKSTART_ROUTINE routine;
    PVOID ctx;
    PseudoThread* pt;
};

void FinishSystemThread(PseudoThread* pt) {
    AcquireSRWLockExclusive(&g_disp_lock);
    pt->hdr.SignalState = 1;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
}

DWORD WINAPI SystemThreadMain(LPVOID p) {
    ThreadStart ts = *static_cast<ThreadStart*>(p);
    PoolFree(p, 0, false);
    SwapIrql(PASSIVE_LEVEL);
    PseudoThread* pt = PseudoThreadForTid(GetCurrentThreadId(), true);
    ts.pt = pt;
    ts.routine(ts.ctx);
    FinishSystemThread(pt);
    return 0;
}

struct NotifyEntry { void* routine; int kind; };
SRWLOCK g_notify_lock = SRWLOCK_INIT;
std::vector<NotifyEntry>& Notifies() { static std::vector<NotifyEntry> v; return v; }
enum { kNotifyLoadImage = 1, kNotifyCreateProcess = 2, kNotifyCreateThread = 3 };

std::vector<std::pair<std::wstring, PKEVENT>>& NamedEvents() { static std::vector<std::pair<std::wstring, PKEVENT>> v; return v; }
SRWLOCK g_named_lock = SRWLOCK_INIT;

// Dummy OBJECT_TYPE blobs: drivers pass *IoFileObjectType & co. and never look inside.
struct DummyType { char blob[0xD0]; };
DummyType g_types[12];
}  // namespace

// Called by the driver loader for every successfully loaded image.
void NotifyImageLoaded(const std::wstring& full_name, PVOID base, SIZE_T size) {
    std::vector<NotifyEntry> copy;
    AcquireSRWLockShared(&g_notify_lock);
    copy = Notifies();
    ReleaseSRWLockShared(&g_notify_lock);
    for (const NotifyEntry& n : copy) {
        if (n.kind != kNotifyLoadImage) continue;
        UNICODE_STRING us;
        us.Buffer = const_cast<PWSTR>(full_name.c_str());
        us.Length = static_cast<USHORT>(full_name.size() * sizeof(wchar_t));
        us.MaximumLength = us.Length + sizeof(wchar_t);
        struct {
            ULONG Properties;
            PVOID ImageBase;
            ULONG ImageSelector;
            SIZE_T ImageSize;
            ULONG ImageSectionNumber;
        } info{};
        info.Properties = (1u << 8);   // SystemModeImage
        info.ImageBase = base;
        info.ImageSize = size;
        using Fn = VOID(NTAPI*)(PUNICODE_STRING, HANDLE, PVOID);
        reinterpret_cast<Fn>(n.routine)(&us, nullptr, &info);
    }
}

}  // namespace win10xbox::drivers::detail

namespace ntk {
using namespace win10xbox::drivers::detail;
namespace dsp = win10xbox::dispatch;

// ---- Ob ----
PVOID g_obj_type_ptrs[12];
PVOID g_ps_initial_system_process = nullptr;
ULONG g_nt_build_number = 0xF0004A65;   // free build 19045
ULONG g_init_safe_boot_mode = 0;
ULONG_PTR g_mm_user_probe_address = 0x00007FFFFFFF0000ull;
PVOID g_mm_system_range_start = reinterpret_cast<PVOID>(0xFFFF800000000000ull);
PVOID g_mm_highest_user_address = reinterpret_cast<PVOID>(0x00007FFFFFFEFFFFull);
ULONG g_nt_global_flag = 0;
USHORT g_nls_mb_code_page_tag = 0;

NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE h, ACCESS_MASK access, PVOID type, KPROCESSOR_MODE mode, PVOID* object,
                                         PVOID handle_info) {
    (void)access; (void)type; (void)mode;
    if (handle_info) std::memset(handle_info, 0, 16);
    if (!object) return STATUS_INVALID_PARAMETER;
    *object = nullptr;
    if (h == reinterpret_cast<HANDLE>(-1)) { *object = CurrentPseudoProcess(); return STATUS_SUCCESS; }
    if (h == reinterpret_cast<HANDLE>(-2)) { *object = CurrentPseudoThread(); return STATUS_SUCCESS; }
    if (PFILE_OBJECT fo = HostFileObjectForHandle(h)) {
        ObjRef(fo);
        *object = fo;
        return STATUS_SUCCESS;
    }
    dsp::EmulatedObject eo;
    if (dsp::LookupEmulatedObject(h, &eo) && eo.kind == dsp::ObjectKind::Other && eo.extra) {
        ObjRef(eo.extra);
        *object = eo.extra;
        return STATUS_SUCCESS;
    }
    if (DWORD tid = GetThreadId(h)) {
        PseudoThread* t = PseudoThreadForTid(tid, true);
        if (t) { ObjRef(t); *object = t; return STATUS_SUCCESS; }
    }
    if (DWORD pid = GetProcessId(h)) {
        if (pid == GetCurrentProcessId()) { *object = CurrentPseudoProcess(); return STATUS_SUCCESS; }
    }
    FIXME(driver, "ObReferenceObjectByHandle(%p): host handle with no kernel object counterpart", h);
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS NTAPI ObReferenceObjectByHandleWithTag(HANDLE h, ACCESS_MASK access, PVOID type, KPROCESSOR_MODE mode, ULONG tag,
                                                PVOID* object, PVOID handle_info) {
    (void)tag;
    return ObReferenceObjectByHandle(h, access, type, mode, object, handle_info);
}
NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID object, ACCESS_MASK access, PVOID type, KPROCESSOR_MODE mode) {
    (void)access; (void)type; (void)mode;
    if (ObjRef(object) < 0) TRACE(driver, "ObReferenceObjectByPointer(%p): unmanaged object, no count kept", object);
    return STATUS_SUCCESS;
}
LONG_PTR NTAPI ObfReferenceObject(PVOID object) {
    LONG n = ObjRef(object);
    return n < 0 ? 0 : n - 1;
}
LONG_PTR NTAPI ObfDereferenceObject(PVOID object) {
    LONG n = ObjDeref(object);
    return n < 0 ? 0 : n;
}
LONG_PTR NTAPI ObfReferenceObjectWithTag(PVOID object, ULONG tag) { (void)tag; return ObfReferenceObject(object); }
LONG_PTR NTAPI ObfDereferenceObjectWithTag(PVOID object, ULONG tag) { (void)tag; return ObfDereferenceObject(object); }
VOID NTAPI ObDereferenceObjectDeferDelete(PVOID object) { ObfDereferenceObject(object); }
NTSTATUS NTAPI ObReferenceObjectByName(PUNICODE_STRING name, ULONG attrs, PVOID access_state, ACCESS_MASK access,
                                       PVOID type, KPROCESSOR_MODE mode, PVOID parse_ctx, PVOID* object) {
    (void)attrs; (void)access_state; (void)access; (void)type; (void)mode; (void)parse_ctx;
    if (!name || !object) return STATUS_INVALID_PARAMETER;
    *object = nullptr;
    if (PDEVICE_OBJECT d = NsResolve(View(*name), nullptr, nullptr)) { ObjRef(d); *object = d; return STATUS_SUCCESS; }
    if (PDRIVER_OBJECT drv = DriverByName(View(*name))) { ObjRef(drv); *object = drv; return STATUS_SUCCESS; }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
NTSTATUS NTAPI ObCloseHandle(HANDLE h, KPROCESSOR_MODE mode) {
    (void)mode;
    using Fn = NTSTATUS(NTAPI*)(HANDLE);
    auto fn = reinterpret_cast<Fn>(ResolveNtFunction("NtClose"));
    return fn ? fn(h) : STATUS_NOT_IMPLEMENTED;
}
VOID NTAPI ObMakeTemporaryObject(PVOID) {}
NTSTATUS NTAPI ObRegisterCallbacks(PVOID registration, PVOID* handle) {
    (void)registration;
    WARN(driver, "ObRegisterCallbacks: accepted, but process/thread handle callbacks are never delivered by the host");
    if (handle) *handle = reinterpret_cast<PVOID>(1);
    return STATUS_SUCCESS;
}
VOID NTAPI ObUnRegisterCallbacks(PVOID) {}
USHORT NTAPI ObGetFilterVersion(void) { return 0x100; }

// ---- Ps ----
HANDLE NTAPI PsGetCurrentProcessId(void) { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentProcessId())); }
HANDLE NTAPI PsGetCurrentThreadId(void) { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentThreadId())); }
PVOID NTAPI IoGetCurrentProcess(void) { return CurrentPseudoProcess(); }
PVOID NTAPI PsGetCurrentProcess(void) { return CurrentPseudoProcess(); }
PVOID NTAPI PsGetCurrentThread(void) { return CurrentPseudoThread(); }
HANDLE NTAPI PsGetProcessId(PVOID p) {
    ObjType t;
    if (!ObjLookup(p, &t) || t != ObjType::Process) return nullptr;
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(static_cast<PseudoProcess*>(p)->pid));
}
HANDLE NTAPI PsGetThreadId(PVOID t) { return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(static_cast<PseudoThread*>(t)->tid)); }
HANDLE NTAPI PsGetThreadProcessId(PVOID t) { (void)t; return PsGetCurrentProcessId(); }
PVOID NTAPI PsGetThreadProcess(PVOID t) { (void)t; return CurrentPseudoProcess(); }
const char* NTAPI PsGetProcessImageFileName(PVOID p) { return static_cast<PseudoProcess*>(p)->image_name; }
PVOID NTAPI PsGetProcessPeb(PVOID p) {
    ObjType t;
    if (!ObjLookup(p, &t) || t != ObjType::Process) return nullptr;
    return NtCurrentTeb()->ProcessEnvironmentBlock;
}
ULONG NTAPI PsGetProcessSessionId(PVOID) { return 1; }
ULONG NTAPI PsGetCurrentProcessSessionId(void) { return 1; }
BOOLEAN NTAPI PsIsSystemThread(PVOID) { return FALSE; }
NTSTATUS NTAPI PsLookupProcessByProcessId(HANDLE pid, PVOID* process) {
    ULONG_PTR id = reinterpret_cast<ULONG_PTR>(pid);
    if (id == GetCurrentProcessId()) { *process = CurrentPseudoProcess(); return STATUS_SUCCESS; }
    if (id == 4) { *process = SystemPseudoProcess(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}
NTSTATUS NTAPI PsLookupThreadByThreadId(HANDLE tid, PVOID* thread) {
    DWORD id = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(tid));
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!h) return STATUS_INVALID_PARAMETER;
    CloseHandle(h);
    PseudoThread* t = PseudoThreadForTid(id, true);
    if (!t) return STATUS_INSUFFICIENT_RESOURCES;
    ObjRef(t);
    *thread = t;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI PsCreateSystemThread(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES oa, HANDLE process, PVOID client_id,
                                    PKSTART_ROUTINE routine, PVOID ctx) {
    (void)access; (void)oa; (void)process;
    auto* ts = static_cast<ThreadStart*>(PoolAlloc(sizeof(ThreadStart), kTagMisc));
    if (!ts) return STATUS_INSUFFICIENT_RESOURCES;
    ts->routine = routine;
    ts->ctx = ctx;
    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, SystemThreadMain, ts, CREATE_SUSPENDED, &tid);
    if (!th) { PoolFree(ts, 0, false); return STATUS_INSUFFICIENT_RESOURCES; }
    PseudoThread* pt = PseudoThreadForTid(tid, true);   // created before the thread runs so waiters can reference it
    if (handle) *handle = th; else CloseHandle(th);
    if (client_id) {
        static_cast<HANDLE*>(client_id)[0] = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentProcessId()));
        static_cast<HANDLE*>(client_id)[1] = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tid));
    }
    (void)pt;
    if (handle) ResumeThread(*handle); else ResumeThread(th);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI PsTerminateSystemThread(NTSTATUS status) {
    FinishSystemThread(PseudoThreadForTid(GetCurrentThreadId(), true));
    ExitThread(static_cast<DWORD>(status));
    return status;
}
BOOLEAN NTAPI PsGetVersion(PULONG major, PULONG minor, PULONG build, PUNICODE_STRING csd) {
    if (major) *major = 10;
    if (minor) *minor = 0;
    if (build) *build = 19045;
    if (csd) { csd->Length = 0; }
    return FALSE;
}
NTSTATUS NTAPI PsSetLoadImageNotifyRoutine(PVOID routine) {
    AcquireSRWLockExclusive(&g_notify_lock);
    Notifies().push_back({routine, kNotifyLoadImage});
    ReleaseSRWLockExclusive(&g_notify_lock);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI PsRemoveLoadImageNotifyRoutine(PVOID routine) {
    AcquireSRWLockExclusive(&g_notify_lock);
    NTSTATUS st = STATUS_PROCEDURE_NOT_FOUND;
    for (auto it = Notifies().begin(); it != Notifies().end(); ++it)
        if (it->routine == routine && it->kind == kNotifyLoadImage) { Notifies().erase(it); st = STATUS_SUCCESS; break; }
    ReleaseSRWLockExclusive(&g_notify_lock);
    return st;
}
NTSTATUS NTAPI PsSetCreateProcessNotifyRoutine(PVOID routine, BOOLEAN remove) {
    WARN(driver, "PsSetCreateProcessNotifyRoutine: accepted, but guest process events are not delivered to drivers");
    AcquireSRWLockExclusive(&g_notify_lock);
    if (!remove) Notifies().push_back({routine, kNotifyCreateProcess});
    ReleaseSRWLockExclusive(&g_notify_lock);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI PsSetCreateProcessNotifyRoutineEx(PVOID routine, BOOLEAN remove) { return PsSetCreateProcessNotifyRoutine(routine, remove); }
NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine(PVOID routine) {
    WARN(driver, "PsSetCreateThreadNotifyRoutine: accepted, but guest thread events are not delivered to drivers");
    AcquireSRWLockExclusive(&g_notify_lock);
    Notifies().push_back({routine, kNotifyCreateThread});
    ReleaseSRWLockExclusive(&g_notify_lock);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI PsRemoveCreateThreadNotifyRoutine(PVOID routine) {
    AcquireSRWLockExclusive(&g_notify_lock);
    NTSTATUS st = STATUS_PROCEDURE_NOT_FOUND;
    for (auto it = Notifies().begin(); it != Notifies().end(); ++it)
        if (it->routine == routine && it->kind == kNotifyCreateThread) { Notifies().erase(it); st = STATUS_SUCCESS; break; }
    ReleaseSRWLockExclusive(&g_notify_lock);
    return st;
}

// ---- named kernel events ----
namespace {
NTSTATUS CreateNamedEvent(PUNICODE_STRING name, EVENT_TYPE type, PHANDLE handle, PKEVENT* out) {
    std::wstring key = name ? NsCanonical(View(*name)) : std::wstring();
    PKEVENT ev = nullptr;
    if (!key.empty()) {
        AcquireSRWLockShared(&g_named_lock);
        for (auto& kv : NamedEvents()) if (kv.first == key) { ev = kv.second; break; }
        ReleaseSRWLockShared(&g_named_lock);
    }
    if (!ev) {
        ev = static_cast<PKEVENT>(PoolAlloc(sizeof(KEVENT), NTK_TAG('N', 'k', 'E', 'v')));
        if (!ev) return STATUS_INSUFFICIENT_RESOURCES;
        KeInitializeEvent(ev, type, TRUE);
        ObjRegister(ev, ObjType::Event, [](void* p) { PoolFree(p, 0, false); }, 1);
        if (!key.empty()) {
            AcquireSRWLockExclusive(&g_named_lock);
            NamedEvents().emplace_back(key, ev);
            ReleaseSRWLockExclusive(&g_named_lock);
        }
    } else {
        ObjRef(ev);
    }
    HANDLE h = dsp::RegisterEmulatedObject(dsp::ObjectKind::Other, reinterpret_cast<std::uint64_t>(ev), ev);
    if (!h) return STATUS_INSUFFICIENT_RESOURCES;
    if (handle) *handle = h;
    *out = ev;
    return STATUS_SUCCESS;
}
}  // namespace
PKEVENT NTAPI IoCreateNotificationEvent(PUNICODE_STRING name, PHANDLE handle) {
    PKEVENT ev = nullptr;
    return NT_SUCCESS(CreateNamedEvent(name, NotificationEvent, handle, &ev)) ? ev : nullptr;
}
PKEVENT NTAPI IoCreateSynchronizationEvent(PUNICODE_STRING name, PHANDLE handle) {
    PKEVENT ev = nullptr;
    return NT_SUCCESS(CreateNamedEvent(name, SynchronizationEvent, handle, &ev)) ? ev : nullptr;
}

// ---- Mm ----
BOOLEAN NTAPI MmIsAddressValid(PVOID va) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(va, &mbi, sizeof(mbi))) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return TRUE;
}
PVOID NTAPI MmGetSystemRoutineAddress(PUNICODE_STRING name) {
    if (!name || !name->Buffer) return nullptr;
    std::string a;
    for (size_t i = 0; i < name->Length / 2; ++i) a.push_back(static_cast<char>(name->Buffer[i]));
    if (void* p = FindExport(a.c_str())) return p;
    if (a.size() > 2 && (a.compare(0, 2, "Zw") == 0 || a.compare(0, 2, "Nt") == 0))
        if (void* p = ResolveNtFunction(("Nt" + a.substr(2)).c_str())) return p;
    TRACE(driver, "MmGetSystemRoutineAddress(%s): not exported", a.c_str());
    return nullptr;
}
PVOID NTAPI MmMapIoSpace(ULONGLONG phys, SIZE_T bytes, MEMORY_CACHING_TYPE cache) {
    (void)cache;
    FIXME(driver, "MmMapIoSpace(%#llx, %zu): the host has no physical address space; returning NULL", phys, static_cast<size_t>(bytes));
    return nullptr;
}
PVOID NTAPI MmMapIoSpaceEx(ULONGLONG phys, SIZE_T bytes, ULONG protect) {
    (void)protect;
    return MmMapIoSpace(phys, bytes, MmNonCached);
}
VOID NTAPI MmUnmapIoSpace(PVOID, SIZE_T) {}
PVOID NTAPI MmAllocateContiguousMemory(SIZE_T bytes, ULONGLONG highest) {
    (void)highest;
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
PVOID NTAPI MmAllocateContiguousMemorySpecifyCache(SIZE_T bytes, ULONGLONG low, ULONGLONG high, ULONGLONG boundary,
                                                   MEMORY_CACHING_TYPE cache) {
    (void)low; (void)high; (void)boundary; (void)cache;
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
VOID NTAPI MmFreeContiguousMemory(PVOID p) { VirtualFree(p, 0, MEM_RELEASE); }
VOID NTAPI MmFreeContiguousMemorySpecifyCache(PVOID p, SIZE_T, MEMORY_CACHING_TYPE) { VirtualFree(p, 0, MEM_RELEASE); }
PVOID NTAPI MmAllocateNonCachedMemory(SIZE_T bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
VOID NTAPI MmFreeNonCachedMemory(PVOID p, SIZE_T) { VirtualFree(p, 0, MEM_RELEASE); }
LARGE_INTEGER NTAPI MmGetPhysicalAddress(PVOID va) {
    TRACE(driver, "MmGetPhysicalAddress(%p): returning the virtual address (no physical memory model)", va);
    LARGE_INTEGER r;
    r.QuadPart = static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(va));
    return r;
}
int NTAPI MmQuerySystemSize(void) { return 1; /* MmMediumSystem */ }
BOOLEAN NTAPI MmIsDriverVerifying(PVOID) { return FALSE; }
PVOID NTAPI MmPageEntireDriver(PVOID addr) { return addr; }
VOID NTAPI MmResetDriverPaging(PVOID) {}
PVOID NTAPI MmLockPagableDataSection(PVOID addr) { return addr; }
VOID NTAPI MmUnlockPagableImageSection(PVOID) {}
HANDLE NTAPI MmSecureVirtualMemory(PVOID, SIZE_T, ULONG) { return reinterpret_cast<HANDLE>(1); }
VOID NTAPI MmUnsecureVirtualMemory(HANDLE) {}
NTSTATUS NTAPI MmCopyMemory(PVOID dst, ULONGLONG src, SIZE_T bytes, ULONG flags, PSIZE_T done) {
    if (flags & 1 /* MM_COPY_MEMORY_PHYSICAL */) {
        FIXME(driver, "MmCopyMemory(physical): no physical address space");
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!ProbeRange(reinterpret_cast<void*>(static_cast<ULONG_PTR>(src)), bytes, false)) return STATUS_ACCESS_VIOLATION;
    std::memcpy(dst, reinterpret_cast<void*>(static_cast<ULONG_PTR>(src)), bytes);
    if (done) *done = bytes;
    return STATUS_SUCCESS;
}
VOID NTAPI ProbeForRead(PVOID addr, SIZE_T len, ULONG align) {
    if ((reinterpret_cast<ULONG_PTR>(addr) & (align ? align - 1 : 0)) || (len && !ProbeRange(addr, len, false)))
        RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, nullptr);
}
VOID NTAPI ProbeForWrite(PVOID addr, SIZE_T len, ULONG align) {
    if ((reinterpret_cast<ULONG_PTR>(addr) & (align ? align - 1 : 0)) || (len && !ProbeRange(addr, len, true)))
        RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, nullptr);
}

// ---- Se / Cm / Etw (null sinks, see README) ----
BOOLEAN NTAPI SeSinglePrivilegeCheck(LONGLONG privilege, KPROCESSOR_MODE mode) { (void)privilege; (void)mode; return TRUE; }
NTSTATUS NTAPI CmRegisterCallback(PVOID fn, PVOID ctx, PLARGE_INTEGER cookie) {
    (void)fn; (void)ctx;
    WARN(driver, "CmRegisterCallback: accepted, registry notifications are not delivered to drivers");
    if (cookie) cookie->QuadPart = 1;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI CmRegisterCallbackEx(PVOID fn, PUNICODE_STRING altitude, PVOID driver_obj, PVOID ctx, PLARGE_INTEGER cookie, PVOID reserved) {
    (void)altitude; (void)driver_obj; (void)reserved;
    return CmRegisterCallback(fn, ctx, cookie);
}
NTSTATUS NTAPI CmUnRegisterCallback(LARGE_INTEGER) { return STATUS_SUCCESS; }
NTSTATUS NTAPI EtwRegister(const GUID* provider, PVOID callback, PVOID ctx, PULONGLONG handle) {
    (void)provider; (void)callback; (void)ctx;
    if (handle) *handle = 1;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI EtwUnregister(ULONGLONG) { return STATUS_SUCCESS; }
NTSTATUS NTAPI EtwWrite(ULONGLONG, PVOID, const GUID*, ULONG, PVOID) { return STATUS_SUCCESS; }
NTSTATUS NTAPI EtwWriteTransfer(ULONGLONG, PVOID, const GUID*, const GUID*, ULONG, PVOID) { return STATUS_SUCCESS; }
NTSTATUS NTAPI EtwWriteString(ULONGLONG, UCHAR, ULONGLONG, const GUID*, PCWSTR) { return STATUS_SUCCESS; }
BOOLEAN NTAPI EtwEventEnabled(ULONGLONG, PVOID) { return FALSE; }
BOOLEAN NTAPI EtwProviderEnabled(ULONGLONG, UCHAR, ULONGLONG) { return FALSE; }
NTSTATUS NTAPI EtwSetInformation(ULONGLONG, int, PVOID, ULONG) { return STATUS_SUCCESS; }
NTSTATUS NTAPI IoWMIRegistrationControl(PDEVICE_OBJECT, ULONG) { return STATUS_SUCCESS; }
NTSTATUS NTAPI WmiTraceMessage(ULONGLONG, ULONG, const GUID*, USHORT) { return STATUS_SUCCESS; }

}  // namespace ntk

namespace win10xbox::drivers::detail {

void ObPsMmInit() {
    for (int i = 0; i < 12; ++i) ntk::g_obj_type_ptrs[i] = &g_types[i];
    ntk::g_ps_initial_system_process = SystemPseudoProcess();
    (void)CurrentPseudoProcess();
}
void ObPsMmShutdown() {
    AcquireSRWLockExclusive(&g_notify_lock);
    Notifies().clear();
    ReleaseSRWLockExclusive(&g_notify_lock);
    AcquireSRWLockExclusive(&g_named_lock);
    NamedEvents().clear();
    ReleaseSRWLockExclusive(&g_named_lock);
    g_proc_init = false;
}

namespace {
const Export kObPsMmTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
    X(ObReferenceObjectByHandle), X(ObReferenceObjectByHandleWithTag), X(ObReferenceObjectByPointer),
    X(ObfReferenceObject), X(ObfDereferenceObject), X(ObfReferenceObjectWithTag), X(ObfDereferenceObjectWithTag),
    X(ObDereferenceObjectDeferDelete), X(ObReferenceObjectByName), X(ObCloseHandle), X(ObMakeTemporaryObject),
    X(ObRegisterCallbacks), X(ObUnRegisterCallbacks), X(ObGetFilterVersion),
    X(PsGetCurrentProcessId), X(PsGetCurrentThreadId), X(IoGetCurrentProcess), X(PsGetCurrentProcess),
    X(PsGetCurrentThread), X(PsGetProcessId), X(PsGetThreadId), X(PsGetThreadProcessId), X(PsGetThreadProcess),
    X(PsGetProcessImageFileName), X(PsGetProcessPeb), X(PsGetProcessSessionId), X(PsGetCurrentProcessSessionId),
    X(PsIsSystemThread), X(PsLookupProcessByProcessId), X(PsLookupThreadByThreadId), X(PsCreateSystemThread),
    X(PsTerminateSystemThread), X(PsGetVersion), X(PsSetLoadImageNotifyRoutine), X(PsRemoveLoadImageNotifyRoutine),
    X(PsSetCreateProcessNotifyRoutine), X(PsSetCreateProcessNotifyRoutineEx), X(PsSetCreateThreadNotifyRoutine),
    X(PsRemoveCreateThreadNotifyRoutine),
    X(IoCreateNotificationEvent), X(IoCreateSynchronizationEvent),
    X(MmIsAddressValid), X(MmGetSystemRoutineAddress), X(MmMapIoSpace), X(MmMapIoSpaceEx), X(MmUnmapIoSpace),
    X(MmAllocateContiguousMemory), X(MmAllocateContiguousMemorySpecifyCache), X(MmFreeContiguousMemory),
    X(MmFreeContiguousMemorySpecifyCache), X(MmAllocateNonCachedMemory), X(MmFreeNonCachedMemory),
    X(MmGetPhysicalAddress), X(MmQuerySystemSize), X(MmIsDriverVerifying), X(MmPageEntireDriver),
    X(MmResetDriverPaging), X(MmLockPagableDataSection), X(MmUnlockPagableImageSection), X(MmSecureVirtualMemory),
    X(MmUnsecureVirtualMemory), X(MmCopyMemory), X(ProbeForRead), X(ProbeForWrite),
    X(SeSinglePrivilegeCheck), X(CmRegisterCallback), X(CmRegisterCallbackEx), X(CmUnRegisterCallback),
    X(EtwRegister), X(EtwUnregister), X(EtwWrite), X(EtwWriteTransfer), X(EtwWriteString), X(EtwEventEnabled),
    X(EtwProviderEnabled), X(EtwSetInformation), X(IoWMIRegistrationControl), X(WmiTraceMessage),
#undef X
    {"ObfReferenceObject_alias_unused", nullptr},
};
}  // namespace

// Data exports are listed separately: their "address" is the variable.
const Export kDataExports[] = {
    {"IoFileObjectType", &ntk::g_obj_type_ptrs[0]},
    {"IoDeviceObjectType", &ntk::g_obj_type_ptrs[1]},
    {"IoDriverObjectType", &ntk::g_obj_type_ptrs[2]},
    {"PsProcessType", &ntk::g_obj_type_ptrs[3]},
    {"PsThreadType", &ntk::g_obj_type_ptrs[4]},
    {"ExEventObjectType", &ntk::g_obj_type_ptrs[5]},
    {"ExSemaphoreObjectType", &ntk::g_obj_type_ptrs[6]},
    {"SeTokenObjectType", &ntk::g_obj_type_ptrs[7]},
    {"CmKeyObjectType", &ntk::g_obj_type_ptrs[8]},
    {"PsInitialSystemProcess", &ntk::g_ps_initial_system_process},
    {"NtBuildNumber", &ntk::g_nt_build_number},
    {"NtGlobalFlag", &ntk::g_nt_global_flag},
    {"InitSafeBootMode", &ntk::g_init_safe_boot_mode},
    {"MmUserProbeAddress", &ntk::g_mm_user_probe_address},
    {"MmSystemRangeStart", &ntk::g_mm_system_range_start},
    {"MmHighestUserAddress", &ntk::g_mm_highest_user_address},
    {"NlsMbCodePageTag", &ntk::g_nls_mb_code_page_tag},
    {"PsLoadedModuleList", &g_ps_loaded_module_list},
};

ExportGroup ObPsMmExports() { return {kObPsMmTable, sizeof(kObPsMmTable) / sizeof(kObPsMmTable[0]) - 1}; }
ExportGroup DataExports() { return {kDataExports, sizeof(kDataExports) / sizeof(kDataExports[0])}; }

}  // namespace win10xbox::drivers::detail
