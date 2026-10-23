// Route B (config gui = Native, docs/GUI.md "Route B: native win32k"): the guest runs the native
// user32/gdi32/gdi32full/win32u, every win32u syscall passes through to the host's own win32u.dll export of
// the same name (dispatcher.cpp: RenumberSyscallThunks, UseHostWin32u), so the host's win32k manages the
// guest's windows. This file loads the host's GUI DLLs and routes win32k's user-mode callbacks.
//
// Callbacks. win32k calls back through the HOST ntdll's KiUserCallbackDispatcher, which calls
// PEB.KernelCallbackTable[index](args, len) on the thread that made the syscall. That thread is inside our
// dispatcher, in the HOST TLS view (ThreadState::depth >= 1), while the guest's user32 -- which installed its
// own table in the PEB -- expects to run in the guest view. And the PEB table is process-wide: a host thread
// with windows of its own (host-uwp's CoreWindow) must keep getting the HOST user32. So once the guest's user32
// has written its table, the PEB gets our trampoline table instead; trampoline i routes per thread:
//   guest thread  -> CallInGuestView(guest_table[i], args, len)
//   other thread  -> host_table[i](args, len), unchanged
// The invariant that makes the guest case correct is written down in docs/DISPATCHER.md ("User-mode
// callbacks in native GUI mode").
#include "internal.h"
#include <array>
#include <cstring>
#include <utility>

namespace win10xbox::dispatch::detail {
namespace {

using KernelCallbackFn = NTSTATUS(NTAPI*)(void* args, ULONG len);

// More than any user32 has (Windows 10/11 user32's apfnDispatch has well under 256 entries). The trampolines
// do not need the guest table's real length: the kernel only ever passes indices valid for it.
constexpr unsigned kCallbackSlots = 512;

// Namespace scope and trivially initialised: read by the trampolines on whatever thread win32k calls back.
KernelCallbackFn* volatile g_guest_callbacks = nullptr;   // the guest's user32 table (PEB, after its DllMain)
KernelCallbackFn* g_host_callbacks = nullptr;             // the host's user32 table (PEB, after LoadLibrary)
volatile LONG64 g_to_guest = 0, g_to_host = 0;
HostGui g_host_gui{};
bool g_host_gui_ok = false;

NTSTATUS RouteCallback(unsigned index, void* args, ULONG len) {
    ThreadState* st = CurrentThreadState();
    KernelCallbackFn* const guest = g_guest_callbacks;
    if (!st || !(st->flags & kThreadGuest) || !guest) {
        // Not a guest thread (or the guest has no table yet): the host's user32, as if we were not here.
        InterlockedIncrement64(&g_to_host);
        return g_host_callbacks[index](args, len);
    }
    const auto n = static_cast<unsigned long long>(InterlockedIncrement64(&g_to_guest));
    // depth 0 would mean the guest view is already installed (a callback that did not come through our
    // dispatcher): nothing host-side may log there, and CallInGuestView below then switches nothing.
    if (st->depth)
        TRACE(dispatch, "user-mode callback #%llu: KernelCallbackTable[%u] -> guest user32 %p (args %p, %lu bytes)",
              n, index, reinterpret_cast<void*>(guest[index]), args, len);
    // Resolved before the view switch: CallInGuestView's contract. If the callback ends in NtCallbackReturn
    // (the normal case), that syscall unwinds this frame away in the kernel and never returns here.
    const KernelCallbackFn fn = guest[index];
    return CallInGuestView(fn, args, len);
}

template <unsigned I> NTSTATUS NTAPI Trampoline(void* args, ULONG len) { return RouteCallback(I, args, len); }

template <unsigned... I>
constexpr std::array<KernelCallbackFn, sizeof...(I)> MakeTrampolines(std::integer_sequence<unsigned, I...>) {
    return {{&Trampoline<I>...}};
}
// Constant-initialised (no dynamic init, no guard): PEB.KernelCallbackTable points straight at it.
constexpr std::array<KernelCallbackFn, kCallbackSlots> kTrampolines =
    MakeTrampolines(std::make_integer_sequence<unsigned, kCallbackSlots>{});

void* TrampolineTable() { return const_cast<KernelCallbackFn*>(kTrampolines.data()); }

// ---- The ntdll side of the native user32 (patched over the guest ntdll's exports in native mode) ------
// The native user32 registers its client window procedures with RtlInitializeNtUserPfn, and its DefWindowProcW /
// DefDlgProcW are forwarders to ntdll!NtdllDefWindowProc_W / NtdllDialogWndProc_W, which jump through those
// tables. Wine's ntdll has all of them, but sized for Wine's user32: its RtlInitializeNtUserPfn refuses the
// larger tables of Windows' user32 with STATUS_INVALID_PARAMETER (17 procedures vs 23 on Windows 10/11). These
// replacements keep whatever size user32 hands in. They run in the GUEST view (called by guest code, no stub in
// between): plain code over namespace-scope arrays, no TLS, no logging, no host state.
using ClientProc = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
constexpr std::size_t kMaxClientProcs = 64;
ClientProc g_procs_a[kMaxClientProcs], g_procs_w[kMaxClientProcs], g_workers[kMaxClientProcs];
volatile LONG g_procs_set = 0;
bool g_procs_reported = false;
std::uint32_t g_procs_bytes[3] = {};
// Indices in Windows' PFNCLIENT (the same in Wine's ntuser.h): pfnDefWindowProc 4, pfnDialogWndProc 10.
constexpr std::size_t kDefWindowProc = 4, kDialogWndProc = 10;

NTSTATUS NTAPI Native_RtlInitializeNtUserPfn(const void* a, ULONG a_size, const void* w, ULONG w_size,
                                             const void* workers, ULONG workers_size) {
    if (a_size > sizeof g_procs_a || w_size > sizeof g_procs_w || workers_size > sizeof g_workers ||
        (a_size | w_size | workers_size) % sizeof(void*))
        return st::InvalidParameter;
    if (InterlockedCompareExchange(&g_procs_set, 1, 0) != 0) return st::InvalidParameter;   // once, like Windows
    std::memcpy(g_procs_a, a, a_size);
    std::memcpy(g_procs_w, w, w_size);
    std::memcpy(g_workers, workers, workers_size);
    g_procs_bytes[0] = a_size; g_procs_bytes[1] = w_size; g_procs_bytes[2] = workers_size;
    return st::Success;
}
NTSTATUS NTAPI Native_RtlRetrieveNtUserPfn(const void** a, const void** w, const void** workers) {
    if (!g_procs_set) return st::InvalidParameter;
    *a = g_procs_a; *w = g_procs_w; *workers = g_workers;
    return st::Success;
}
NTSTATUS NTAPI Native_RtlResetNtUserPfn() {
    if (!g_procs_set) return st::InvalidParameter;
    std::memset(g_procs_a, 0, sizeof g_procs_a);
    std::memset(g_procs_w, 0, sizeof g_procs_w);
    std::memset(g_workers, 0, sizeof g_workers);
    g_procs_set = 0;
    return st::Success;
}
LRESULT WINAPI Native_NtdllDefWindowProc_A(HWND h, UINT m, WPARAM wp, LPARAM lp) { return g_procs_a[kDefWindowProc](h, m, wp, lp); }
LRESULT WINAPI Native_NtdllDefWindowProc_W(HWND h, UINT m, WPARAM wp, LPARAM lp) { return g_procs_w[kDefWindowProc](h, m, wp, lp); }
LRESULT WINAPI Native_NtdllDialogWndProc_A(HWND h, UINT m, WPARAM wp, LPARAM lp) { return g_procs_a[kDialogWndProc](h, m, wp, lp); }
LRESULT WINAPI Native_NtdllDialogWndProc_W(HWND h, UINT m, WPARAM wp, LPARAM lp) { return g_procs_w[kDialogWndProc](h, m, wp, lp); }

// CsrClientConnectToServer: the native user32's DllMain connects to csrss's user server; Wine's ntdll has only a
// stub that raises. The host ntdll holds this process's real csrss connection. Called from guest code, so the
// host call runs under HostModeScope (host view), which also makes the trace line safe.
using PFN_CsrConnect = NTSTATUS(NTAPI*)(PCWSTR, ULONG, PVOID, PULONG, PBOOLEAN);
PFN_CsrConnect g_host_csr_connect = nullptr;
NTSTATUS NTAPI Native_CsrClientConnectToServer(PCWSTR dir, ULONG server, PVOID info, PULONG info_size,
                                               PBOOLEAN server_to_server) {
    // The 4th/5th arguments are passed on untouched and never dereferenced here: this build's user32 passes the
    // connection-info size by value (0x248), not as a PULONG, and the host ntdll of the same build takes it that way.
    HostModeScope scope;
    const NTSTATUS s = g_host_csr_connect ? g_host_csr_connect(dir, server, info, info_size, server_to_server)
                                          : st::NotImplemented;
    TRACE(dispatch, "CsrClientConnectToServer(server %lu, arg4 %p, arg5 %p) -> %08lx (host ntdll)", server,
          static_cast<void*>(info_size), static_cast<void*>(server_to_server), static_cast<unsigned long>(s));
    return s;
}

}  // namespace

std::vector<NamedTarget> NativeUserOverrides() {
    g_host_csr_connect = reinterpret_cast<PFN_CsrConnect>(HostExport("CsrClientConnectToServer"));
#define P(name) {#name, reinterpret_cast<const void*>(&Native_##name)}
    return {P(CsrClientConnectToServer), P(RtlInitializeNtUserPfn), P(RtlRetrieveNtUserPfn), P(RtlResetNtUserPfn),
            P(NtdllDefWindowProc_A), P(NtdllDefWindowProc_W), P(NtdllDialogWndProc_A), P(NtdllDialogWndProc_W)};
#undef P
}

void NoteNativeUserPfn() {
    if (g_procs_set)
        TRACE(dispatch, "RtlInitializeNtUserPfn: the guest's user32 registered %u/%u/%u bytes of client procedures "
                        "(DefWindowProcW %p)", g_procs_bytes[0], g_procs_bytes[1], g_procs_bytes[2],
              reinterpret_cast<void*>(g_procs_w[kDefWindowProc]));
}

std::uint64_t NativeCallbacksToGuest() { return static_cast<std::uint64_t>(g_to_guest); }
std::uint64_t NativeCallbacksToHost() { return static_cast<std::uint64_t>(g_to_host); }
const HostGui* HostGuiFunctions() { return g_host_gui_ok ? &g_host_gui : nullptr; }

void NoteNativeWin32uCall() {
    if (g_procs_set && !g_procs_reported) {
        g_procs_reported = true;
        NoteNativeUserPfn();
    }
    auto& slot = PebField<KernelCallbackFn*>(peb::KernelCallbackTable);
    KernelCallbackFn* const cur = slot;
    if (!cur || cur == TrampolineTable() || cur == g_host_callbacks) return;
    // The guest's user32 (its DllMain) has installed its own table: same layout as the host's, other base.
    KernelCallbackFn* const previous = g_guest_callbacks;
    g_guest_callbacks = cur;
    slot = static_cast<KernelCallbackFn*>(TrampolineTable());
    TRACE(dispatch, "PEB.KernelCallbackTable: the guest's user32 installed %p (previous guest table %p, host's %p); "
                    "replaced by the %u trampolines at %p, routed per thread", static_cast<void*>(cur),
          static_cast<void*>(previous), static_cast<void*>(g_host_callbacks), kCallbackSlots, TrampolineTable());
}

NTSTATUS ConfigureNativeGui() {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    HMODULE gdi32 = LoadLibraryW(L"gdi32.dll");
    HMODULE win32u = LoadLibraryW(L"win32u.dll");
    if (!user32 || !gdi32 || !win32u) {
        ERR(dispatch, "gui native: the host's user32/gdi32/win32u did not load (%p %p %p, error %lu)",
            static_cast<void*>(user32), static_cast<void*>(gdi32), static_cast<void*>(win32u), GetLastError());
        return st::DllNotFound;
    }
    HostGui& h = g_host_gui;
    bool ok = true;
#define R(mod, fn) ok &= (h.fn = reinterpret_cast<decltype(h.fn)>(GetProcAddress(mod, #fn))) != nullptr
    R(user32, EnumWindows); R(user32, GetWindowThreadProcessId); R(user32, IsWindowVisible); R(user32, IsIconic);
    R(user32, GetWindowRect); R(user32, GetWindowDC); R(user32, ReleaseDC); R(user32, GetSystemMetrics);
    R(gdi32, CreateCompatibleDC); R(gdi32, CreateDIBSection); R(gdi32, SelectObject); R(gdi32, BitBlt);
    R(gdi32, DeleteDC); R(gdi32, DeleteObject); R(gdi32, GdiFlush);
#undef R
    auto get_desktop = reinterpret_cast<HWND(WINAPI*)()>(GetProcAddress(user32, "GetDesktopWindow"));
    if (!ok || !get_desktop) {
        ERR(dispatch, "gui native: the host's user32/gdi32 lack an export the composer needs");
        return st::NotImplemented;
    }
    // The host's user32 must have connected this process (and this thread) to win32k before the guest's first
    // GUI call: a harmless call does that, and its result is the first evidence that win32k is reachable here.
    const HWND desktop = get_desktop();
    const int cx = h.GetSystemMetrics(SM_CXSCREEN), cy = h.GetSystemMetrics(SM_CYSCREEN);
    // Warm the composer's GDI path now, while PEB->Ldr is the host's: anything the host's gdi32 resolves lazily
    // (delay loads) must not be resolved against the guest's module list later.
    if (HDC mem = h.CreateCompatibleDC(nullptr)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 1;
        bi.bmiHeader.biHeight = -1;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        if (HBITMAP bmp = h.CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
            HGDIOBJ old = h.SelectObject(mem, bmp);
            if (HDC wdc = h.GetWindowDC(desktop)) {
                h.BitBlt(mem, 0, 0, 1, 1, wdc, 0, 0, SRCCOPY);
                h.ReleaseDC(desktop, wdc);
            }
            h.GdiFlush();
            h.SelectObject(mem, old);
            h.DeleteObject(bmp);
        }
        h.DeleteDC(mem);
    }
    g_host_callbacks = PebField<KernelCallbackFn*>(peb::KernelCallbackTable);
    g_host_gui_ok = true;
    TRACE(dispatch, "gui native: host user32 %p gdi32 %p win32u %p; desktop window %p, screen %dx%d, host "
                    "KernelCallbackTable %p", static_cast<void*>(user32), static_cast<void*>(gdi32),
          static_cast<void*>(win32u), static_cast<void*>(desktop), cx, cy, static_cast<void*>(g_host_callbacks));
    if (!g_host_callbacks) {
        ERR(dispatch, "gui native: the host's user32 left PEB.KernelCallbackTable empty");
        return st::Unsuccessful;
    }
    return UseHostWin32u(win32u);
}

// Emulated GUI + "d3dkmt": "host" (docs/slices/swapchain.md). The guest's windows stay core/user's; only the D3DKMT
// calls of the native dxgi reach the host's win32k, so the process must be connected to it exactly as in route B
// (the host's user32 loaded and used before the PEB take-over), the host's user-mode callbacks must keep reaching
// the host's user32 (the trampolines, installed by NoteNativeWin32uCall once the guest's user32 has its own table
// in), and the host windows the swap chains present into live on a thread started here, before the take-over
// (a host thread created afterwards would initialise against the guest's loader lists).
NTSTATUS ConfigureHostD3dkmt() {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    HMODULE gdi32 = LoadLibraryW(L"gdi32.dll");
    HMODULE win32u = LoadLibraryW(L"win32u.dll");
    if (!user32 || !gdi32 || !win32u) {
        ERR(dispatch, "d3dkmt host: the host's user32/gdi32/win32u did not load (%p %p %p, error %lu)",
            static_cast<void*>(user32), static_cast<void*>(gdi32), static_cast<void*>(win32u), GetLastError());
        return st::DllNotFound;
    }
    auto get_desktop = reinterpret_cast<HWND(WINAPI*)()>(GetProcAddress(user32, "GetDesktopWindow"));
    if (!get_desktop) return st::NotImplemented;
    const HWND desktop = get_desktop();   // connects this process to win32k
    g_host_callbacks = PebField<KernelCallbackFn*>(peb::KernelCallbackTable);
    TRACE(dispatch, "d3dkmt host: host user32 %p gdi32 %p win32u %p, desktop %p, KernelCallbackTable %p",
          static_cast<void*>(user32), static_cast<void*>(gdi32), static_cast<void*>(win32u),
          static_cast<void*>(desktop), static_cast<void*>(g_host_callbacks));
    if (!g_host_callbacks) {
        ERR(dispatch, "d3dkmt host: the host's user32 left PEB.KernelCallbackTable empty");
        return st::Unsuccessful;
    }
    if (NTSTATUS s = StartHostWindowThread(user32, gdi32)) return s;
    return UseHostD3dkmt(win32u);
}

}  // namespace win10xbox::dispatch::detail
