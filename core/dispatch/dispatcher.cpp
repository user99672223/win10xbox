// win10xbox dispatcher core: syscall table, policies, relay tracing, the
// generic host caller, emulated handles and the per-thread TEB view switch.
// See docs/DISPATCHER.md.
#include "internal.h"
#include "nt_spawn.h"
#include "win10xbox/config.h"
#include "win10xbox/probe.h"
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
// Incremented (lock inc) by core/asm every time TEB.TlsSlots[tls_index] held a
// value that is not one of our ThreadStates. Never expected to be non-zero;
// win10xbox_dispatch reports the first one on the dispatch channel.
std::uint64_t win10xbox_thread_slot_clobbered = 0;
// Times host state was reached while the guest TLS view was installed. Any
// non-zero value is a bug of the class that crashed Xl_NtContinue on MSVC; see
// "The guest-view window" in core/dispatch/internal.h.
std::uint64_t win10xbox_guest_view_violations = 0;
void win10xbox_syscall_entry();
}

namespace win10xbox::dispatch {
namespace detail {
const ImplDef* NlsImpls();   // nt_nls.cpp (registered like the lists in internal.h)

// What last reached host state inside the guest-view window (a literal). At
// namespace scope and trivially initialised: it is written from inside the
// window, where nothing may be lazily initialised.
const char* volatile g_guest_view_culprit = nullptr;

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
#define WIN32U_SYSCALL(id, name, nargs) {id, #name, nargs},   // ids 0x1000+, same entry stub
#include "syscall_table.gen.inc"
#undef WIN32U_SYSCALL
#undef SYSCALL
};

// Calls that do not return to the dispatcher on success; relay logs them first.
const char* const kNoReturn[] = {"NtContinue", "NtContinueEx", "NtRaiseException", "NtTerminateProcess",
                                 "NtTerminateThread"};

constexpr std::uint32_t kMaxId = 0x2000;   // ntdll 0x0000.., win32u 0x1000..
constexpr std::uint32_t kWin32uFirstId = 0x1000;
// Route B (gui native): win32u syscalls the native win32u.dll exports but Wine's table does not list get an
// entry of their own at map time, numbered from here (Wine's win32u ids end at 0x1603). kUnknownSyscallId is
// what a thunk whose name we cannot dispatch at all is renumbered to: no entry ever has it, so it lands in
// "unknown syscall" instead of in whatever entry its foreign id happens to collide with.
constexpr std::uint32_t kExtraFirstId = 0x1800;
constexpr std::uint32_t kUnknownSyscallId = 0x1FFF;

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    bool initialized = false;
    std::vector<Entry> entries;
    std::vector<std::uint8_t> noreturn;    // parallel to entries
    Entry* by_id[kMaxId] = {};
    HMODULE host_ntdll = nullptr;
    HostNt host{};
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    void* host_tls_bitmap = nullptr;   // PEB.TlsBitmap before the takeover (host ntdll's)
    LONG guest_tls_reserved = 0;       // ReserveGuestTlsSlot has run
    bool test_clobber_slot = false;    // WIN10XBOX_TEST_TLS_CLOBBER, latched in Initialize
    LARGE_INTEGER qpf{};
    std::unordered_map<std::string, Entry*> by_name;   // every entry, for RenumberSyscallThunks
    // Route B (config gui = native): win32u entries pass through to this module (the HOST's win32u.dll).
    HMODULE host_win32u = nullptr;
    bool gui_native = false;
    std::vector<std::unique_ptr<Entry>> extra;         // native-only win32u syscalls (ids kExtraFirstId..)
    std::uint32_t next_extra_id = kExtraFirstId;
    const Entry* end_paint = nullptr;                  // NtUserEndPaint: presentation snapshot point
};
// NAMESPACE SCOPE ON PURPOSE. As a function-local `static` this emitted an
// MSVC thread-safe-init guard, and the guard reads _Init_thread_epoch through
// TEB.ThreadLocalStoragePointer -- so Host(), called from Xl_NtContinue after
// LeaveToGuestView(), faulted on the guest's TLS array. At namespace scope the
// access is a plain address with no guard and no TLS of any kind, which is the
// whole fix; it does not depend on how the object is initialised.
//
// NOT `constinit`: MSVC's Debug STL makes std::vector's default constructor
// non-constexpr (_ITERATOR_DEBUG_LEVEL), so `constinit State g_state;` is
// rejected with C2127 in the Debug configuration while compiling fine in
// RelWithDebInfo -- ci.yml run 39786498185 onwards went red on exactly that.
// Dropping the keyword costs only the compile-time rejection of a future member
// that would reintroduce dynamic initialisation; the durable protection against
// this bug class is NoteHostStateUnderGuestView() plus the gating test
// integration.guest_view_clean, which is portable and actually fails when the
// hazard returns. Nothing here may be touched during static initialisation:
// dispatch::Initialize() is called explicitly by the host, long after it.
State g_state;
// Instrumented even though it is now guard-free: reaching ANY host state inside
// the guest-view window is the bug, and this accessor is the one the crash came
// through (Host() -> S()), so it is the most valuable place to notice a
// regression. TestViolateGuestView must not use it -- see g_test_violate_view.
State& S() {
    NoteHostStateUnderGuestView("dispatch::S (Host, Lookup, ...)");
    return g_state;
}

// Namespace scope and trivially initialised: read from inside the window, so it
// must not go through S() (which would report itself) or any lazy init.
bool g_test_violate_view = false;

// The remaining singletons keep their function-local `static` (their types
// cannot be constant-initialised) and so keep the MSVC init guard. None is
// reachable from the guest-view window today; the check is what tells us if
// that ever changes, on Wine as well as MSVC, instead of a null+4 fault only a
// Windows host can produce.
SRWLOCK g_export_lock = SRWLOCK_INIT;
std::unordered_map<std::string, void*>& ExportCache() {
    NoteHostStateUnderGuestView("dispatch::ExportCache");
    static std::unordered_map<std::string, void*> m;
    return m;
}

SRWLOCK g_obj_lock = SRWLOCK_INIT;
std::map<std::uintptr_t, EmulatedObject>& Objects() {
    NoteHostStateUnderGuestView("dispatch::Objects");
    static std::map<std::uintptr_t, EmulatedObject> m;
    return m;
}

SRWLOCK g_threads_lock = SRWLOCK_INIT;
std::vector<ThreadState*>& Threads() {
    NoteHostStateUnderGuestView("dispatch::Threads");
    static std::vector<ThreadState*> v;
    return v;
}

bool IEquals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

void* FindImpl(const char* name) {
    for (auto list : {FileImpls(), SectionImpls(), ProcessImpls(), NlsImpls(), SubsystemImpls(), UserImpls(),
                      PrivilegedImpls(), ObjectImpls()})
        for (const ImplDef* d = list; d && d->name; ++d)
            if (!std::strcmp(d->name, name)) return d->fn;
    return nullptr;
}

void ResolveHost(HostNt& h) {
#define R(field) h.field = reinterpret_cast<decltype(h.field)>(HostExport(#field))
    R(NtClose); R(NtCreateFile); R(NtOpenFile); R(NtCreateNamedPipeFile); R(NtQueryAttributesFile); R(NtQueryFullAttributesFile);
    R(NtDeleteFile); R(NtQueryInformationFile); R(NtSetInformationFile); R(NtOpenDirectoryObject); R(NtOpenSymbolicLinkObject);
    R(NtClose); R(NtCreateFile); R(NtOpenFile); R(NtQueryAttributesFile); R(NtQueryFullAttributesFile);
    R(NtDeleteFile); R(NtQueryInformationFile); R(NtSetInformationFile); R(NtQueryObject); R(RtlCreateHeap); R(NtOpenDirectoryObject); R(NtOpenSymbolicLinkObject);
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

// TEB.TlsSlots index for our ThreadState. Both allocators hand out the lowest
// clear bit in PEB.TlsBitmap, and Wine's loader_init additionally claims a few
// indices by fiat (SlotUsable in internal.h), so take indices until we reach one
// at or above kPreferredSlot, keep the highest usable one and give the rest
// back. Everything we hold is released before we return, so this costs the
// process nothing but cannot be handed the same unusable index twice.
DWORD AllocThreadSlot() {
    DWORD taken[teb::TlsSlotCount];
    unsigned n = 0;
    while (n < teb::TlsSlotCount) {
        DWORD i = TlsAlloc();
        if (i == TLS_OUT_OF_INDEXES) break;
        if (i >= teb::TlsSlotCount) { TlsFree(i); break; }   // no asm fast path above 63
        taken[n++] = i;
        if (i >= kPreferredSlot) break;
    }
    DWORD keep = TLS_OUT_OF_INDEXES;
    for (unsigned k = 0; k < n; ++k)
        if (SlotUsable(taken[k]) && (keep == TLS_OUT_OF_INDEXES || taken[k] > keep)) keep = taken[k];
    for (unsigned k = 0; k < n; ++k)
        if (taken[k] != keep) TlsFree(taken[k]);
    return keep;
}

// WIN10XBOX_TEST_TLS_INDEX=<n> makes Initialize use TEB.TlsSlots[n] instead of
// the one TlsAlloc hands out, so a Wine/desktop-Windows run can exercise an
// index that only a real-Windows host would ever be given. The collision this
// guards against is impossible to reach by luck under Wine: there the host ntdll
// IS Wine's, so its loader_init reserved index NTDLL_TLS_ERRNO (16) long before
// our TlsAlloc ran and TlsAlloc can never return it. Same shape as
// WIN10XBOX_TEST_NO_HOST_PATHS in nt_file.cpp. Test hook.
//   WIN10XBOX_TEST_TLS_INDEX=16   request 16; the SlotUsable guard relocates it
//   WIN10XBOX_TEST_TLS_INDEX=!16  force 16 past the guard (reproduces the bug)
bool TestSlotOverride(DWORD* index, bool* force) {
    wchar_t v[16];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEST_TLS_INDEX", v, 16);
    if (!n || n >= 16) return false;
    const wchar_t* p = v;
    *force = (*p == L'!');
    if (*force) ++p;
    DWORD parsed = 0;
    if (!*p) return false;
    for (; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        parsed = parsed * 10 + static_cast<DWORD>(*p - L'0');
    }
    if (parsed >= teb::TlsSlotCount) return false;   // never breaks the asm fast path
    *index = parsed;
    return true;
}

// ---- The tagged TEB slot ----------------------------------------------------
// What we store in TEB.TlsSlots[tls_index] is not the bare ThreadState pointer
// but the pointer with a tag in bits 48..63, which are always zero in an x64
// user-mode address. The tag is a function of bits 16..30 OF THE POINTER
// ITSELF, so it is redundant information rather than a constant cookie, and
// that is the point: Wine's _errno() hands out &TEB.TlsSlots[16] as the storage
// for an `int` (wine-src/dlls/ntdll/thread.c:445), so a collision clobbers only
// the LOW 32 BITS of the slot. The surviving value is neither NULL nor a clean
// sentinel, which is exactly why the plain `test rbx,rbx` in core/asm never
// caught it. A 4-byte write to the low dword changes bits 16..30 while leaving
// the tag alone, so the two stop agreeing and we detect it ARITHMETICALLY,
// before dereferencing anything. A full 8-byte foreign pointer leaves bits
// 48..63 clear, while a valid tag always has bit 15 set, so that is caught too.
constexpr std::uint64_t kSlotTagShift = 48;
constexpr std::uint64_t kSlotPtrMask = (1ull << kSlotTagShift) - 1;

// The tag bits 48..63 must hold for the slot to be ours. Bit 15 is forced so a
// zero tag field can never validate.
constexpr std::uint64_t SlotTagOf(std::uint64_t v) { return 0x8000ull | ((v >> 16) & 0x7FFFull); }

std::uint64_t TagSlot(const ThreadState* st) {
    const auto p = reinterpret_cast<std::uint64_t>(st);
    return p | (SlotTagOf(p) << kSlotTagShift);
}

// Mirrored instruction for instruction by the view-switch block in BOTH
// core/asm twins, except for the ThreadState::magic check, which only this side
// makes (the asm validates the pointer; the cookie additionally rejects a stale
// pointer to a ThreadState that has already been detached).
ThreadState* ValidateSlot(std::uint64_t raw) {
    if (!raw) return nullptr;
    if ((raw >> kSlotTagShift) != SlotTagOf(raw)) return nullptr;
    const std::uint64_t p = raw & kSlotPtrMask;
    if (!p) return nullptr;
    auto* st = reinterpret_cast<ThreadState*>(p);
    if (st->magic != kThreadStateMagic) return nullptr;
    return st;
}

void AppendArg(std::string& out, std::uint64_t v) {
    char buf[24];
    if (v <= 0xffffffffull) std::snprintf(buf, sizeof buf, "%08llx", static_cast<unsigned long long>(v));
    else std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    out += buf;
}

bool IsWin32kName(const std::string& n) { return n.rfind("NtUser", 0) == 0 || n.rfind("NtGdi", 0) == 0; }

}  // namespace

const HostNt& Host() { return S().host; }

// ---- Dispatch by name (any ntdll / win32u build) -------------------------------------
// The id in a thunk's `mov eax, imm32` is the numbering of whatever build the DLL comes from: Wine's
// (0x1000.. for win32u, the 22H2 numbers for ntdll) or, for a native win32u.dll, that Windows build's. The
// table is keyed by Wine's numbering, so the imm32 is rewritten to OUR id for the thunk's NAME here, while the
// image is still writable (MapImageView, right after loader::RewriteSyscallStubs). For Wine's own DLLs this is
// a no-op; for native ones it is what makes the id mean the right function.
std::size_t RenumberSyscallThunks(const std::vector<loader::StubPatch>& patched, const wchar_t* module) {
    State& s = S();
    std::size_t renumbered = 0, unknown = 0, extra = 0;
    std::string unknown_names;
    AcquireSRWLockExclusive(&s.lock);
    for (const loader::StubPatch& p : patched) {
        if (!p.address) continue;
        std::uint32_t want = kUnknownSyscallId;
        auto it = s.by_name.find(p.name);
        if (it != s.by_name.end()) {
            want = it->second->id;
        } else if (s.gui_native && s.host_win32u && IsWin32kName(p.name) && s.next_extra_id < kUnknownSyscallId) {
            // A win32u syscall of this Windows build that Wine's table does not have: pass it through by name too.
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(s.host_win32u, p.name.c_str()))) {
                auto e = std::make_unique<Entry>();
                e->id = s.next_extra_id++;
                e->name = _strdup(p.name.c_str());   // lives as long as the entry (forever)
                e->nargs = kMaxArgs;                 // unknown: copy the most the entry stub can
                e->policy = Policy::PassThrough;
                e->host_fn = fn;
                e->note = "win32u (host win32k, native-only)";
                want = e->id;
                s.by_name.emplace(e->name, e.get());
                s.by_id[e->id] = e.get();
                s.extra.push_back(std::move(e));
                ++extra;
            }
        }
        if (want == kUnknownSyscallId) {
            ++unknown;
            if (unknown <= 8) (unknown_names += unknown_names.empty() ? "" : ", ") += p.name;
        }
        auto* imm = static_cast<std::uint8_t*>(p.address) + 4;   // mov eax, imm32 at thunk+3: B8 <imm32>
        std::uint32_t have;
        std::memcpy(&have, imm, 4);
        if (have == want) continue;
        DWORD old = 0;
        const bool flip = VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old) != 0;
        std::memcpy(imm, &want, 4);
        if (flip) VirtualProtect(imm, 4, old, &old);
        ++renumbered;
    }
    ReleaseSRWLockExclusive(&s.lock);
    if (renumbered || unknown)
        TRACE(dispatch, "%ls: %zu of %zu syscall thunks renumbered to the table's ids by name (%zu added as native-only "
                        "win32u pass-through)", module ? module : L"?", renumbered, patched.size(), extra);
    if (unknown)
        FIXME(dispatch, "%ls: %zu syscall thunks have no table entry and dispatch as unknown (id %#x): %s%s",
              module ? module : L"?", unknown, kUnknownSyscallId, unknown_names.c_str(), unknown > 8 ? ", ..." : "");
    return renumbered;
}

NTSTATUS UseHostWin32u(HMODULE win32u) {
    State& s = S();
    if (!s.initialized || !win32u) return st::Unsuccessful;
    AcquireSRWLockExclusive(&s.lock);
    std::size_t pass = 0, todo = 0;
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        Entry& e = s.entries[i];
        if (e.id >= kWin32uFirstId) {
            e.host_fn = reinterpret_cast<void*>(GetProcAddress(win32u, e.name));
            e.policy = e.host_fn ? Policy::PassThrough : Policy::Todo;
            e.note = e.host_fn ? "win32u (host win32k)" : "win32u (no host export)";
            // Wine's argument counts follow Wine's prototypes, which are not Windows' in every case: copy the
            // most the entry stub can, the callee ignores the rest.
            e.nargs = kMaxArgs;
            ++(e.host_fn ? pass : todo);
            if (!std::strcmp(e.name, "NtUserEndPaint")) s.end_paint = &e;
        } else if (!std::strcmp(e.name, "NtCallbackReturn") && e.host_fn) {
            // The real kernel has the callback active and unwinds to the syscall that made it (docs/DISPATCHER.md
            // "User-mode callbacks in native GUI mode"): pass it through, and relay-log it before the call.
            e.policy = Policy::PassThrough;
            e.note = "host kernel unwinds the user-mode callback";
            s.noreturn[i] = 1;
        }
    }
    s.host_win32u = win32u;
    s.gui_native = true;
    ReleaseSRWLockExclusive(&s.lock);
    TRACE(dispatch, "gui native: %zu win32u syscalls pass through to the host's win32u.dll %p, %zu have no host "
                    "export (Todo); NtCallbackReturn passes through", pass, win32u, todo);
    return st::Success;
}

bool NativeGuiActive() { return g_state.gui_native; }

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
    if (!off) return nullptr;
    const std::uint64_t raw = __readgsqword(static_cast<unsigned long>(off));
    ThreadState* st = ValidateSlot(raw);
    if (!st && raw) {
        // Someone else owns TEB.TlsSlots[tls_index] now. Running on a foreign
        // pointer would write its contents into the TEB, so treat the thread as
        // having no state and make the breakage visible instead. Only a counter
        // here: this function runs inside the guest-view window (from
        // ReenterHostView), so it must touch no TLS and must not log --
        // win10xbox_dispatch reports it from the host view instead.
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&win10xbox_thread_slot_clobbered));
    }
    return st;
}

bool GuestViewActive() {
    // Deliberately not via CurrentThreadState(): S() calls this on every
    // syscall, so this skips the cookie check (a diagnostic, not a boundary)
    // and reads the slot directly. TLS-free, like everything in the window.
    const std::uint64_t off = win10xbox_thread_slot_offset;
    if (!off) return false;
    const std::uint64_t raw = __readgsqword(static_cast<unsigned long>(off));
    if (!raw || (raw >> kSlotTagShift) != SlotTagOf(raw)) return false;
    const auto* st = reinterpret_cast<const ThreadState*>(raw & kSlotPtrMask);
    // depth as well as the flag, and this is not belt and braces: the calls the
    // window exists for (NtContinue and friends) do NOT return, so
    // ReenterHostView never runs and the flag stays set for the life of the
    // thread. depth is what actually distinguishes "the guest is running" from
    // "we are back in host code": the asm entry stub and HostModeScope both
    // raise it before any host code runs, and LeaveToGuestView zeroes it. The
    // flag then keeps a thread that has never left for the guest -- depth is
    // also 0 during bootstrap, while the host view is still installed -- from
    // being reported.
    return st && (st->flags & kThreadGuestView) != 0 && st->depth == 0;
}

std::uint64_t GuestViewViolations() { return win10xbox_guest_view_violations; }

void NoteHostStateUnderGuestView(const char* what) {
    if (!GuestViewActive()) return;
    g_guest_view_culprit = what;   // literal; last one wins
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&win10xbox_guest_view_violations));
}

DWORD ThreadSlotIndex() { return S().tls_index; }
std::uint64_t ThreadSlotClobbers() { return win10xbox_thread_slot_clobbered; }

bool RepairThreadSlot() {
    if (!win10xbox_thread_slot_offset) return false;
    const DWORD tid = GetCurrentThreadId();
    ThreadState* mine = nullptr;
    AcquireSRWLockShared(&g_threads_lock);
    for (auto* t : Threads())
        if (t->tid == tid) { mine = t; break; }
    ReleaseSRWLockShared(&g_threads_lock);
    if (!mine) return false;
    TlsSetValue(S().tls_index, reinterpret_cast<void*>(TagSlot(mine)));
    return true;
}

// WIN10XBOX_TEST_TLS_CLOBBER=1 writes a 4-byte 0 through &TEB.TlsSlots[index]
// at guest entry -- byte for byte what a guest doing `errno = 0` through Wine's
// _errno() (wine-src/dlls/ntdll/thread.c:445) does to a slot it shares with us.
// It leaves our pointer non-NULL with its low dword gone, which is the exact
// shape the old `test rbx,rbx` in core/asm let through. Test hook; the variable
// is latched in Initialize because by guest entry the PEB's environment block
// is the guest's and no longer has the host's variables in it.
void TestClobberThreadSlot(const char* why) {
    if (!S().test_clobber_slot || !win10xbox_thread_slot_offset) return;
    auto* slot = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(__readgsqword(0x30)) +
                                                  win10xbox_thread_slot_offset);
    const std::uint64_t before = *slot;
    *reinterpret_cast<volatile int*>(slot) = 0;   // sizeof(int), like errno
    WARN(dispatch, "WIN10XBOX_TEST_TLS_CLOBBER: TEB.TlsSlots[%lu] %p -> %p, simulating a guest errno store (%s)",
         S().tls_index, reinterpret_cast<void*>(before), reinterpret_cast<void*>(*slot), why);
}

// WIN10XBOX_TEST_GUEST_VIEW_VIOLATION=1 makes Xl_NtContinue touch host state
// inside the guest-view window, which is the bug that crashed real Windows at
// dispatcher.cpp's `static State s;`. mingw's __cxa_guard needs no TLS, so the
// fault itself is unreachable under Wine -- but the ORDERING is not, and that
// is what the detector keys on, so this proves the check works on linux-cross.
// Test hook, latched in Initialize (the guest's environment block replaces the
// host's during the takeover).
void TestViolateGuestView() {
    if (!g_test_violate_view) return;
    // Both ways host code gets into trouble here: logging (trace::Log takes a
    // mutex and walks the sinks, two more function-local statics) and reaching
    // a singleton. The first is dropped and counted by the trace probe, the
    // second by NoteHostStateUnderGuestView.
    TRACE(dispatch, "WIN10XBOX_TEST_GUEST_VIEW_VIOLATION: logging inside the guest-view window");
#if defined(_MSC_VER)
    // Under MSVC the access itself is the fault the detector exists for: the
    // init guard of ExportCache's static reads _Init_thread_epoch through the
    // guest's TLS array (measured: ci.yml run 39818491081, windows
    // RelWithDebInfo, c0000005 reading 0x104 inside win10xbox.exe). Record
    // exactly what ExportCache() records on entry and stop short of the static:
    // the detector and its report are what this hook proves, on both hosts.
    NoteHostStateUnderGuestView("dispatch::ExportCache");
#else
    ExportCache();   // a function-local static: the exact shape that faults on MSVC
#endif
}

bool ThreadSlotIntact() {
    const std::uint64_t off = win10xbox_thread_slot_offset;
    return off && ValidateSlot(__readgsqword(static_cast<unsigned long>(off))) != nullptr;
}

void ReserveGuestTlsSlot(const char* why) {
    State& s = S();
    if (s.tls_index >= teb::TlsSlotCount) return;                    // nothing to protect
    void* bm = PebField<void*>(peb::TlsBitmap);
    // Until Wine's loader_init has installed its own RTL_BITMAP there is no
    // guest allocator yet; stay unlatched and look again on the next call.
    if (!bm || bm == s.host_tls_bitmap) return;
    if (InterlockedCompareExchange(&s.guest_tls_reserved, 1, 0) != 0) return;
    // RTL_BITMAP { ULONG SizeOfBitMap; ULONG *Buffer; } (x64: Buffer at +8)
    const std::uint32_t size = *static_cast<std::uint32_t*>(bm);
    auto* buffer = *reinterpret_cast<std::uint32_t**>(static_cast<std::uint8_t*>(bm) + 8);
    const auto* own_bits = reinterpret_cast<const std::uint32_t*>(CurrentPeb() + peb::TlsBitmapBits);
    if (!buffer || s.tls_index >= size) {
        ERR(dispatch, "guest TLS bitmap at %p unusable (size %lu, buffer %p): slot %lu NOT reserved (%s)", bm,
            static_cast<unsigned long>(size), static_cast<void*>(buffer), s.tls_index, why);
        return;
    }
    const std::uint32_t word = s.tls_index / 32, bit = 1u << (s.tls_index % 32);
    const bool already = (buffer[word] & bit) != 0;
    if (!already)
        InterlockedOr(reinterpret_cast<volatile LONG*>(&buffer[word]), static_cast<LONG>(bit));
    // "already set" means the guest's bitmap is backed by PEB.TlsBitmapBits and
    // therefore shares our host allocator's bits; "was clear" means it does not,
    // and this call is what keeps the guest from re-handing-out our slot.
    if (already)
        TRACE(dispatch, "guest TLS bitmap %p (bits %p, PEB.TlsBitmapBits %p, %lu slots): slot %lu already in use, "
                        "allocators share the PEB bits (%s)", bm, static_cast<void*>(buffer),
              static_cast<const void*>(own_bits), static_cast<unsigned long>(size), s.tls_index, why);
    else
        WARN(dispatch, "guest TLS bitmap %p (bits %p, PEB.TlsBitmapBits %p) did NOT have slot %lu marked in use: "
                       "reserved it now, the guest allocator would have handed it out (%s)", bm,
             static_cast<void*>(buffer), static_cast<const void*>(own_bits), s.tls_index, why);
}

ThreadState* AttachThreadState(std::uint32_t flags) {
    if (ThreadState* st = CurrentThreadState()) { st->flags |= flags; return st; }
    if (S().tls_index == TLS_OUT_OF_INDEXES) return nullptr;
    auto* st = new ThreadState{};
    if ((reinterpret_cast<std::uint64_t>(st) >> kSlotTagShift) != 0) {
        // Not reachable with x64 user-mode addresses; refuse rather than store a
        // pointer the slot tag cannot protect.
        ERR(dispatch, "ThreadState at %p has bits 48..63 set: TEB view switching off on thread %04lx", st,
            GetCurrentThreadId());
        delete st;
        return nullptr;
    }
    st->host_tls = st->guest_tls = __readgsqword(teb::ThreadLocalStoragePointer);
    st->host_fls = st->guest_fls = __readgsqword(teb::FlsData);
    st->flags = flags;
    st->tid = GetCurrentThreadId();
    st->magic = kThreadStateMagic;   // before the TEB slot points at it
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &st->thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    TlsSetValue(S().tls_index, reinterpret_cast<void*>(TagSlot(st)));
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
    st->magic = 0;   // a stale pointer to freed memory must not validate
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

void SetSyscallLastError(std::uint32_t error) {
    if (ThreadState* st = CurrentThreadState()) {
        st->syscall_error = error;
        st->syscall_error_set = 1;
    }
}

// win32u syscalls whose return type is NTSTATUS (include/ntuser.h, include/ntgdi.h,
// include/ddk/d3dkmthk.h). Every other win32u syscall returns a BOOL, a count, a
// handle or an LRESULT, for which STATUS_NOT_IMPLEMENTED (0xC0000002) would read
// as TRUE / a valid-looking handle: those report 0 when unimplemented.
bool Win32uReturnsNtStatus(const char* name) {
    static const char* const kNames[] = {
        "NtUserBuildHimcList", "NtUserBuildHwndList", "NtUserBuildNameList", "NtUserBuildPropList",
        "NtUserDisplayConfigGetDeviceInfo", "NtUserEnumDisplayDevices", "NtUserInitializeClientPfnArrays",
        "NtUserSetClipboardData",
    };
    if (!std::strncmp(name, "NtGdiDdDDI", 10)) return true;
    for (const char* n : kNames)
        if (!std::strcmp(n, name)) return true;
    return false;
}

std::vector<ThreadState*> GuestThreads() {
    AcquireSRWLockShared(&g_threads_lock);
    std::vector<ThreadState*> out;
    for (auto* t : Threads())
        if (t->flags & kThreadGuest) out.push_back(t);
    ReleaseSRWLockShared(&g_threads_lock);
    return out;
}

// TLS-free, and must stay so: both run with the guest TLS array installed for
// part of their body. See "The guest-view window" in internal.h.
std::uint32_t LeaveToGuestView() {
    ThreadState* st = CurrentThreadState();
    if (!st || !st->depth) return 0;
    std::uint32_t d = st->depth;
    SwapOut(st);
    st->depth = 0;
    st->flags |= kThreadGuestView;   // opens the window
    return d;
}

void ReenterHostView(std::uint32_t depth) {
    ThreadState* st = CurrentThreadState();
    if (!st || !depth) return;
    st->flags &= ~kThreadGuestView;   // closes it, before anything can log
    SwapIn(st);
    st->depth = depth;
}

}  // namespace detail

using namespace detail;

HostModeScope::HostModeScope()
    : state_(CurrentThreadState()),
      last_error_(__readgsdword(teb::LastErrorValue)),
      last_status_(static_cast<long>(__readgsdword(teb::LastStatusValue))) {
    auto* st = static_cast<ThreadState*>(state_);
    if (st && ++st->depth == 1) SwapIn(st);
}
HostModeScope::~HostModeScope() {
    __writegsdword(teb::LastErrorValue, last_error_);
    __writegsdword(teb::LastStatusValue, static_cast<unsigned long>(last_status_));
    auto* st = static_cast<ThreadState*>(state_);
    if (st && --st->depth == 0) SwapOut(st);
}

// ---- Host ntdll ---------------------------------------------------------------

// Must be primed before the PEB take-over: GetModuleHandleW resolves through
// PEB->Ldr, which Wine's loader_init replaces with the guest loader list, so a
// first call made afterwards would latch the GUEST ntdll and every "host"
// export would be a rewritten syscall stub. Initialize() primes it (and fails
// if it cannot), which is well before process::Launch takes the PEB over.
HMODULE HostNtdll() {
    State& s = S();
    if (!s.host_ntdll) {
        s.host_ntdll = GetModuleHandleW(L"ntdll.dll");
        if (s.initialized)
            ERR(dispatch, "host ntdll resolved after Initialize: it may be the guest's (PEB->Ldr is Wine's)");
    }
    return s.host_ntdll;
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
    probe::Install();   // before any guest syscall can hand an implementation a bad pointer
    State& s = S();
    AcquireSRWLockExclusive(&s.lock);
    if (s.initialized) { ReleaseSRWLockExclusive(&s.lock); return st::Success; }
    if (!HostNtdll()) { ReleaseSRWLockExclusive(&s.lock); return st::DllNotFound; }
    ResolveHost(s.host);
    PrimeChildProcesses();   // host kernel32 entry points and executable path, before the PEB take-over
    InitializeObjectNamespace();   // the host's named-object directory and this program tree's layer, before the take-over
    QueryPerformanceFrequency(&s.qpf);

    s.entries.clear();
    s.entries.reserve(std::size(kTable));
    std::size_t counts[4] = {};
    std::size_t missing = 0;
    std::string missing_names;
    for (const TableDef& t : kTable) {
        Entry e{};
        e.id = t.id;
        e.name = t.name;
        e.nargs = t.nargs ? t.nargs : NargsOverride(t.name);
        if (e.nargs > kMaxArgs) e.nargs = kMaxArgs;
        e.policy = Policy::Todo;
        e.note = "";
        if (t.id >= kWin32uFirstId) {
            // The win32k surface: no host export exists for an AppContainer (and the ids are Wine's own
            // numbering anyway), so an entry is Emulate when core/user implements it and Todo otherwise.
            // No per-syscall policy lines: 1540 of them would say the same thing.
            e.impl = FindImpl(t.name);
            e.policy = e.impl ? Policy::Emulate : Policy::Todo;
            e.note = e.impl ? "win32u (core/user)" : "win32u";
            ++counts[static_cast<int>(e.policy)];
            s.entries.push_back(e);
            continue;
        }
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
        if (e.policy == Policy::PassThrough && !e.host_fn) {
            TRACE(dispatch, "%s: host ntdll has no export, calls return STATUS_NOT_IMPLEMENTED", t.name);
            if (missing++ < 3) (missing_names += missing_names.empty() ? "" : ", ") += t.name;
        }
        ++counts[static_cast<int>(e.policy)];
        s.entries.push_back(e);
    }
    if (missing)
        WARN(dispatch, "%zu pass-through syscalls have no host ntdll export (%s%s); +dispatch lists them", missing,
             missing_names.c_str(), missing > 3 ? ", ..." : "");
    s.noreturn.assign(s.entries.size(), 0);
    std::fill(std::begin(s.by_id), std::end(s.by_id), nullptr);
    s.by_name.clear();
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        Entry& e = s.entries[i];
        if (e.id < kMaxId) s.by_id[e.id] = &e;
        s.by_name.emplace(e.name, &e);
        for (const char* n : kNoReturn)
            if (!std::strcmp(n, e.name)) s.noreturn[i] = 1;
    }

    // TLS slot for ThreadState; core/asm reads it through the TEB directly,
    // which needs a slot in TEB.TlsSlots[0..63]. Remember which bitmap the host
    // allocator used so ReserveGuestTlsSlot can tell when the guest installs its
    // own (this runs before the PEB take-over).
    // Lets trace::Log drop (and count) a line emitted inside the guest-view
    // window instead of faulting on its own statics.
    trace::SetUnsafeProbe(&GuestViewActive);
    s.host_tls_bitmap = PebField<void*>(peb::TlsBitmap);
    {
        wchar_t v[8];
        const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEST_TLS_CLOBBER", v, 8);
        s.test_clobber_slot = n != 0 && n < 8 && v[0] != L'0';
        const DWORD g = GetEnvironmentVariableW(L"WIN10XBOX_TEST_GUEST_VIEW_VIOLATION", v, 8);
        g_test_violate_view = g != 0 && g < 8 && v[0] != L'0';
    }
    s.tls_index = AllocThreadSlot();
    DWORD want = 0;
    bool force = false;
    if (TestSlotOverride(&want, &force)) {
        if (force) {
            WARN(dispatch, "WIN10XBOX_TEST_TLS_INDEX=!%lu: forcing TEB.TlsSlots[%lu] past the safety guard "
                           "(test hook; %s)", want, want,
                 SlotUsable(want) ? "index is safe anyway" : "index is one Wine claims by fiat");
            s.tls_index = want;
        } else if (SlotUsable(want)) {
            TRACE(dispatch, "WIN10XBOX_TEST_TLS_INDEX=%lu: using TEB.TlsSlots[%lu] (test hook)", want, want);
            s.tls_index = want;
        } else {
            WARN(dispatch, "WIN10XBOX_TEST_TLS_INDEX=%lu rejected: Wine's ntdll claims that slot by fiat "
                           "(0 reserved, %lu = NTDLL_TLS_ERRNO); staying on slot %lu", want, kWineErrnoSlot,
                 s.tls_index);
        }
    }
    if (s.tls_index != TLS_OUT_OF_INDEXES && s.tls_index < teb::TlsSlotCount) {
        win10xbox_thread_slot_offset = teb::TlsSlots + 8ull * s.tls_index;
        // Logged unconditionally: which index we hold is the first thing needed
        // to diagnose a TEB.TlsSlots collision, and it is not recoverable from
        // anywhere else in a host log or console trace.
        TRACE(dispatch, "ThreadState in TEB.TlsSlots[%lu] (TEB+%#llx)%s%s; PEB.TlsBitmap=%p", s.tls_index,
              static_cast<unsigned long long>(win10xbox_thread_slot_offset),
              s.tls_index < kPreferredSlot ? ", below the preferred slot " "19" : "",
              SlotUsable(s.tls_index) ? "" : " -- UNSAFE: Wine's ntdll claims this slot by fiat",
              s.host_tls_bitmap);
    } else {
        ERR(dispatch, "TLS slot %lu unusable: guest/host TEB view switching disabled", s.tls_index);
    }

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

std::uint64_t CallWithArgs64(void* fn, const std::uint64_t* a, std::uint32_t nargs) {
    using U = std::uint64_t;
    using Fn = U(NTAPI*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
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
    // A syscall never changes the thread's last error / last status, but the
    // host Win32 calls made below (and tracing) do: both live in the TEB the
    // guest shares, so put them back.
    const unsigned long last_error = __readgsdword(teb::LastErrorValue);
    const unsigned long last_status = __readgsdword(teb::LastStatusValue);
    State& s = S();
    // The guest's loader_init installs its own PEB.TlsBitmap before it makes
    // its first syscall, so this is the earliest point at which our slot can be
    // reserved against the guest's allocator -- ahead of any guest DLL's
    // DLL_PROCESS_ATTACH, which is the first thing that could call TlsAlloc.
    // One relaxed load per syscall until it latches.
    if (!s.guest_tls_reserved) ReserveGuestTlsSlot("first guest syscall");
    if (win10xbox_guest_view_violations || trace::DroppedUnsafe()) {
        static volatile LONG view_reported = 0;
        if (!InterlockedExchange(&view_reported, 1)) {
            const char* who = g_guest_view_culprit;
            ERR(dispatch, "host state reached under the guest TLS view %llu times (last: %s), %llu trace lines "
                          "dropped there: any thread-local access in that window faults on MSVC "
                          "(function-local statics included) -- resolve it before LeaveToGuestView, "
                          "e.g. via CallInGuestView",
                static_cast<unsigned long long>(win10xbox_guest_view_violations), who ? who : "?",
                static_cast<unsigned long long>(trace::DroppedUnsafe()));
        }
    }
    if (win10xbox_thread_slot_clobbered && !ThreadSlotIntact()) {
        // Detected, so not silent TEB corruption any more. Put our tagged
        // pointer back: whoever wrote over it is sharing the slot with us, and
        // losing the view switch breaks host code far more than it breaks them.
        const bool fixed = RepairThreadSlot();
        static volatile LONG reported = 0;
        if (!InterlockedExchange(&reported, 1))
            ERR(dispatch, "TEB.TlsSlots[%lu] was overwritten (%llu times): the guest is using our slot; %s",
                s.tls_index, static_cast<unsigned long long>(win10xbox_thread_slot_clobbered),
                fixed ? "restored it" : "could not restore it, TEB view switching is off on this thread");
    }
    // Nested syscalls (a window procedure called back from inside one) keep their own override.
    ThreadState* const self = CurrentThreadState();
    const std::uint32_t outer_error = self ? self->syscall_error : 0;
    const std::uint32_t outer_error_set = self ? self->syscall_error_set : 0;
    if (self) self->syscall_error_set = 0;
    Entry* e = f->id < kMaxId ? s.by_id[f->id] : nullptr;
    if (!e) {
        if (self) { self->syscall_error = outer_error; self->syscall_error_set = outer_error_set; }
        FIXME(dispatch, "unknown syscall id %#x (ret=%p)", f->id, reinterpret_cast<void*>(f->return_address));
        __writegsdword(teb::LastErrorValue, last_error);
        __writegsdword(teb::LastStatusValue, last_status);
        return st::NotImplemented;
    }
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&e->calls));
    const bool relay = trace::Enabled(&__win10xbox_dbch_relay, trace::Class::Trace);
    const bool timing = relay && trace::Enabled(&__win10xbox_dbch_timing, trace::Class::Trace);
    if (relay && e >= s.entries.data() && e < s.entries.data() + s.entries.size() &&   // not the native-only extras
        s.noreturn[static_cast<std::size_t>(e - s.entries.data())]) {
        std::string line = FormatRelay(*e, f->args, 0);
        line.resize(line.rfind(" ret="));
        trace::Log(&__win10xbox_dbch_relay, trace::Class::Trace, "", "%s (may not return)", line.c_str());
    }
    LARGE_INTEGER t0{}, t1{};
    if (timing) QueryPerformanceCounter(&t0);

    NTSTATUS status;
    // The win32u surface returns whole 64-bit values (HWND, LRESULT, LONG_PTR, pointers).
    const bool win32u = e->id >= kWin32uFirstId;
    std::uint64_t result64 = 0;
    // Route B (gui native): a win32u syscall handed to the host's own win32k. win32k reports errors by writing
    // TEB.LastErrorValue itself, which the guest must see.
    const bool native_win32u = s.gui_native && win32u && e->policy == Policy::PassThrough && e->host_fn;
    unsigned long native_error = last_error;
    if (native_win32u) {
        NoteNativeWin32uCall();   // installs the callback trampolines once the guest's user32 has its table in
        __writegsdword(teb::LastErrorValue, last_error);
        result64 = CallWithArgs64(e->host_fn, f->args, e->nargs);
        native_error = __readgsdword(teb::LastErrorValue);
        status = static_cast<NTSTATUS>(result64);
    } else
    switch (e->policy) {
    case Policy::PassThrough:
        status = e->host_fn ? CallWithArgs(e->host_fn, f->args, e->nargs) : st::NotImplemented;
        if (!e->host_fn) FIXME(dispatch, "%s: no host export", e->name);
        break;
    case Policy::Translate:
    case Policy::Emulate:
        if (e->impl && win32u) {
            result64 = CallWithArgs64(e->impl, f->args, e->nargs);
            status = static_cast<NTSTATUS>(result64);
        }
        else if (e->impl) status = CallWithArgs(e->impl, f->args, e->nargs);
        else if (e->host_fn) status = CallWithArgs(e->host_fn, f->args, e->nargs);
        else status = st::NotImplemented;
        break;
    case Policy::Todo:
    default:
        FIXME(dispatch, "unimplemented syscall %s", e->name);
        status = st::NotImplemented;
        if (win32u) {
            // Not a silent success: 0 is the failure value of every non-NTSTATUS win32u call, and the
            // guest's GetLastError says why.
            if (!Win32uReturnsNtStatus(e->name)) status = 0;
            result64 = static_cast<std::uint32_t>(status);
            if (self) { self->syscall_error = ERROR_CALL_NOT_IMPLEMENTED; self->syscall_error_set = 1; }
        }
        break;
    }

    if (relay) {
        double us = -1.0;
        if (timing) {
            QueryPerformanceCounter(&t1);
            us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(s.qpf.QuadPart ? s.qpf.QuadPart : 1);
        }
        std::string line = FormatRelay(*e, f->args, status, us);
        if (win32u && (result64 >> 32)) {
            char hi[32];
            std::snprintf(hi, sizeof hi, " (ret64=%016llx)", static_cast<unsigned long long>(result64));
            line += hi;
        }
        trace::Log(&__win10xbox_dbch_relay, trace::Class::Trace, "", "%s", line.c_str());
    }
    if (native_win32u) {
        if (e == s.end_paint) NativeGuestPainted();   // core/user/native_compose.cpp: snapshot for presentation
        __writegsdword(teb::LastErrorValue, native_error);
        __writegsdword(teb::LastStatusValue, last_status);
        if (self) { self->syscall_error = outer_error; self->syscall_error_set = outer_error_set; }
        return static_cast<std::int64_t>(result64);
    }
    __writegsdword(teb::LastErrorValue, self && self->syscall_error_set ? self->syscall_error : last_error);
    __writegsdword(teb::LastStatusValue, last_status);
    if (self) { self->syscall_error = outer_error; self->syscall_error_set = outer_error_set; }
    if (win32u) return static_cast<std::int64_t>(result64);
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
