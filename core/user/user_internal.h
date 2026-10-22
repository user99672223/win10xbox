// Internal interfaces of core/user (the win32k surface). Not a public header:
// only core/user/*.cpp and its unit tests include it. docs/GUI.md is the design.
#pragma once
#include "win10xbox/user.h"
#include "win10xbox/trace.h"
#include "../dispatch/internal.h"   // ThreadState, CallInGuestView, PebField, st::, teb::/peb:: offsets
#include "winexbox/driver.h"

#include <cstddef>
#include <cstdint>
#include <vector>

WIN10XBOX_EXTERN_CHANNEL(user);   // window manager / messages (WIN10XBOX_DEBUG=+user)
WIN10XBOX_EXTERN_CHANNEL(gdi);    // GDI objects / drawing (+gdi)

namespace win10xbox::user::detail {

// ---- The lock ---------------------------------------------------------------
// One lock for the whole module, like wineserver's: windows, classes, queues,
// GDI handle table and surfaces. Held for the duration of a syscall, released
// while a thread blocks in GetMessage / MsgWaitForMultipleObjects and while a
// user-mode callback runs (a window procedure makes syscalls of its own).
SRWLOCK& UserLock();
class LockGuard {
public:
    LockGuard() { AcquireSRWLockExclusive(&UserLock()); }
    ~LockGuard() { ReleaseSRWLockExclusive(&UserLock()); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

// ---- Per-thread state ----------------------------------------------------------
// Lives behind TEB.Win32ClientInfo[61]; the first bytes of Win32ClientInfo are
// user32's `struct ntuser_thread_info` (ntuser.h), written by the window
// manager (message_time, message_pos, top_window, dpi_context, ...).
struct CallbackFrame {
    std::vector<std::uint8_t> result;   // what NtCallbackReturn copied
    NTSTATUS status = 0;
    bool returned = false;
};
struct UserThread {
    DWORD tid = 0;
    std::vector<CallbackFrame> callbacks;   // nesting stack, index = depth
    std::uint32_t callback_depth = 0;
    void* queue = nullptr;                  // the window manager's per-thread message queue (win_message.cpp)
};
// The calling thread's record, created on first use.
UserThread& CurrentUserThread();
// NULL when this thread never touched the module.
UserThread* FindUserThread();

// ---- User-mode callbacks (KeUserModeCallback, in-process) ----------------------
// Calls PEB.KernelCallbackTable[id](args, len) -- a user32 entry point -- with
// the thread in the GUEST TLS view (the procedure is guest code and makes
// syscalls, which nest normally). The callback ends with NtCallbackReturn
// (emulated here: the result bytes are copied into the thread's CallbackFrame
// before the procedure's stack frame is gone); *ret_ptr / *ret_len then point at
// that copy, valid until the next callback at the same nesting depth on this
// thread. Returns the status NtCallbackReturn was given, or STATUS_NOT_IMPLEMENTED
// when user32 has not installed its table yet. Call with the user lock
// RELEASED.
NTSTATUS UserModeCallback(ULONG id, const void* args, ULONG len, void** ret_ptr, ULONG* ret_len);

// Syscall: NtCallbackReturn(result, size, status).
NTSTATUS NTAPI User_NtCallbackReturn(void* result, ULONG size, NTSTATUS status);

// Which win32u syscall is being emulated right now (for the unimplemented counter / FIXMEs).
void NoteUnimplemented(const char* name);

}  // namespace win10xbox::user::detail
