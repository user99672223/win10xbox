// Route B + Direct3D 11 (docs/DECISIONS.md G1). The guest runs the NATIVE d3d11/dxgi; their kernel interface is
// D3DKMT: gdi32!D3DKMTxxx -> the native win32u's NtGdiDdDDIxxx thunks, which RenumberSyscallThunks renumbers by
// name and win10xbox_dispatch passes through to the host's win32u like every other route-B call, i.e. to the
// host's dxgkrnl. Nothing here changes those calls. This file only watches them, for two things:
//
//   * evidence: the first time each NtGdiDdDDI* name passes through is traced ("native d3d: first D3DKMT call
//     NtGdiDdDDIxxx"), and a count is kept;
//   * presentation: a D3D frame reaches the window without any NtUserEndPaint, so the native composer
//     (core/user/native_compose.cpp) would never see it. After a present-type D3DKMT call (a name containing
//     "Present") the guest's windows are composed again (at most every 50 ms), and once more right BEFORE
//     NtUserDestroyWindow passes through, so the last picture of a window is kept when the program closes it.
#include "internal.h"

#include <cstring>

namespace win10xbox::dispatch::detail {
namespace {

// Namespace scope, trivially initialised; touched only from the host view inside win10xbox_dispatch.
constexpr int kMaxNames = 256;
const char* g_seen[kMaxNames];
LONG g_nseen = 0;   // under g_lock
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG64 g_ddi_calls = 0, g_present_calls = 0, g_present_composes = 0;
ULONGLONG g_last_compose_tick = 0;

bool StartsWith(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

const char* g_failed[kMaxNames];   // names whose first non-success status has been reported (under g_lock)
LONG g_nfailed = 0;

// The first call of each name, with the status the host returned; and, once per name, the first status that is not
// success (an NTSTATUS error or warning: the measurement of what the host's dxgkrnl refuses, e.g. in an AppContainer).
void NoteFirst(const char* name, std::uint64_t result) {
    const auto status = static_cast<unsigned long>(result);
    const bool bad = status >= 0x80000000ul;
    AcquireSRWLockExclusive(&g_lock);
    bool seen = false, failed_seen = !bad;
    for (LONG i = 0; i < g_nseen; ++i)
        if (g_seen[i] == name || !std::strcmp(g_seen[i], name)) { seen = true; break; }
    if (!seen && g_nseen < kMaxNames) g_seen[g_nseen++] = name;
    if (bad) {
        for (LONG i = 0; i < g_nfailed; ++i)
            if (g_failed[i] == name || !std::strcmp(g_failed[i], name)) { failed_seen = true; break; }
        if (!failed_seen && g_nfailed < kMaxNames) g_failed[g_nfailed++] = name;
    }
    const LONG n = g_nseen;
    ReleaseSRWLockExclusive(&g_lock);
    if (!seen)
        TRACE(dispatch, "native d3d: first D3DKMT call %s -> %08lx (host win32k/dxgkrnl), %ld distinct so far", name,
              status, n);
    if (!failed_seen) TRACE(dispatch, "native d3d: D3DKMT %s returned %08lx", name, status);
}

}  // namespace

void NativeWin32uBefore(const char* name) {
    if (name[2] == 'U' && !std::strcmp(name, "NtUserDestroyWindow")) NativeGuestPainted();
}

void NativeWin32uAfter(const char* name, std::uint64_t result) {
    if (!StartsWith(name, "NtGdiDdDDI")) return;
    InterlockedIncrement64(&g_ddi_calls);
    NoteFirst(name, result);
    if (!std::strstr(name, "Present")) return;
    const LONG64 n = InterlockedIncrement64(&g_present_calls);
    const ULONGLONG now = GetTickCount64();
    if (n > 1 && now - g_last_compose_tick < 50) return;
    g_last_compose_tick = now;
    const LONG64 c = InterlockedIncrement64(&g_present_composes);
    if (c == 1)
        TRACE(dispatch, "native d3d: %s -> %08lx: composing the guest's windows after presents", name,
              static_cast<unsigned long>(result));
    NativeGuestPainted();
}

void NoteHostD3dkmtCall(const char* name, std::uint64_t result) {
    InterlockedIncrement64(&g_ddi_calls);
    NoteFirst(name, result);
}

std::uint64_t NativeD3dkmtCalls() { return static_cast<std::uint64_t>(g_ddi_calls); }

}  // namespace win10xbox::dispatch::detail
