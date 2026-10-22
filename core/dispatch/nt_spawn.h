// Process creation (D1) and the refusals around it: internal interface of core/dispatch/nt_spawn.cpp.
// Not a public header: core/dispatch/*.cpp and tests/unit/test_syscalls_process.cpp include it.
#pragma once
#include "internal.h"

namespace win10xbox::dispatch::detail {

// ---- Syscall implementations (exact NT signatures; wired in nt_process.cpp's ProcessImpls) -------------
NTSTATUS NTAPI Xl_NtCreateUserProcess(PHANDLE process_handle, PHANDLE thread_handle, ACCESS_MASK process_access,
                                      ACCESS_MASK thread_access, POBJECT_ATTRIBUTES process_attr,
                                      POBJECT_ATTRIBUTES thread_attr, ULONG process_flags, ULONG thread_flags,
                                      PVOID params, PVOID create_info, PVOID attribute_list);
NTSTATUS NTAPI Xl_NtCreateProcessEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG, HANDLE, HANDLE, HANDLE,
                                    ULONG);
NTSTATUS NTAPI Xl_NtCreateThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PCLIENT_ID, PCONTEXT, PVOID,
                                 BOOLEAN);
NTSTATUS NTAPI Xl_NtCreateDebugObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
NTSTATUS NTAPI Xl_NtDebugActiveProcess(HANDLE, HANDLE);
NTSTATUS NTAPI Xl_NtSetSystemInformation(ULONG klass, PVOID info, ULONG length);

// ---- Child processes (Xl_NtCreateUserProcess) -------------------------------------------------------------
// Captures what must be read from the HOST view of the process (the host executable's path, the host kernel32
// entry points) before the PEB take-over replaces the loader lists: GetModuleFileNameW(NULL) and
// GetModuleHandleW(L"kernel32.dll") answer with the guest's afterwards. Called from dispatch::Initialize.
void PrimeChildProcesses();
// Test hook: run `exe` instead of the host executable when spawning (the tests use a plain guest program as
// the "host", which ignores its command line). Empty restores GetModuleFileNameW(NULL).
void SetChildHostExecutableForTest(const std::wstring& exe);

// Appends `arg` to `out` quoted so that CommandLineToArgvW / the CRT's wmain parsing give back exactly `arg`.
void AppendQuotedArgument(std::wstring* out, std::wstring_view arg);
// The host command line for a child: `"<host exe>" --machine <root> [--cwd <dir>] [--guest-env] --cmdline <raw>
// <guest image>`, which host-win32's main turns back into process::LaunchOptions. `guest_cmdline` is the child's
// raw guest command line, handed over verbatim (program-visible quoting is not reconstructed from argv).
std::wstring BuildChildCommandLine(const std::wstring& host_exe, const std::wstring& machine_root,
                                   const std::wstring& guest_image, const std::wstring& guest_cmdline,
                                   const std::wstring& guest_cwd, bool pass_environment);
// The child's HOST environment block (NAME=value\0...\0\0): the guest's entries, then the host-owned variables
// (names starting WIN10XBOX_ or WINE, which configure the host process and are never the guest's), taken from
// `host_env`. Guest entries with a host-owned name are dropped. host-win32's `--guest-env` is the inverse.
std::vector<wchar_t> BuildChildEnvironmentBlock(const std::vector<std::wstring>& guest_env,
                                                const std::vector<std::wstring>& host_env);
// True for names the host process owns (WIN10XBOX_*, WINE*), compared case-insensitively.
bool IsHostOwnedVariable(std::wstring_view entry);
// The entries of the HOST process's environment block ("NAME=value"), read from the host's own process parameters
// (the PEB's block is the guest's once Launch has taken the PEB over).
std::vector<std::wstring> HostEnvironmentEntries();

// ---- NtSetSystemInformation -----------------------------------------------------------------------------
// What NtSetSystemInformation does with a class: passes it to the host kernel, or answers
// STATUS_PRIVILEGE_NOT_HELD itself. `*name` receives a label for traces (nullptr when the class is unknown).
enum class SetSystemInfoAction { PassThrough, PrivilegeNotHeld };
SetSystemInfoAction ClassifySetSystemInformation(ULONG klass, const char** name = nullptr,
                                                 const char** privilege = nullptr);

}  // namespace win10xbox::dispatch::detail
