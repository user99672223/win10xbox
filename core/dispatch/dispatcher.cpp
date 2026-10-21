// win10xbox dispatcher core: syscall table, policies, relay tracing, the
// generic host caller, emulated handles and the per-thread TEB view switch.
// See docs/DISPATCHER.md.
#include "internal.h"
#include "win10xbox/config.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

WIN10XBOX_DECLARE_CHANNEL(timing);
WIN10XBOX_DECLARE_CHANNEL(wine);
WIN10XBOX_DECLARE_CHANNEL(process);

extern "C" {
// TEB offset of our TLS slot (0 = view switching disabled). Read by core/asm.
std::uint64_t win10xbox_thread_slot_offset = 0;
void win10xbox_syscall_entry();
}

namespace win10xbox::dispatch {
namespace detail {
namespace {

struct PolicyDef { const char* name; Policy policy; const char* note; };
const PolicyDef kPolicies[] = {
#define POLICY(name, pol, note) {#name, Policy::pol, note},
#include "syscall_policy.inc"
#undef POLICY
};

struct TableDef { std::uint32_t id; const char* name; std::uint32_t nargs; };
const TableDef kTable[] = {
#define SYSCALL(id, name, nargs) {id, #name, nargs},
#include "syscall_table.gen.inc"
#undef SYSCALL
};

// Calls that do not return to the dispatcher on success; relay logs them first.
const char* const kNoReturn[] = {"NtContinue", "NtContinueEx", "NtRaiseException", "NtTerminateProcess",
                                 "NtTerminateThread"};

constexpr std::uint32_t kMaxId = 0x2000;   // ntdll 0x0000.., win32u 0x1000..

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    bool initialized = false;
    std::vector<Entry> entries;
    std::vector<std::uint8_t> noreturn;    // parallel to entries
    Entry* by_id[kMaxId] = {};
    HMODULE host_ntdll = nullptr;
    HostNt host{};
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    LARGE_INTEGER qpf{};
};
State& S() { static State s; return s; }

SRWLOCK g_export_lock = SRWLOCK_INIT;
std::unordered_map<std::string, void*>& ExportCache() { static std::unordered_map<std::string, void*> m; return m; }

SRWLOCK g_obj_lock = SRWLOCK_INIT;
std::map<std::uintptr_t, EmulatedObject>& Objects() { static std::map<std::uintptr_t, EmulatedObject> m; return m; }

SRWLOCK g_threads_lock = SRWLOCK_INIT;
std::vector<ThreadState*>& Threads() { static std::vector<ThreadState*> v; return v; }

bool IEquals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

void* FindImpl(const char* name) {
    for (auto list : {FileImpls(), SectionImpls(), ProcessImpls(), SubsystemImpls()})
        for (const ImplDef* d = list; d && d->name; ++d)
            if (!std::strcmp(d->name, name)) return d->fn;
    return nullptr;
}

void ResolveHost(HostNt& h) {
#define R(field) h.field = reinterpret_cast<decltype(h.field)>(HostExport(#field))
    R(NtClose); R(NtCreateFile); R(NtOpenFile); R(NtQueryAttributesFile); R(NtQueryFullAttributesFile);
    R(NtDeleteFile); R(NtQueryInformationFile); R(NtOpenDirectoryObject); R(NtOpenSymbolicLinkObject);
    R(NtCreateSection); R(NtCreateSectionEx); R(NtMapViewOfSection); R(NtMapViewOfSectionEx);
    R(NtUnmapViewOfSection); R(NtUnmapViewOfSectionEx); R(NtQuerySection); R(NtAreMappedFilesTheSame);
    R(NtDuplicateObject); R(NtQueryVirtualMemory); R(NtQueryInformationProcess); R(NtQueryInformationThread);
    R(NtQuerySystemInformation); R(NtQuerySecurityObject); R(NtTerminateProcess); R(NtTerminateThread);
    R(NtContinue); R(NtContinueEx); R(NtRaiseException); R(NtSetContextThread); R(NtCreateThreadEx);
    R(NtQuerySystemTime); R(RtlCreateProcessParametersEx);
#undef R
}

void SwapIn(ThreadState* st) {
    st->guest_tls = __readgsqword(teb::ThreadLocalStoragePointer);
    __writegsqword(teb::ThreadLocalStoragePointer, st->host_tls);
    st->guest_fls = __readgsqword(teb::FlsData);
    __writegsqword(teb::FlsData, st->host_fls);
}
void SwapOut(ThreadState* st) {
    st->host_tls = __readgsqword(teb::ThreadLocalStoragePointer);
    __writegsqword(teb::ThreadLocalStoragePointer, st->guest_tls);
    st->host_fls = __readgsqword(teb::FlsData);
    __writegsqword(teb::FlsData, st->guest_fls);
}

void AppendArg(std::string& out, std::uint64_t v) {
    char buf[24];
    if (v <= 0xffffffffull) std::snprintf(buf, sizeof buf, "%08llx", static_cast<unsigned long long>(v));
    else std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    out += buf;
}

}  // namespace

const HostNt& Host() { return S().host; }

std::uint32_t NargsOverride(const char* name) {
    static const struct { const char* name; std::uint32_t nargs; } kFix[] = {
        {"NtMapUserPhysicalPagesScatter", 3}, {"NtWaitForMultipleObjects32", 5}, {"NtApphelpCacheControl", 2},
        {"NtCreateProcessEx", 9}, {"NtTraceEvent", 4},
    };
    for (auto& f : kFix)
        if (!std::strcmp(f.name, name)) return f.nargs;
    return 0;
}

// ---- Thread state ----------------------------------------------------------

ThreadState* CurrentThreadState() {
    std::uint64_t off = win10xbox_thread_slot_offset;
    return off ? reinterpret_cast<ThreadState*>(__readgsqword(static_cast<unsigned long>(off))) : nullptr;
}

ThreadState* AttachThreadState(std::uint32_t flags) {
    if (ThreadState* st = CurrentThreadState()) { st->flags |= flags; return st; }
    if (S().tls_index == TLS_OUT_OF_INDEXES) return nullptr;
    auto* st = new ThreadState{};
    st->host_tls = st->guest_tls = __readgsqword(teb::ThreadLocalStoragePointer);
    st->host_fls = st->guest_fls = __readgsqword(teb::FlsData);
    st->flags = flags;
    st->tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &st->thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    TlsSetValue(S().tls_index, st);
    AcquireSRWLockExclusive(&g_threads_lock);
    Threads().push_back(st);
    ReleaseSRWLockExclusive(&g_threads_lock);
    return st;
}

void DetachThreadState() {
    ThreadState* st = CurrentThreadState();
    if (!st) return;
    AcquireSRWLockExclusive(&g_threads_lock);
    auto& v = Threads();
    v.erase(std::remove(v.begin(), v.end(), st), v.end());
    ReleaseSRWLockExclusive(&g_threads_lock);
    TlsSetValue(S().tls_index, nullptr);
    if (st->thread) CloseHandle(st->thread);
    delete st;
}

void ForgetGuestThread(DWORD tid) {
    AcquireSRWLockExclusive(&g_threads_lock);
    auto& v = Threads();
    // The thread may still be running briefly; its ThreadState is leaked, not freed.
    v.erase(std::remove_if(v.begin(), v.end(), [tid](ThreadState* t) { return t->tid == tid; }), v.end());
    ReleaseSRWLockExclusive(&g_threads_lock);
}

std::vector<ThreadState*> GuestThreads() {
    AcquireSRWLockShared(&g_threads_lock);
    std::vector<ThreadState*> out;
    for (auto* t : Threads())
        if (t->flags & kThreadGuest) out.push_back(t);
    ReleaseSRWLockShared(&g_threads_lock);
    return out;
}

std::uint32_t LeaveToGuestView() {
    ThreadState* st = CurrentThreadState();
    if (!st || !st->depth) return 0;
    std::uint32_t d = st->depth;
    SwapOut(st);
    st->depth = 0;
    return d;
}

void ReenterHostView(std::uint32_t depth) {
    ThreadState* st = CurrentThreadState();
    if (!st || !depth) return;
    SwapIn(st);
    st->depth = depth;
}

}  // namespace detail

using namespace detail;

HostModeScope::HostModeScope() : state_(CurrentThreadState()) {
    auto* st = static_cast<ThreadState*>(state_);
    if (st && ++st->depth == 1) SwapIn(st);
}
HostModeScope::~HostModeScope() {
    auto* st = static_cast<ThreadState*>(state_);
    if (st && --st->depth == 0) SwapOut(st);
}

// ---- Host ntdll ---------------------------------------------------------------

HMODULE HostNtdll() {
    if (!S().host_ntdll) S().host_ntdll = GetModuleHandleW(L"ntdll.dll");
    return S().host_ntdll;
}

void* HostExport(const char* name) {
    AcquireSRWLockShared(&g_export_lock);
    auto it = ExportCache().find(name);
    void* p = it != ExportCache().end() ? it->second : nullptr;
    bool found = it != ExportCache().end();
    ReleaseSRWLockShared(&g_export_lock);
    if (found) return p;
    p = reinterpret_cast<void*>(GetProcAddress(HostNtdll(), name));
    AcquireSRWLockExclusive(&g_export_lock);
    ExportCache()[name] = p;
    ReleaseSRWLockExclusive(&g_export_lock);
    return p;
}

// ---- Table ----------------------------------------------------------------------

NTSTATUS Initialize() {
    State& s = S();
    AcquireSRWLockExclusive(&s.lock);
    if (s.initialized) { ReleaseSRWLockExclusive(&s.lock); return st::Success; }
    if (!HostNtdll()) { ReleaseSRWLockExclusive(&s.lock); return st::DllNotFound; }
    ResolveHost(s.host);
    QueryPerformanceFrequency(&s.qpf);

    s.entries.clear();
    s.entries.reserve(std::size(kTable));
    std::size_t counts[4] = {};
    for (const TableDef& t : kTable) {
        Entry e{};
        e.id = t.id;
        e.name = t.name;
        e.nargs = t.nargs ? t.nargs : NargsOverride(t.name);
        if (e.nargs > kMaxArgs) e.nargs = kMaxArgs;
        e.policy = Policy::Todo;
        e.note = "";
        for (const PolicyDef& p : kPolicies)
            if (!std::strcmp(p.name, t.name)) { e.policy = p.policy; e.note = p.note; break; }
        e.host_fn = HostExport(t.name);
        e.impl = FindImpl(t.name);
        if ((e.policy == Policy::Translate || e.policy == Policy::Emulate) && !e.impl) {
            if (e.host_fn && !NeverPassThrough(t.name)) {
                WARN(dispatch, "%s: %s implementation not built, passing through to the host", t.name,
                     PolicyName(e.policy));
                e.policy = Policy::PassThrough;
            } else {
                WARN(dispatch, "%s: %s implementation not built, Todo", t.name, PolicyName(e.policy));
                e.policy = Policy::Todo;
            }
        }
        if (e.policy == Policy::PassThrough && !e.host_fn)
            WARN(dispatch, "%s: host ntdll has no export, calls will fail", t.name);
        ++counts[static_cast<int>(e.policy)];
        s.entries.push_back(e);
    }
    s.noreturn.assign(s.entries.size(), 0);
    std::fill(std::begin(s.by_id), std::end(s.by_id), nullptr);
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        Entry& e = s.entries[i];
        if (e.id < kMaxId) s.by_id[e.id] = &e;
        for (const char* n : kNoReturn)
            if (!std::strcmp(n, e.name)) s.noreturn[i] = 1;
    }

    // TLS slot for ThreadState; core/asm reads it through the TEB directly,
    // which needs a slot in TEB.TlsSlots[0..63].
    s.tls_index = TlsAlloc();
    if (s.tls_index != TLS_OUT_OF_INDEXES && s.tls_index < 64)
        win10xbox_thread_slot_offset = teb::TlsSlots + 8ull * s.tls_index;
    else
        ERR(dispatch, "TLS slot %lu unusable: guest/host TEB view switching disabled", s.tls_index);

    s.initialized = true;
    ReleaseSRWLockExclusive(&s.lock);
    TRACE(dispatch, "%zu syscalls: %zu pass-through, %zu translate, %zu emulate, %zu todo", s.entries.size(),
          counts[0], counts[1], counts[2], counts[3]);
    return st::Success;
}

void Shutdown() {
    State& s = S();
    if (!s.initialized || !trace::Enabled(&__win10xbox_dbch_relay, trace::Class::Trace)) return;
    std::vector<const Entry*> used;
    for (const Entry& e : s.entries)
        if (e.calls) used.push_back(&e);
    std::sort(used.begin(), used.end(), [](const Entry* a, const Entry* b) { return a->calls > b->calls; });
    std::uint64_t total = 0;
    for (auto* e : used) total += e->calls;
    TRACE(relay, "statistics: %llu calls to %zu syscalls", static_cast<unsigned long long>(total), used.size());
    for (auto* e : used)
        TRACE(relay, "  %8llu %s (%s)", static_cast<unsigned long long>(e->calls), e->name, PolicyName(e->policy));
}

const Entry* Lookup(std::uint32_t id) { return id < kMaxId ? S().by_id[id] : nullptr; }

const Entry* LookupByName(const char* name) {
    for (const Entry& e : S().entries)
        if (!std::strcmp(e.name, name)) return &e;
    return nullptr;
}

std::size_t Count() { return S().entries.size(); }

const char* PolicyName(Policy p) {
    switch (p) {
    case Policy::PassThrough: return "PassThrough";
    case Policy::Translate: return "Translate";
    case Policy::Emulate: return "Emulate";
    case Policy::Todo: return "Todo";
    }
    return "?";
}

bool ParsePolicy(const char* text, Policy* out) {
    if (!text) return false;
    static const struct { const char* name; Policy p; } kNames[] = {
        {"passthrough", Policy::PassThrough}, {"pass-through", Policy::PassThrough}, {"pass", Policy::PassThrough},
        {"translate", Policy::Translate}, {"emulate", Policy::Emulate}, {"todo", Policy::Todo},
    };
    for (auto& n : kNames)
        if (IEquals(text, n.name)) { *out = n.p; return true; }
    return false;
}

NTSTATUS SetPolicy(const char* name, Policy p) {
    State& s = S();
    if (!s.initialized) return st::Unsuccessful;
    for (Entry& e : s.entries) {
        if (std::strcmp(e.name, name)) continue;
        if ((p == Policy::Translate || p == Policy::Emulate) && !e.impl) return st::NotImplemented;
        if (p == Policy::PassThrough && !e.host_fn) return st::NotSupported;
        TRACE(dispatch, "%s: %s -> %s", e.name, PolicyName(e.policy), PolicyName(p));
        e.policy = p;
        return st::Success;
    }
    return st::ObjectNameNotFound;
}

void* SyscallEntryAddress() { return reinterpret_cast<void*>(&win10xbox_syscall_entry); }

NTSTATUS CallWithArgs(void* fn, const std::uint64_t* a, std::uint32_t nargs) {
    using U = std::uint64_t;
    using Fn = NTSTATUS(NTAPI*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
    static_assert(kMaxArgs == 20, "CallWithArgs passes exactly kMaxArgs arguments");
    U v[kMaxArgs] = {};
    for (std::uint32_t i = 0; i < nargs && i < kMaxArgs; ++i) v[i] = a[i];
    return reinterpret_cast<Fn>(fn)(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                                    v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19]);
}

std::string FormatRelay(const Entry& e, const std::uint64_t* args, NTSTATUS status, double elapsed_us) {
    std::string out = e.name;
    out += '(';
    for (std::uint32_t i = 0; i < e.nargs; ++i) {
        if (i) out += ',';
        AppendArg(out, args[i]);
    }
    char tail[64];
    std::snprintf(tail, sizeof tail, ") ret=%08lx", static_cast<unsigned long>(status));
    out += tail;
    if (elapsed_us >= 0) {
        std::snprintf(tail, sizeof tail, " (%.1fus)", elapsed_us);
        out += tail;
    }
    return out;
}

extern "C" std::uint32_t win10xbox_syscall_nargs(std::uint32_t id) {
    const Entry* e = id < kMaxId ? S().by_id[id] : nullptr;
    return e ? e->nargs : 0;
}

extern "C" std::int64_t win10xbox_dispatch(Frame* f) {
    State& s = S();
    Entry* e = f->id < kMaxId ? s.by_id[f->id] : nullptr;
    if (!e) {
        FIXME(dispatch, "unknown syscall id %#x (ret=%p)", f->id, reinterpret_cast<void*>(f->return_address));
        return st::NotImplemented;
    }
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&e->calls));
    const bool relay = trace::Enabled(&__win10xbox_dbch_relay, trace::Class::Trace);
    const bool timing = relay && trace::Enabled(&__win10xbox_dbch_timing, trace::Class::Trace);
    if (relay && s.noreturn[static_cast<std::size_t>(e - s.entries.data())]) {
        std::string line = FormatRelay(*e, f->args, 0);
        line.resize(line.rfind(" ret="));
        trace::Log(&__win10xbox_dbch_relay, trace::Class::Trace, "", "%s (may not return)", line.c_str());
    }
    LARGE_INTEGER t0{}, t1{};
    if (timing) QueryPerformanceCounter(&t0);

    NTSTATUS status;
    switch (e->policy) {
    case Policy::PassThrough:
        status = e->host_fn ? CallWithArgs(e->host_fn, f->args, e->nargs) : st::NotImplemented;
        if (!e->host_fn) FIXME(dispatch, "%s: no host export", e->name);
        break;
    case Policy::Translate:
    case Policy::Emulate:
        if (e->impl) status = CallWithArgs(e->impl, f->args, e->nargs);
        else if (e->host_fn) status = CallWithArgs(e->host_fn, f->args, e->nargs);
        else status = st::NotImplemented;
        break;
    case Policy::Todo:
    default:
        FIXME(dispatch, "unimplemented syscall %s", e->name);
        status = st::NotImplemented;
        break;
    }

    if (relay) {
        double us = -1.0;
        if (timing) {
            QueryPerformanceCounter(&t1);
            us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(s.qpf.QuadPart ? s.qpf.QuadPart : 1);
        }
        std::string line = FormatRelay(*e, f->args, status, us);
        trace::Log(&__win10xbox_dbch_relay, trace::Class::Trace, "", "%s", line.c_str());
    }
    return status;
}

// ---- Emulated objects ------------------------------------------------------------

HANDLE RegisterEmulatedObject(ObjectKind kind, std::uint64_t id, void* extra) {
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!h) return nullptr;
    AcquireSRWLockExclusive(&g_obj_lock);
    Objects()[reinterpret_cast<std::uintptr_t>(h)] = EmulatedObject{kind, id, extra};
    ReleaseSRWLockExclusive(&g_obj_lock);
    return h;
}

bool LookupEmulatedObject(HANDLE h, EmulatedObject* out) {
    AcquireSRWLockShared(&g_obj_lock);
    auto it = Objects().find(reinterpret_cast<std::uintptr_t>(h));
    bool found = it != Objects().end();
    if (found && out) *out = it->second;
    ReleaseSRWLockShared(&g_obj_lock);
    return found;
}

void UnregisterEmulatedObject(HANDLE h) {
    AcquireSRWLockExclusive(&g_obj_lock);
    std::size_t n = Objects().erase(reinterpret_cast<std::uintptr_t>(h));
    ReleaseSRWLockExclusive(&g_obj_lock);
    if (n) CloseHandle(h);
}

}  // namespace win10xbox::dispatch
