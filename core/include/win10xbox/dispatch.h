// win10xbox dispatcher: every guest syscall lands here. Owned by core/dispatch/.
//
// Entry: the loader rewrites each guest ntdll/win32u thunk so that after
// `mov r10,rcx ; mov eax,<id>` it jumps to SyscallEntry (core/asm). The asm
// stub saves the guest state, builds a Frame (id, pointer to the stack args,
// register args) and calls Dispatch(). The table is generated into
// core/dispatch/syscall_table.gen.inc from Wine's ntsyscalls.h (ids are the
// real Windows 10 x64 numbers) and the per-syscall policy is hand-maintained
// in core/dispatch/syscall_policy.inc:
//
//   PassThrough : call the host ntdll export of the same name, args unchanged
//   Translate   : per-syscall translator rewrites args (paths/handles/time),
//                 calls the host, fixes up results
//   Emulate     : user-mode implementation (never touches the host resource)
//   Todo        : trace loudly, return STATUS_NOT_IMPLEMENTED
//
// Every call is traced on the `relay` channel with args + NTSTATUS.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <string>
#include <vector>
#include "win10xbox/loader.h"

namespace win10xbox::dispatch {

enum class Policy : std::uint8_t { PassThrough, Translate, Emulate, Todo };

// Register state captured by the asm entry stub. Argument order matches the
// x64 calling convention: rcx(=r10), rdx, r8, r9, then stack args.
struct Frame {
    std::uint32_t id;          // syscall id from eax
    std::uint32_t nargs;       // from the table (0 if unknown)
    std::uint64_t* args;       // args[0..nargs): a contiguous copy, rcx first
    std::uint64_t  return_address;
    std::uint64_t  guest_rsp;  // rsp at the thunk's `jmp` (points at return address)
};

struct Entry {
    std::uint32_t id;
    const char*   name;        // "NtCreateFile"
    std::uint32_t nargs;       // argument count (not bytes)
    Policy        policy;
    void*         host_fn;     // resolved host ntdll export (PassThrough/Translate), may be null
    void*         impl;        // our implementation (Translate/Emulate): same signature as the syscall
    const char*   note;        // from syscall_policy.inc, shown in SYSCALLS.md
    std::uint64_t calls;       // relay statistics
};

// Initialise: resolve host ntdll exports by name, build the table, generate
// the entry stubs. Idempotent. The host ntdll module is the one the process
// was started with (GetModuleHandleW(L"ntdll.dll")).
NTSTATUS Initialize();
// Dumps per-syscall call counts on the `relay` channel (when enabled).
// Idempotent; the table stays usable afterwards.
void Shutdown();
const Entry* Lookup(std::uint32_t id);
const Entry* LookupByName(const char* name);
std::size_t Count();
// Runtime override from machine.json "syscall_overrides": {"NtFoo":"passthrough"}.
NTSTATUS SetPolicy(const char* name, Policy p);
// "passthrough"/"PassThrough"/"translate"/"emulate"/"todo" (case-insensitive).
bool ParsePolicy(const char* text, Policy* out);
const char* PolicyName(Policy p);

// Address the loader patches into every thunk. Reads eax/r10 like the kernel.
void* SyscallEntryAddress();
// C-level dispatcher called by the asm stub. Returns the NTSTATUS to place in rax.
extern "C" std::int64_t win10xbox_dispatch(Frame* frame);
// Called by the asm stub before copying stack arguments: nargs for `id`, at most kMaxArgs.
extern "C" std::uint32_t win10xbox_syscall_nargs(std::uint32_t id);
constexpr std::uint32_t kMaxArgs = 20;

// Calls `fn` (an NT-style function) with args[0..nargs) and returns its NTSTATUS.
// Missing trailing args are passed as 0 (x64: the callee ignores extras).
NTSTATUS CallWithArgs(void* fn, const std::uint64_t* args, std::uint32_t nargs);
// Same call, keeping the whole of RAX: the win32u surface returns HWNDs, LRESULTs,
// LONG_PTRs and pointers, not NTSTATUS (NtUserMessageCall, NtUserCallOneParam...).
std::uint64_t CallWithArgs64(void* fn, const std::uint64_t* args, std::uint32_t nargs);

// Relay line body as traced on the `relay` channel:
//   NtCreateFile(0000000000abcd00,00100080,...) ret=00000000
// with " (12.3us)" appended when elapsed_us >= 0 (+timing). Exposed for tests.
std::string FormatRelay(const Entry& e, const std::uint64_t* args, NTSTATUS status, double elapsed_us = -1.0);

// ---- Guest/host thread view -------------------------------------------------
// A guest thread runs with the guest ntdll's static-TLS array and FLS data in
// its TEB (Wine's loader_init installs them). Host code on that thread (the
// dispatcher, VEH handlers, scanner site handlers, functions patched into
// guest DLLs) must run with the host's values instead, or host C++ runtime
// TLS/FLS (MSVC magic statics, CRT per-thread data, libc++abi EH globals)
// would read guest data. SyscallEntry switches automatically; any other host
// entry point reachable from guest code uses this guard. Cheap (four TEB
// moves), nests, and is a no-op on threads that never ran guest code.
class HostModeScope {
public:
    HostModeScope();
    ~HostModeScope();
    HostModeScope(const HostModeScope&) = delete;
    HostModeScope& operator=(const HostModeScope&) = delete;
private:
    void* state_;
    unsigned long last_error_;   // TEB LastErrorValue / LastStatusValue are shared with the
    long last_status_;           // guest and restored on exit (syscalls never change them)
};

// ---- CPU identity seen through patched cpuid sites (D10) ---------------------
struct CpuPolicy {
    std::string vendor;            // 12 chars ("AuthenticAMD"); empty = the real vendor
    bool hide_hypervisor = true;   // clear CPUID.1:ECX[31], zero leaves 0x40000000..0x400000FF
};
void SetCpuPolicy(const CpuPolicy& p);
CpuPolicy GetCpuPolicy();
// Scanner handler (loader::SiteHandler) used for every guest image we map.
bool CpuSiteHandler(loader::SiteKind kind, loader::ScanRegs* regs, void* ctx);

// ---- Emulated object handles -----------------------------------------------
// Emulated objects (registry keys, tokens, sections we map ourselves, driver
// devices) get guest HANDLE values from one table so NtClose/NtDuplicateObject/
// NtQueryObject can route them. Host handles are passed through unchanged.
// Guest-visible handle values for emulated objects are host event handles
// (so waits and inheritance behave) tagged in this table.
enum class ObjectKind : std::uint8_t { RegistryKey, Token, ImageSection, Device, Directory, Other };
struct EmulatedObject { ObjectKind kind; std::uint64_t id; void* extra; };
HANDLE RegisterEmulatedObject(ObjectKind kind, std::uint64_t id, void* extra = nullptr);
bool   LookupEmulatedObject(HANDLE h, EmulatedObject* out);
void   UnregisterEmulatedObject(HANDLE h);

// ---- Wine unix-call providers --------------------------------------------
// Wine's PE ntdll calls its unix side through __wine_unix_call_dispatcher with
// the enum in dlls/ntdll/unixlib.h (load_so_dll, unwind_builtin_dll,
// wine_dbg_write, wine_server_call, wine_server_fd_to_handle,
// wine_server_handle_to_fd, wine_spawnvp, system_time_precise). We provide
// them; other builtin DLLs with unix libs (win32u, ws2_32...) register their
// own tables here by module name.
using UnixCallFn = NTSTATUS(WINAPI*)(void* args);
void RegisterUnixLib(const wchar_t* module_name, const UnixCallFn* table, std::size_t count);
NTSTATUS WINAPI UnixCallDispatcher(std::uint64_t handle, unsigned int code, void* args);

// ---- Host ntdll ------------------------------------------------------------
HMODULE HostNtdll();
void*   HostExport(const char* name);   // GetProcAddress on the host ntdll, cached

}  // namespace win10xbox::dispatch

// ---- Process bootstrap -----------------------------------------------------
namespace win10xbox::process {

struct LaunchOptions {
    std::wstring image_path;        // guest Win32 path, e.g. L"C:\\Program Files\\app\\app.exe"
    std::wstring command_line;      // full guest command line (image first)
    std::wstring current_directory; // guest path, default C:\Users\<user>
    std::vector<std::wstring> environment;  // "NAME=value"; empty = default guest environment
    bool run_on_new_thread = true;  // UWP host needs the calling thread back
    bool wait = true;               // wait for guest exit, return its exit code
};

// Maps the Wine ntdll from the machine folder, applies stub rewriting and
// Rtl-forwarding to the host ntdll, builds the guest PEB view (image base,
// process parameters, version), then enters LdrInitializeThunk with a
// context targeting RtlUserThreadStart(entry, peb). Returns the guest exit
// code when `wait`, else 0; negative NTSTATUS-like values on bootstrap failure.
// With run_on_new_thread == false the calling thread becomes the guest main
// thread and never returns: the host process exits with the guest's code.
// Machine folder layout (see docs/DISPATCHER.md):
//   <machine_root>\Windows\builtin\ntdll.dll, kernelbase.dll, kernel32.dll ...  (Wine PE)
//   <machine_root>\Windows\System32\*.dll                                     (native)
int Launch(const LaunchOptions& opts);
// Guest ExitProcess lands here (NtTerminateProcess on the current process).
[[noreturn]] void GuestExit(std::uint32_t exit_code);
std::uint32_t GuestExitCode();
bool IsGuestThread();   // true on threads running guest code (TLS flag set by Launch/thread start)
// Waits for the guest started by Launch(wait=false); returns its exit code.
int WaitForGuest(DWORD timeout_ms = INFINITE);
// Guest path of the main image ("C:\\..."), empty before Launch.
std::wstring GuestImagePath();
// A child started by a guest (NtCreateUserProcess) is this same host program with `--guest-env`: its guest
// environment arrives as its own process environment, behind the WIN10XBOX_* / WINE* variables that configure the
// host process. Returns the entries for LaunchOptions::environment, host-owned variables removed.
std::vector<std::wstring> InheritedGuestEnvironment();
// Makes host directory `host_dir` visible to the guest as drive `letter`
// for this run (e.g. host-win32 running an EXE outside the machine folder).
// Returns false when the VFS module owns drive mappings and has no runtime
// API for it (then only machine.json "drives" apply).
bool MapHostDirectory(wchar_t letter, const std::wstring& host_dir);

}  // namespace win10xbox::process
