// Virtual processes (D1-b): a second (third, ...) guest process that lives INSIDE the host process, started by
// CreateProcess from a program that runs under a shell (user::ShellActive()). It is a second guest view: its own
// mapping of the guest ntdll, its own PEB (TEB.ProcessEnvironmentBlock of its threads points at it while they run guest
// code), loader lists, process parameters and environment, its own pid, exit code and exit event. Shared with the
// process that started it: the handle table and every object behind it, the VFS, the registry, core/user (one window
// manager and desktop), host memory and the host's heap (the guest ntdll's heap functions are the host's, rtl_forward.inc).
// Isolation is by convention, not by the CPU: a wild pointer in one reaches the other. docs/DECISIONS.md, D1-b.
#pragma once
#include "internal.h"

namespace win10xbox::dispatch::detail {

struct VirtualProcess {
    DWORD pid = 0, parent_pid = 0;
    std::wstring image_path;                       // guest Win32 path
    std::shared_ptr<ImageSection> image;           // the EXE (NtQueryInformationProcess ProcessImageInformation)
    void* ldr_init = nullptr;                      // its own guest ntdll: LdrInitializeThunk / RtlUserThreadStart
    void* user_thread_start = nullptr;
    std::uint8_t* peb = nullptr;                   // the private PEB
    void* params = nullptr;                        // RTL_USER_PROCESS_PARAMETERS it was started with
    void* exe_base = nullptr;
    std::size_t stack_reserve = 1 << 20;
    std::vector<wchar_t> environment;
    HANDLE exit_event = nullptr;                   // manual-reset host event: the process handle's object
    volatile LONG exit_code = 259;                 // STATUS_PENDING until it exits
    volatile LONG exited = 0;
    volatile LONG tls_reserved = 0;                // ReserveGuestTlsSlot has run against this PEB
    volatile LONG entry_noted = 0;                 // NoteGuestEntry has seen the launch continuation
};

struct VirtualProcessRequest {
    std::wstring image;                            // guest Win32 path of the EXE
    std::wstring command_line, current_directory;
    std::vector<std::wstring> env;
    bool has_env = false;
    std::uintptr_t console_handle = 0;
    ULONG console_flags = 0;
    HANDLE std_handles[3] = {};
    ULONG start_x_block[8] = {};                   // StartingX .. (dwX dwY dwXSize dwYSize dwXCountChars dwYCountChars dwFillAttribute)
    ULONG start_flags = 0, show_window = 0;
    bool suspended = false;
};

// The virtual process of the calling thread (null on the initial process's threads and on host threads).
VirtualProcess* CurrentVirtualProcess();
// Starts a virtual process: maps the EXE and a second guest ntdll, builds the PEB and parameters, starts the main
// thread. *process / *thread are host event handles (the process one signals at exit; the thread one at thread exit is
// not tracked: it is a duplicate of the process event) registered so the process syscalls can find the object again.
NTSTATUS CreateVirtualProcess(const VirtualProcessRequest& req, PHANDLE process, PHANDLE thread, DWORD* pid, DWORD* tid);
VirtualProcess* VirtualProcessFromHandle(HANDLE h);
void ForgetVirtualProcessHandle(HANDLE h);                 // NtClose
void NoteDuplicatedVirtualProcessHandle(HANDLE src, HANDLE dup);
// Another process ends it: its threads are terminated, the exit event is signalled.
NTSTATUS TerminateVirtualProcess(VirtualProcess* vp, NTSTATUS code);
// The calling thread's own process is done (its last thread exited, or NtTerminateProcess(current)).
[[noreturn]] void ExitVirtualProcess(VirtualProcess* vp, NTSTATUS code);
// Threads of `vp` (null: the initial process) among the guest threads.
std::size_t CountGuestThreads(const VirtualProcess* vp);

}  // namespace win10xbox::dispatch::detail
