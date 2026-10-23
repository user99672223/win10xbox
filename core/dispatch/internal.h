// Internal interfaces of core/dispatch (dispatcher, translators, bootstrap).
// Not a public header: only core/dispatch/*.cpp and tests/unit/test_dispatch.cpp
// include it.
#pragma once
#include "win10xbox/dispatch.h"
#include "win10xbox/loader.h"
#include "win10xbox/trace.h"
#include <intrin.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

WIN10XBOX_EXTERN_CHANNEL(wine);
WIN10XBOX_EXTERN_CHANNEL(process);
// DLL/module loading: opens of image files, sections, views, identity checks,
// the guest's loaded-module list and its import resolution (nt_section.cpp).
WIN10XBOX_EXTERN_CHANNEL(modules);

namespace win10xbox::dispatch::detail {

// ---- NTSTATUS values (own names: windows.h defines some STATUS_* as DWORD) --
namespace st {
constexpr NTSTATUS Success                = 0x00000000;
constexpr NTSTATUS Pending                = 0x00000103;
constexpr NTSTATUS ImageNotAtBase         = 0x40000003;
constexpr NTSTATUS BufferOverflow         = (NTSTATUS)0x80000005;
constexpr NTSTATUS NoMoreEntries          = (NTSTATUS)0x8000001A;
constexpr NTSTATUS Unsuccessful           = (NTSTATUS)0xC0000001;
constexpr NTSTATUS NotImplemented         = (NTSTATUS)0xC0000002;
constexpr NTSTATUS InvalidInfoClass       = (NTSTATUS)0xC0000003;
constexpr NTSTATUS InfoLengthMismatch     = (NTSTATUS)0xC0000004;
constexpr NTSTATUS AccessViolation        = (NTSTATUS)0xC0000005;
constexpr NTSTATUS InvalidHandle          = (NTSTATUS)0xC0000008;
constexpr NTSTATUS InvalidParameter       = (NTSTATUS)0xC000000D;
constexpr NTSTATUS NoMemory               = (NTSTATUS)0xC0000017;
constexpr NTSTATUS ConflictingAddresses   = (NTSTATUS)0xC0000018;
constexpr NTSTATUS AccessDenied           = (NTSTATUS)0xC0000022;
constexpr NTSTATUS BufferTooSmall         = (NTSTATUS)0xC0000023;
constexpr NTSTATUS ObjectTypeMismatch     = (NTSTATUS)0xC0000024;
constexpr NTSTATUS ObjectNameInvalid      = (NTSTATUS)0xC0000033;
constexpr NTSTATUS ObjectNameNotFound     = (NTSTATUS)0xC0000034;
constexpr NTSTATUS ObjectPathNotFound     = (NTSTATUS)0xC000003A;
constexpr NTSTATUS PrivilegeNotHeld       = (NTSTATUS)0xC0000061;
constexpr NTSTATUS InvalidImageFormat     = (NTSTATUS)0xC000007B;
constexpr NTSTATUS NotSupported           = (NTSTATUS)0xC00000BB;
constexpr NTSTATUS InvalidImageNotMz      = (NTSTATUS)0xC000012F;
constexpr NTSTATUS DllNotFound            = (NTSTATUS)0xC0000135;
constexpr NTSTATUS InvalidImageWin32      = (NTSTATUS)0xC0000359;
constexpr NTSTATUS NotSameDevice          = (NTSTATUS)0xC00000D4;
constexpr NTSTATUS InvalidAddress         = (NTSTATUS)0xC0000141;
constexpr NTSTATUS NotMappedView          = (NTSTATUS)0xC0000019;
}  // namespace st

inline bool NtSuccess(NTSTATUS s) { return s >= 0; }

// ---- x64 TEB/PEB offsets we touch (stable since Windows 7; Wine matches) ----
namespace teb {
constexpr std::uint32_t StackBase                 = 0x08;
constexpr std::uint32_t StackLimit                = 0x10;
constexpr std::uint32_t ThreadLocalStoragePointer = 0x58;
constexpr std::uint32_t Peb                       = 0x60;
constexpr std::uint32_t DeallocationStack         = 0x1478;
constexpr std::uint32_t LastErrorValue            = 0x68;
constexpr std::uint32_t LastStatusValue           = 0x1250;
constexpr std::uint32_t TlsSlots                  = 0x1480;
constexpr std::uint32_t FlsData                   = 0x17C8;
// TEB.TlsSlots is an INLINE array of this many pointers, not a pointer to an
// array: unlike ThreadLocalStoragePointer and FlsData (both swapped per view by
// SwapIn/SwapOut below), it cannot be given a separate host and guest copy, so
// it is the one per-thread surface the guest's TlsAlloc and our ThreadState
// slot share. See "The TLS slot we hold" in docs/DISPATCHER.md.
constexpr std::uint32_t TlsSlotCount              = 64;
// TEB.Win32ClientInfo[62]: user32 reads `struct ntuser_thread_info` at its start (ntuser.h NtUserGetThreadInfo);
// core/user keeps its own per-thread pointer in the LAST slot.
constexpr std::uint32_t Win32ClientInfo           = 0x800;
constexpr std::uint32_t Win32ClientInfoCount      = 62;
}
namespace peb {
constexpr std::uint32_t ImageBaseAddress   = 0x10;
constexpr std::uint32_t Ldr                = 0x18;
constexpr std::uint32_t ProcessParameters  = 0x20;
constexpr std::uint32_t ProcessHeap        = 0x30;
constexpr std::uint32_t FastPebLock        = 0x38;
constexpr std::uint32_t TlsBitmap          = 0x78;
constexpr std::uint32_t TlsBitmapBits      = 0x80;   // ULONG[2]: the bits PEB.TlsBitmap points at
constexpr std::uint32_t AnsiCodePageData   = 0xA0;
constexpr std::uint32_t OemCodePageData    = 0xA8;
constexpr std::uint32_t UnicodeCaseTableData = 0xB0;
constexpr std::uint32_t LoaderLock         = 0x110;
constexpr std::uint32_t OSMajorVersion     = 0x118;
constexpr std::uint32_t OSMinorVersion     = 0x11C;
constexpr std::uint32_t OSBuildNumber      = 0x120;   // USHORT
constexpr std::uint32_t OSCSDVersion       = 0x122;   // USHORT
constexpr std::uint32_t OSPlatformId       = 0x124;
constexpr std::uint32_t TlsExpansionBitmap = 0x238;
constexpr std::uint32_t CSDVersion         = 0x2E8;   // UNICODE_STRING
// The process-default activation context (the EXE's manifest). Wine's loader_init -> actctx_init() stores its OWN
// process_actctx here (wine-src/dlls/ntdll/actctx.c: `Peb->ActivationContextData = process_actctx`; the static
// `system_actctx` inside Wine's ntdll when the EXE has no manifest), and a Wine ACTIVATION_CONTEXT is not Microsoft's
// ACTIVATION_CONTEXT_DATA blob. Offset checked against wine-src/include/winternl.h (x64: 0x2f8).
constexpr std::uint32_t ActivationContextData = 0x2F8;
// The win32k surface (core/user): user32 reads PEB.KernelCallbackTable (set by its DllMain, read back by our
// user-mode callback), gdi32 decodes every HGDIOBJ through PEB.GdiSharedHandleTable (x64: 0x58 / 0xF8).
constexpr std::uint32_t KernelCallbackTable = 0x58;
constexpr std::uint32_t GdiSharedHandleTable = 0xF8;
}
inline std::uint8_t* CurrentPeb() { return reinterpret_cast<std::uint8_t*>(__readgsqword(teb::Peb)); }
template <class T> T& PebField(std::uint32_t off) { return *reinterpret_cast<T*>(CurrentPeb() + off); }

// ---- Per-thread state (TLS slot; layout shared with core/asm) ---------------
// `magic` must hold kThreadStateMagic: core/asm and CurrentThreadState() check
// it before trusting the TEB slot, so a foreign value written into
// TEB.TlsSlots[tls_index] degrades into a counted, logged error instead of a
// bogus pointer whose contents get written into the TEB (see ValidateSlot in
// dispatcher.cpp and the view-switch block in core/asm/syscall_entry.S).
struct ThreadState {
    std::uint64_t host_tls;     // 0x00
    std::uint64_t host_fls;     // 0x08
    std::uint64_t guest_tls;    // 0x10
    std::uint64_t guest_fls;    // 0x18
    std::uint32_t depth;        // 0x20: >0 = host view active
    std::uint32_t flags;        // 0x24
    DWORD         tid;          // 0x28
    std::uint32_t magic;        // 0x2C: kThreadStateMagic (was padding; checked by core/asm)
    HANDLE        thread;       // 0x30: duplicated host handle (guest threads), may be null
    // Wine debug output (replacements for ntdll's per-TEB debug_info)
    char          dbg_line[1024];
    std::uint32_t dbg_pos;
    std::uint8_t  dbg_class;
    char          dbg_prefix[96];
    char          dbg_strings[1024];
    std::uint32_t dbg_str_pos;
    // The guest-visible last error a syscall wants to report. win10xbox_dispatch
    // restores TEB.LastErrorValue around every syscall (host calls clobber it);
    // an emulation that has an error to report (core/user: the win32u surface,
    // where user32 reads GetLastError after a failed NtUser call) sets these
    // through SetSyscallLastError and the dispatcher writes that value instead.
    std::uint32_t syscall_error;
    std::uint32_t syscall_error_set;
};
static_assert(offsetof(ThreadState, host_tls) == 0x00);
static_assert(offsetof(ThreadState, host_fls) == 0x08);
static_assert(offsetof(ThreadState, guest_tls) == 0x10);
static_assert(offsetof(ThreadState, guest_fls) == 0x18);
static_assert(offsetof(ThreadState, depth) == 0x20);
static_assert(offsetof(ThreadState, magic) == 0x2C);   // core/asm/syscall_entry.{asm,S}

// Cookie in ThreadState::magic. core/asm compares against this literal, so a
// change here needs the same change in BOTH asm twins.
constexpr std::uint32_t kThreadStateMagic = 0x57310B01;   // "W10XB01"

constexpr std::uint32_t kThreadGuest       = 1u << 0;   // runs guest code
constexpr std::uint32_t kThreadMain        = 1u << 1;   // guest main thread (bootstrap)
constexpr std::uint32_t kThreadCallerOwned = 1u << 2;   // bootstrap ran on the Launch caller's thread
constexpr std::uint32_t kThreadGuestView   = 1u << 3;   // between LeaveToGuestView and ReenterHostView

ThreadState* CurrentThreadState();
// Creates (or returns) the calling thread's state, capturing the host TLS/FLS
// pointers currently in the TEB.
ThreadState* AttachThreadState(std::uint32_t flags);
void DetachThreadState();
// Put the TEB in the guest view and reset nesting (before host calls that do
// not return to us on success: NtContinue, NtRaiseException, ...). Returns the
// depth to restore with ReenterHostView if the call does return.
std::uint32_t LeaveToGuestView();
void ReenterHostView(std::uint32_t depth);
std::vector<ThreadState*> GuestThreads();   // snapshot
// See ThreadState::syscall_error. Host view only (from inside a syscall).
void SetSyscallLastError(std::uint32_t error);

// ---- The guest-view window --------------------------------------------------
// Between LeaveToGuestView() and ReenterHostView() the TEB shows the GUEST's
// ThreadLocalStoragePointer, which has no block for the host image. Host code
// that touches ANY thread-local storage in that window therefore reads through
// the guest's TLS array and faults -- and under MSVC that includes code with no
// thread_local in sight, because a function-local `static` emits a thread-safe
// init guard that reads `_Init_thread_epoch` at offset 4 of the host module's
// TLS block. That is a null+4 load, which is how `static State s;` in
// dispatcher.cpp crashed Xl_NtContinue on real Windows while mingw (libc++abi
// __cxa_guard, no TLS at all) stayed green.
//
// The rule: nothing but the functions listed as TLS-free below may run in that
// window. Everything a transfer needs is resolved BEFORE the swap, which
// CallInGuestView makes the compiler guarantee rather than the author remember.
// TLS-free, and required to stay so: CurrentThreadState, SwapIn/SwapOut,
// LeaveToGuestView, ReenterHostView, GuestViewActive, core/asm.
bool GuestViewActive();
// Records that host state was reached inside the window; `what` must be a
// literal. Cannot log (logging is itself unsafe there): it counts, and the next
// syscall reports it from the host view. Zero violations is an invariant the
// Wine job gates on, which is what makes this class visible without MSVC.
void NoteHostStateUnderGuestView(const char* what);
std::uint64_t GuestViewViolations();

// Resumes the guest with `fn(args...)`, a host entry point that does not return
// on success. Every argument -- including the function pointer, so write it as
// Host().NtX -- is evaluated before this call, hence before the view switch:
// that ordering is what keeps the window free of host code.
// Test hook (WIN10XBOX_TEST_GUEST_VIEW_VIOLATION): touches host state inside
// the guest-view window so the detector can be proven under Wine. No-op
// unless the variable is set. Call only from inside the window.
void TestViolateGuestView();

template <class Fn, class... Args>
NTSTATUS CallInGuestView(Fn fn, Args... args) {
    const std::uint32_t depth = LeaveToGuestView();
    TestViolateGuestView();   // no-op unless WIN10XBOX_TEST_GUEST_VIEW_VIOLATION is set
    const NTSTATUS s = fn(args...);
    ReenterHostView(depth);
    return s;
}

// ---- The TLS slot we hold in TEB.TlsSlots (dispatcher.cpp) -------------------
// TEB.TlsSlots has two independent allocators in one process: our host ntdll's
// (through TlsAlloc, at Initialize, before the guest takeover) and the guest's
// Wine ntdll's. They coordinate through PEB.TlsBitmap, whose bit storage is
// PEB.TlsBitmapBits inside the PEB itself, so a bit set by one is visible to
// the other -- EXCEPT for the indices Wine's loader_init claims by fiat with
// RtlSetBits instead of allocating (wine-pe/wine-src/dlls/ntdll/loader.c:4454).
// Holding one of those is unsafe whatever the bitmap says, so we never keep one:
constexpr DWORD kWineErrnoSlot  = 16;   // NTDLL_TLS_ERRNO (wine-src/dlls/ntdll/ntdll_misc.h:36)
constexpr DWORD kWineWow64Slots = 19;   // WOW64_TLS_MAX_NUMBER (wine-src/include/winternl.h:1322)
// Both allocators hand out the lowest clear bit, so an index at or above the
// WoW64 block is also the one least likely to be reached by the guest at all.
constexpr DWORD kPreferredSlot  = kWineWow64Slots;
inline bool SlotUsable(DWORD i) { return i != 0 && i != kWineErrnoSlot && i < teb::TlsSlotCount; }

DWORD ThreadSlotIndex();   // TLS_OUT_OF_INDEXES when view switching is disabled
// Makes sure our index is marked in-use in whatever bitmap PEB.TlsBitmap points
// at once the guest has installed its own, so the guest's allocator can never
// hand it out. A no-op until Wine's loader_init has replaced the pointer, and
// then once only; reports what it found (the "already set" case is what proves
// the two allocators really do share PEB.TlsBitmapBits on this host).
void ReserveGuestTlsSlot(const char* why);
// TEB.TlsSlots[tls_index] holds a cookie-valid ThreadState on this thread.
bool ThreadSlotIntact();
// How often core/asm or CurrentThreadState() found a foreign value in the slot.
std::uint64_t ThreadSlotClobbers();
// Puts this thread's tagged ThreadState pointer back into the slot.
bool RepairThreadSlot();
// Test hook (WIN10XBOX_TEST_TLS_CLOBBER): simulates a guest errno store into
// our slot. No-op unless the variable is set.
void TestClobberThreadSlot(const char* why);

// ---- Typed host ntdll entry points used by translators -------------------------
using PFN_NtClose = NTSTATUS(NTAPI*)(HANDLE);
using PFN_NtCreateFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                          ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using PFN_NtOpenFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using PFN_NtCreateNamedPipeFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG,
                                                   ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, PLARGE_INTEGER);
using PFN_NtQueryAttributesFile = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using PFN_NtDeleteFile = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
using PFN_NtQueryInformationFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using PFN_NtSetInformationFile = PFN_NtQueryInformationFile;
using PFN_RtlCreateHeap = PVOID(NTAPI*)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);
using PFN_NtQueryObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_NtOpenObject = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using PFN_NtCreateSection = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
using PFN_NtCreateSectionEx = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG,
                                               HANDLE, PVOID, ULONG);
using PFN_NtMapViewOfSection = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T,
                                                ULONG, ULONG, ULONG);
using PFN_NtMapViewOfSectionEx = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
using PFN_NtUnmapViewOfSection = NTSTATUS(NTAPI*)(HANDLE, PVOID);
using PFN_NtUnmapViewOfSectionEx = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG);
using PFN_NtQuerySection = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, SIZE_T, PSIZE_T);
using PFN_NtAreMappedFilesTheSame = NTSTATUS(NTAPI*)(PVOID, PVOID);
using PFN_NtDuplicateObject = NTSTATUS(NTAPI*)(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
using PFN_NtQueryVirtualMemory = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
using PFN_NtQueryInformationProcess = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_NtQueryInformationThread = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using PFN_NtQuerySecurityObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_NtTerminateProcess = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
using PFN_NtTerminateThread = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);
using PFN_NtContinue = NTSTATUS(NTAPI*)(PCONTEXT, BOOLEAN);
using PFN_NtContinueEx = NTSTATUS(NTAPI*)(PCONTEXT, PVOID);
using PFN_NtRaiseException = NTSTATUS(NTAPI*)(PEXCEPTION_RECORD, PCONTEXT, BOOLEAN);
using PFN_NtSetContextThread = NTSTATUS(NTAPI*)(HANDLE, PCONTEXT);
using PFN_NtCreateThreadEx = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PVOID, PVOID, ULONG,
                                              SIZE_T, SIZE_T, SIZE_T, PVOID);
using PFN_NtQuerySystemTime = NTSTATUS(NTAPI*)(PLARGE_INTEGER);
using PFN_RtlCreateProcessParametersEx = NTSTATUS(NTAPI*)(PVOID*, PUNICODE_STRING, PUNICODE_STRING, PUNICODE_STRING,
                                                          PUNICODE_STRING, PVOID, PUNICODE_STRING, PUNICODE_STRING,
                                                          PUNICODE_STRING, PUNICODE_STRING, ULONG);

struct HostNt {
    PFN_NtClose NtClose;
    PFN_NtCreateFile NtCreateFile;
    PFN_NtOpenFile NtOpenFile;
    PFN_NtCreateNamedPipeFile NtCreateNamedPipeFile;
    PFN_NtQueryAttributesFile NtQueryAttributesFile;
    PFN_NtQueryAttributesFile NtQueryFullAttributesFile;
    PFN_NtDeleteFile NtDeleteFile;
    PFN_NtQueryInformationFile NtQueryInformationFile;
    PFN_NtSetInformationFile NtSetInformationFile;
    PFN_NtQueryObject NtQueryObject;
    PFN_RtlCreateHeap RtlCreateHeap;
    PFN_NtOpenObject NtOpenDirectoryObject;
    PFN_NtOpenObject NtOpenSymbolicLinkObject;
    PFN_NtCreateSection NtCreateSection;
    PFN_NtCreateSectionEx NtCreateSectionEx;
    PFN_NtMapViewOfSection NtMapViewOfSection;
    PFN_NtMapViewOfSectionEx NtMapViewOfSectionEx;
    PFN_NtUnmapViewOfSection NtUnmapViewOfSection;
    PFN_NtUnmapViewOfSectionEx NtUnmapViewOfSectionEx;
    PFN_NtQuerySection NtQuerySection;
    PFN_NtAreMappedFilesTheSame NtAreMappedFilesTheSame;
    PFN_NtDuplicateObject NtDuplicateObject;
    PFN_NtQueryVirtualMemory NtQueryVirtualMemory;
    PFN_NtQueryInformationProcess NtQueryInformationProcess;
    PFN_NtQueryInformationThread NtQueryInformationThread;
    PFN_NtQuerySystemInformation NtQuerySystemInformation;
    PFN_NtQuerySecurityObject NtQuerySecurityObject;
    PFN_NtTerminateProcess NtTerminateProcess;
    PFN_NtTerminateThread NtTerminateThread;
    PFN_NtContinue NtContinue;
    PFN_NtContinueEx NtContinueEx;
    PFN_NtRaiseException NtRaiseException;
    PFN_NtSetContextThread NtSetContextThread;
    PFN_NtCreateThreadEx NtCreateThreadEx;
    PFN_NtQuerySystemTime NtQuerySystemTime;
    PFN_RtlCreateProcessParametersEx RtlCreateProcessParametersEx;
};
const HostNt& Host();   // valid after Initialize()

// ---- Implementation registry ------------------------------------------------
struct ImplDef { const char* name; void* fn; };
// Translators/emulations implemented in this module, each list terminated by
// {nullptr, nullptr}: nt_file.cpp, nt_section.cpp, nt_process.cpp, and the
// subsystem entry points that are built (subsystems.cpp).
const ImplDef* FileImpls();
const ImplDef* SectionImpls();
const ImplDef* ProcessImpls();
const ImplDef* SubsystemImpls();
// core/user: the win32u surface (NtUser*/NtGdi*, ids 0x1000..) and NtCallbackReturn.
const ImplDef* UserImpls();
// nt_privileged.cpp: privileged syscalls, refused the way Windows refuses an unprivileged caller.
const ImplDef* PrivilegedImpls();
// Subsystem groups compiled out: their Emulate entries pass through to the
// host (registry/token/clock reads must work for Wine's loader_init) except
// the names listed by NeverPassThrough().
bool NeverPassThrough(const char* name);
// Syscalls whose argument count in Wine's table is wrong for Windows (Wine
// lists unimplemented ones as 0); returns 0 if no correction.
std::uint32_t NargsOverride(const char* name);

// ---- Dispatch by name, and route B (native user32/gdi32/win32u on the host's win32k) ----
// Rewrites the `mov eax, imm32` of every rewritten thunk in `patched` (loader::RewriteSyscallStubs) to the
// table's id for the thunk's name, so a thunk dispatches by NAME whatever build its DLL comes from (dispatcher.cpp).
// A name with no entry gets an id no entry has (FIXME, "unknown syscall"); with the native GUI active, a
// NtUser*/NtGdi* name the host's win32u.dll exports gets a pass-through entry of its own instead. Returns how
// many ids changed. The thunks must still be writable or protectable.
std::size_t RenumberSyscallThunks(const std::vector<loader::StubPatch>& patched, const wchar_t* module);
// Every win32u entry becomes PassThrough to the same-named export of `win32u` (the host's win32u.dll), or Todo
// without one, and NtCallbackReturn passes through to the host kernel (dispatcher.cpp).
NTSTATUS UseHostWin32u(HMODULE win32u);
bool NativeGuiActive();
// win32k_native.cpp. Loads the host's user32/gdi32/win32u (before the PEB take-over: LoadLibrary walks
// PEB->Ldr), connects the process to win32k, switches the win32u table over (UseHostWin32u) and resolves the
// host GUI functions core/user's native composer uses. Called by process::Launch when config gui is Native.
NTSTATUS ConfigureNativeGui();
// The guest ntdll's user32-facing exports replaced in native mode (RtlInitializeNtUserPfn & co.,
// NtdllDefWindowProc_*, NtdllDialogWndProc_*, CsrClientConnectToServer) and their replacements, which fit
// the NATIVE user32 (win32k_native.cpp). ApplyRtlForwarding patches them.
struct NamedTarget { const char* name; const void* fn; };
std::vector<NamedTarget> NativeUserOverrides();
// Traces what RtlInitializeNtUserPfn received, once registered (host view).
void NoteNativeUserPfn();
// Called by win10xbox_dispatch before every win32u pass-through: once the guest's user32 has written its own
// table into PEB.KernelCallbackTable, replaces it with our per-index trampolines, which route each user-mode
// callback by thread (guest thread: the guest's user32 in the guest TLS view; any other thread: the host's
// user32). Host view only.
void NoteNativeWin32uCall();
// User-mode callbacks routed to the guest's user32 / to the host's so far.
std::uint64_t NativeCallbacksToGuest();
std::uint64_t NativeCallbacksToHost();
// core/user/native_compose.cpp: the guest finished a paint (NtUserEndPaint passed through). Snapshots the
// guest's windows for user::ComposeFrame and the screenshot. Host view only.
void NativeGuestPainted();
// The host's user32/gdi32 entry points core/user's native composer needs, resolved by ConfigureNativeGui
// while PEB->Ldr was still the host's (GetModuleHandle afterwards would find the GUEST's copies).
struct HostGui {
    decltype(&::EnumWindows) EnumWindows;
    decltype(&::GetWindowThreadProcessId) GetWindowThreadProcessId;
    decltype(&::IsWindowVisible) IsWindowVisible;
    decltype(&::IsIconic) IsIconic;
    decltype(&::GetWindowRect) GetWindowRect;
    decltype(&::GetWindowDC) GetWindowDC;
    decltype(&::ReleaseDC) ReleaseDC;
    decltype(&::GetSystemMetrics) GetSystemMetrics;
    decltype(&::CreateCompatibleDC) CreateCompatibleDC;
    decltype(&::CreateDIBSection) CreateDIBSection;
    decltype(&::SelectObject) SelectObject;
    decltype(&::BitBlt) BitBlt;
    decltype(&::DeleteDC) DeleteDC;
    decltype(&::DeleteObject) DeleteObject;
    decltype(&::GdiFlush) GdiFlush;
};
const HostGui* HostGuiFunctions();   // null unless ConfigureNativeGui succeeded

// ---- Handles ------------------------------------------------------------------
inline bool IsCurrentProcess(HANDLE h) { return h == reinterpret_cast<HANDLE>(-1) || (h && GetProcessId(h) == GetCurrentProcessId()); }
inline bool IsCurrentThreadHandle(HANDLE h) {
    return h == reinterpret_cast<HANDLE>(-2) || (h && GetThreadId(h) == GetCurrentThreadId());
}

// ---- Path helpers (nt_file.cpp) ----------------------------------------------
struct TranslatedOA {
    const OBJECT_ATTRIBUTES* attr = nullptr;   // set when translated
    std::shared_ptr<void> keep;                // owns attr and its name buffer
    POBJECT_ATTRIBUTES Get(const OBJECT_ATTRIBUTES* original) const {
        return const_cast<POBJECT_ATTRIBUTES>(attr ? attr : original);
    }
};
// Name of an OBJECT_ATTRIBUTES as a string view (empty when absent).
std::wstring_view OAName(const OBJECT_ATTRIBUTES* oa);
// Guest OBJECT_ATTRIBUTES -> host. STATUS_SUCCESS (translated or untouched) or
// an error the syscall should return (e.g. unmapped drive).
NTSTATUS TranslateOA(const OBJECT_ATTRIBUTES* in, TranslatedOA* out);
// Guest NT path ("\??\C:\x") -> host NT path. Same contract as vfs::TranslateNtPath.
NTSTATUS TranslateGuestNtPath(std::wstring_view guest, std::wstring* host);
// Host Win32 path -> guest Win32 path; returns false when outside every mapping.
bool UntranslateHostWin32Path(std::wstring_view host, std::wstring* guest);
// Guest Win32 path -> host Win32 path.
NTSTATUS TranslateGuestWin32Path(std::wstring_view guest, std::wstring* host);
// Guest "C:\x\y" -> "\Device\HarddiskVolume1\x\y" (what Windows reports for image names).
std::wstring GuestWin32ToDevicePath(std::wstring_view guest_win32);
// Win32 path of an open file, empty on failure (*error then holds the Win32
// error). Fails routinely inside the console's AppContainer: callers must never
// treat "path unknown" as "path equal".
std::wstring HostPathOfHandle(HANDLE file, DWORD* error = nullptr);
void SetFallbackDrive(wchar_t letter, const std::wstring& host_root);   // no-vfs builds only

// ---- Image sections and views (nt_section.cpp) ---------------------------------
struct ImageSection {
    std::vector<std::uint8_t> bytes;
    // Win32 path of the file on the host. EMPTY whenever the host cannot tell:
    // GetFinalPathNameByHandleW fails inside the console's AppContainer. Never
    // compare two of these without checking for empty first (SameImageFile).
    std::wstring host_path;
    unsigned long host_path_error = 0;   // why host_path is empty (Win32 error), else 0
    std::wstring guest_path;    // Win32 guest view ("C:\..."), may be empty
    std::wstring name;          // export-directory name of a DLL ("kernel32.dll"), empty for EXEs
    std::uint8_t info[0x40];    // SECTION_IMAGE_INFORMATION as Windows fills it
    std::uint32_t size_of_image = 0;
};
NTSTATUS CreateImageSectionFromFile(HANDLE file, std::shared_ptr<ImageSection>* out);
// NtAreMappedFilesTheSame semantics for two image sections. Wine's
// find_existing_module() treats "same" as "this DLL is already loaded": it
// unmaps the new view and hands back the existing module, so a false positive
// aliases one DLL to another (kernel32.dll -> ntdll.dll on the console).
// Identity order: same section object; both host paths known -> path equality;
// otherwise (a path is unknown) -> identical size and bytes. Two unknown paths
// are NEVER treated as equal. *how says which rule decided (for the trace).
bool SameImageFile(const ImageSection& a, const ImageSection& b, const char** how = nullptr);
NTSTATUS BuildImageInformation(const std::uint8_t* file, std::size_t size, std::uint8_t out[0x40], std::uint32_t* size_of_image);

struct ImageView {
    loader::MappedImage image;
    std::shared_ptr<ImageSection> section;
    bool stubs_rewritten = false;
    std::size_t stubs = 0;
};
struct MapOptions {
    void* preferred = nullptr;       // *BaseAddress from the caller
    bool keep_writable = false;      // caller finalizes (Launch patches ntdll first)
    bool is_guest_ntdll = false;
};
// Maps an image section as a view (loader + stub rewrite + scan); records the view.
NTSTATUS MapImageView(const std::shared_ptr<ImageSection>& sec, const MapOptions& opts, ImageView** out);
NTSTATUS FinalizeView(ImageView* view);
bool FindView(const void* address, ImageView* copy_out);          // any address inside the view
bool FindViewByBase(const void* base, ImageView* copy_out);
const std::uint8_t* MainImageInfo();                               // SECTION_IMAGE_INFORMATION of the guest EXE, or null
void SetMainImage(const std::shared_ptr<ImageSection>& sec);
std::shared_ptr<ImageSection> ImageSectionFromHandle(HANDLE h);   // null if not ours
void CloseImageSection(const EmulatedObject& o);
HANDLE DuplicateImageSection(const EmulatedObject& o);
// Export-directory name of a mapped image ("ntdll.dll"), empty if none.
std::string ModuleExportName(const void* image_base);

// ---- Module-load diagnostics (channel `modules`, kept permanently) ----------------
// Short human label of a section / view ("C:\Windows\System32\kernel32.dll",
// else "kernel32.dll", else the host path, else "<unnamed image>").
std::wstring ImageLabel(const ImageSection* sec);
// The guest's own loaded-module list (Wine's PEB->Ldr, walked read-only),
// flagging ntdll/kernel32/kernelbase presence. kernelbase absent at guest entry
// is the forwarder bug this diagnostic exists to catch.
void DumpGuestModules(const char* why);
// Called by NtContinue(Ex) before it switches to the guest: when this is the
// main guest thread's launch continuation (Rip == the guest RtlUserThreadStart),
// dumps the module list once per process. The "guest entry reached" trace line
// it emits is the marker hosts watch for through trace::AddSink.
void NoteGuestEntry(const CONTEXT* ctx);
// Address of the guest's RtlUserThreadStart thunk (set in process.cpp while
// mapping the guest ntdll), so NoteGuestEntry can recognise the launch continuation.
void* GuestEntryThunk();

// ---- Guest threads / exit (process.cpp) -----------------------------------------
NTSTATUS CreateGuestThread(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PVOID start, PVOID arg,
                           ULONG flags, SIZE_T zero_bits, SIZE_T stack_size, SIZE_T max_stack, PVOID attr_list);
// Current guest thread is exiting with `code` (NtTerminateThread on itself).
[[noreturn]] void GuestThreadExit(NTSTATUS code);
void ForgetGuestThread(DWORD tid);                       // another guest thread is being terminated
void TerminateOtherGuestThreads(NTSTATUS code);          // NtTerminateProcess(NULL, code)

// ---- Emulated syscalls implemented here (exact NT signatures) -------------------
NTSTATUS NTAPI Xl_NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                               ULONG, ULONG, PVOID, ULONG);
NTSTATUS NTAPI Xl_NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
NTSTATUS NTAPI Xl_NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
NTSTATUS NTAPI Xl_NtMapViewOfSection(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
NTSTATUS NTAPI Xl_NtClose(HANDLE);

// ---- Unix calls / Wine debug (unix_calls.cpp) ------------------------------------
// Ordinals of the Wine-internal exports of the guest ntdll. wine-pe/patches/0006 exports every wine_* / __wine_*
// function by ordinal only (no name in the export table) and writes the numbers into ntdll.spec, so they are fixed.
namespace wine_ordinal {
inline constexpr std::uint16_t kUnixCallDispatcher = 1455;   // __wine_unix_call_dispatcher (data: a pointer)
inline constexpr std::uint16_t kUnixlibHandle = 1456;        // __wine_unixlib_handle (data: a uint64)
inline constexpr std::uint16_t kDbgGetChannelFlags = 1458;   // __wine_dbg_get_channel_flags
inline constexpr std::uint16_t kDbgHeader = 1459;            // __wine_dbg_header
inline constexpr std::uint16_t kDbgOutput = 1460;            // __wine_dbg_output
inline constexpr std::uint16_t kDbgStrdup = 1461;            // __wine_dbg_strdup
}  // namespace wine_ordinal
std::uint64_t NtdllUnixLibHandle();
void RegisterNtdllUnixLib();
std::uint64_t UnixLibHandleByName(std::wstring_view module_name);   // 0 if none
// Replacements patched over the guest ntdll's __wine_dbg_* exports.
struct WineDebugChannel { unsigned char flags; char name[15]; };
extern "C" unsigned char __cdecl w10x_wine_dbg_get_channel_flags(WineDebugChannel* channel);
extern "C" int __cdecl w10x_wine_dbg_header(int cls, WineDebugChannel* channel, const char* function);
extern "C" int __cdecl w10x_wine_dbg_output(const char* str);
extern "C" const char* __cdecl w10x_wine_dbg_strdup(const char* str);
// Replacement for the guest ntdll's RtlCreateHeap (the host's heap, with Wine's "no maximum means growable").
extern "C" PVOID NTAPI w10x_RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID params);

// ---- Function-level forwarding (rtl_forward.cpp) -----------------------------
struct ForwardResult { std::size_t forwarded = 0, skipped = 0; };
// Redirects listed guest ntdll exports to the host ntdll (and the Wine debug
// exports to us). The image must still be writable.
ForwardResult ApplyRtlForwarding(const loader::MappedImage& guest_ntdll);
// Writes a 5-byte rel32 jmp at `from` to `to` via a near trampoline. Exposed for tests.
bool PatchJump(void* from, const void* to);
// Adds to the guest ntdll's export table every CODE export of the host ntdll that Wine's ntdll does not have,
// each as a jump to the host function, so a native DLL's imports of them resolve instead of binding to Wine's
// raising stubs ("No implementation for ntdll.dll.X"): the native user32 imports ~20 of them. The new
// directory and the jumps live in one allocation within 4 GiB above the image (export RVAs are 32-bit).
// The image must still be writable. Returns how many names were added.
std::size_t ExtendGuestNtdllExports(const loader::MappedImage& guest_ntdll);

}  // namespace win10xbox::dispatch::detail
