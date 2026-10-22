// Process bootstrap (process::Launch), guest threads and guest exit.
// Sequence and PEB take-over are documented in docs/DISPATCHER.md.
#include "internal.h"
#include "nt_spawn.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_VFS
#include "win10xbox/vfs.h"
#endif
#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#if WIN10XBOX_HAVE_TOKEN
#include "win10xbox/token.h"
#endif
#include <algorithm>
#include <cstring>
#include <cwctype>

namespace win10xbox::process {
namespace {

using namespace win10xbox::dispatch;
using namespace win10xbox::dispatch::detail;

// RTL_USER_PROCESS_PARAMETERS (x64) offsets; MSVC's winternl.h hides most fields.
namespace pp {
constexpr std::uint32_t ConsoleHandle = 0x10, ConsoleFlags = 0x18, StdInput = 0x20, StdOutput = 0x28,
                        StdError = 0x30, DllPath = 0x50, Environment = 0x80, StartingX = 0x88, ShowWindowFlags = 0xA8,
                        EnvironmentSize = 0x3F0;
}
template <class T> T& At(void* base, std::uint32_t off) { return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + off); }

using PFN_LdrInitializeThunk = void(NTAPI*)(PCONTEXT, ULONG_PTR, ULONG_PTR, ULONG_PTR);

struct SavedPeb {
    bool saved = false;
    void* image_base; void* ldr; void* params; void* fast_peb_lock; void* tls_bitmap; void* tls_exp_bitmap;
    void* loader_lock; void* ansi; void* oem; void* ucase;
    void* actctx;   // PEB.ActivationContextData: the host EXE's manifest context, overwritten by Wine's actctx_init
    void* kernel_callbacks;   // PEB.KernelCallbackTable: user32's DllMain installs the guest's (core/user reads it)
    void* gdi_shared;         // PEB.GdiSharedHandleTable: core/user installs its own handle table for gdi32
    ULONG os_major, os_minor, platform;
    USHORT build, csd;
};

struct Globals {
    SRWLOCK lock = SRWLOCK_INIT;
    bool launched = false;
    std::wstring image_path;               // guest Win32 path
    PFN_LdrInitializeThunk ldr_init = nullptr;
    void* user_thread_start = nullptr;     // guest RtlUserThreadStart
    HANDLE exit_event = nullptr;
    HANDLE main_thread = nullptr;
    volatile LONG exit_code = 0;
    volatile LONG exited = 0;
    SavedPeb peb;
    std::vector<wchar_t> environment;
    void* params = nullptr;                // what we installed (Wine replaces it in init_user_process_params)
    std::size_t stack_reserve = 1 << 20;
    HANDLE thread_ready = nullptr;          // main guest thread finished host-side thread init
    HANDLE peb_ready = nullptr;             // PEB take-over done, enter the guest
    bool wine_host = false;                 // host ntdll is Wine's (development runs)
    bool leak_actctx = false;               // WIN10XBOX_TEST_LEAK_GUEST_ACTCTX, latched in Launch (test hook)
};
Globals& G() {
    NoteHostStateUnderGuestView("process::G");
    static Globals g;
    return g;
}

struct GuestStart {
    void* start;
    void* arg;
    std::uint32_t flags;
    bool wait_for_peb = false;         // main thread: block until the PEB take-over is done
    bool clear_skip_attach = false;    // we added SKIP_THREAD_ATTACH for the host's sake
};
constexpr std::uint32_t kTebSameTebFlags = 0x17EE;
constexpr USHORT kSkipThreadAttach = 1u << 3;

std::wstring ToLowerCopy(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

void SavePebFields() {
    SavedPeb& s = G().peb;
    if (s.saved) return;
    s.image_base = PebField<void*>(peb::ImageBaseAddress);
    s.ldr = PebField<void*>(peb::Ldr);
    s.params = PebField<void*>(peb::ProcessParameters);
    s.fast_peb_lock = PebField<void*>(peb::FastPebLock);
    s.tls_bitmap = PebField<void*>(peb::TlsBitmap);
    s.tls_exp_bitmap = PebField<void*>(peb::TlsExpansionBitmap);
    s.loader_lock = PebField<void*>(peb::LoaderLock);
    s.ansi = PebField<void*>(peb::AnsiCodePageData);
    s.oem = PebField<void*>(peb::OemCodePageData);
    s.ucase = PebField<void*>(peb::UnicodeCaseTableData);
    s.actctx = PebField<void*>(peb::ActivationContextData);
    s.kernel_callbacks = PebField<void*>(peb::KernelCallbackTable);
    s.gdi_shared = PebField<void*>(peb::GdiSharedHandleTable);
    s.os_major = PebField<ULONG>(peb::OSMajorVersion);
    s.os_minor = PebField<ULONG>(peb::OSMinorVersion);
    s.build = PebField<USHORT>(peb::OSBuildNumber);
    s.csd = PebField<USHORT>(peb::OSCSDVersion);
    s.platform = PebField<ULONG>(peb::OSPlatformId);
    s.saved = true;
}

void RestorePebFields() {
    SavedPeb& s = G().peb;
    if (!s.saved) return;
    PebField<void*>(peb::ImageBaseAddress) = s.image_base;
    PebField<void*>(peb::Ldr) = s.ldr;
    PebField<void*>(peb::ProcessParameters) = s.params;
    PebField<void*>(peb::FastPebLock) = s.fast_peb_lock;
    PebField<void*>(peb::TlsBitmap) = s.tls_bitmap;
    PebField<void*>(peb::TlsExpansionBitmap) = s.tls_exp_bitmap;
    PebField<void*>(peb::LoaderLock) = s.loader_lock;
    PebField<void*>(peb::AnsiCodePageData) = s.ansi;
    PebField<void*>(peb::OemCodePageData) = s.oem;
    PebField<void*>(peb::UnicodeCaseTableData) = s.ucase;
    // Wine's actctx_init() ran during the guest's loader_init and stored ITS process_actctx here: a pointer to a Wine
    // ACTIVATION_CONTEXT (wine-src/dlls/ntdll/actctx.c: without a manifest it is the static `system_actctx` inside the
    // guest ntdll's own data; 0x17009B048 in the Wine runs, i.e. inside the guest ntdll image at 0x170000000). That is not
    // Microsoft's ACTIVATION_CONTEXT_DATA blob, and the host's ntdll reads this field for every SxS lookup made while no
    // activation context is active (DLL name redirection in LoadLibrary / GetModuleHandle, COM class lookup). Leaving
    // it behind hands the resumed host a structure of the wrong format that lives in the guest's image.
    //
    // WIN10XBOX_TEST_LEAK_GUEST_ACTCTX=1 skips exactly this restore, i.e. reinstates the behaviour from before it existed,
    // so that the detector (host-win32's WIN10XBOX_TEST_HOST_ACTCTX) and the real-Windows A/B run in host-win32-smoke can
    // be shown to see it. Latched in Launch: by now the PEB's environment block is the guest's.
    void* const guest_actctx = PebField<void*>(peb::ActivationContextData);
    if (!G().leak_actctx) PebField<void*>(peb::ActivationContextData) = s.actctx;
    PebField<void*>(peb::KernelCallbackTable) = s.kernel_callbacks;
    PebField<void*>(peb::GdiSharedHandleTable) = s.gdi_shared;
    PebField<ULONG>(peb::OSMajorVersion) = s.os_major;
    PebField<ULONG>(peb::OSMinorVersion) = s.os_minor;
    PebField<USHORT>(peb::OSBuildNumber) = s.build;
    PebField<USHORT>(peb::OSCSDVersion) = s.csd;
    PebField<ULONG>(peb::OSPlatformId) = s.platform;
    s.saved = false;
    if (G().leak_actctx)
        WARN(process, "WIN10XBOX_TEST_LEAK_GUEST_ACTCTX: PEB.ActivationContextData left at the guest's %p instead of the "
                      "host's %p (test hook)", guest_actctx, s.actctx);
    else if (guest_actctx != s.actctx)
        TRACE(process, "PEB.ActivationContextData %p (the guest's) -> %p (the host's)", guest_actctx, s.actctx);
    TRACE(process, "host PEB fields restored");
}

// ---- Guest environment -----------------------------------------------------------

std::vector<std::wstring> DefaultEnvironment() {
#if WIN10XBOX_HAVE_VFS
    const std::wstring sysroot = vfs::GuestSystemRoot(), sysdir = vfs::GuestSystemDirectory();
    const std::wstring profile = vfs::GuestUserProfile(), temp = vfs::GuestTempDirectory();
#else
    const std::wstring sysroot = L"C:\\Windows", sysdir = L"C:\\Windows\\system32";
    const std::wstring profile = L"C:\\Users\\User", temp = L"C:\\Users\\User\\AppData\\Local\\Temp";
#endif
#if WIN10XBOX_HAVE_TOKEN
    const std::wstring user = token::UserName(), computer = token::ComputerName();
#else
    const std::wstring user = L"User", computer = L"DESKTOP-XBOX";
#endif
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<std::wstring> env = {
        L"ALLUSERSPROFILE=C:\\ProgramData",
        L"APPDATA=" + profile + L"\\AppData\\Roaming",
        L"CommonProgramFiles=C:\\Program Files\\Common Files",
        L"CommonProgramFiles(x86)=C:\\Program Files (x86)\\Common Files",
        L"CommonProgramW6432=C:\\Program Files\\Common Files",
        L"COMPUTERNAME=" + computer,
        L"ComSpec=" + sysdir + L"\\cmd.exe",
        L"HOMEDRIVE=C:",
        L"HOMEPATH=" + (profile.size() > 2 ? profile.substr(2) : profile),
        L"LOCALAPPDATA=" + profile + L"\\AppData\\Local",
        L"NUMBER_OF_PROCESSORS=" + std::to_wstring(si.dwNumberOfProcessors),
        L"OS=Windows_NT",
        L"Path=" + sysdir + L";" + sysroot + L";" + sysdir + L"\\Wbem",
        L"PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC",
        L"PROCESSOR_ARCHITECTURE=AMD64",
        L"ProgramData=C:\\ProgramData",
        L"ProgramFiles=C:\\Program Files",
        L"ProgramFiles(x86)=C:\\Program Files (x86)",
        L"ProgramW6432=C:\\Program Files",
        L"PUBLIC=C:\\Users\\Public",
        L"SystemDrive=C:",
        L"SystemRoot=" + sysroot,
        L"TEMP=" + temp,
        L"TMP=" + temp,
        L"USERDOMAIN=" + computer,
        L"USERNAME=" + user,
        L"USERPROFILE=" + profile,
        L"windir=" + sysroot,
    };
    std::sort(env.begin(), env.end(), [](const std::wstring& a, const std::wstring& b) {
        return ToLowerCopy(a) < ToLowerCopy(b);
    });
    return env;
}

std::wstring DefaultCurrentDirectory() {
#if WIN10XBOX_HAVE_VFS
    return vfs::GuestUserProfile();
#else
    return L"C:\\Users\\User";
#endif
}

// ---- machine.json "cpu" without the VFS module -----------------------------------------
#if !WIN10XBOX_HAVE_VFS
// Tiny extraction (the VFS module owns machine.json parsing in general):
//   "cpu": { "hide_hypervisor": true, "vendor": "AuthenticAMD" }
void LoadCpuPolicy(const std::wstring& machine_root) {
    CpuPolicy p;
    p.hide_hypervisor = true;
    HANDLE f = CreateFileW((machine_root + L"\\machine.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[16384];
        DWORD n = 0;
        ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
        CloseHandle(f);
        std::string j(buf, n);
        std::size_t cpu = j.find("\"cpu\"");
        std::size_t open = cpu == std::string::npos ? cpu : j.find('{', cpu);
        std::size_t close = open == std::string::npos ? open : j.find('}', open);
        if (close != std::string::npos) {
            std::string obj = j.substr(open, close - open);
            std::size_t v = obj.find("\"vendor\"");
            if (v != std::string::npos) {
                std::size_t q1 = obj.find('"', obj.find(':', v));
                std::size_t q2 = q1 == std::string::npos ? q1 : obj.find('"', q1 + 1);
                if (q2 != std::string::npos) p.vendor = obj.substr(q1 + 1, q2 - q1 - 1);
            }
            std::size_t h = obj.find("\"hide_hypervisor\"");
            if (h != std::string::npos) {
                std::size_t colon = obj.find(':', h);
                std::size_t val = obj.find_first_not_of(" \t\r\n", colon + 1);
                if (val != std::string::npos) p.hide_hypervisor = obj.compare(val, 5, "false") != 0;
            }
        }
    }
    if (!p.vendor.empty() && p.vendor.size() != 12) {
        WARN(process, "machine.json cpu.vendor \"%s\" is not 12 characters, using the real vendor", p.vendor.c_str());
        p.vendor.clear();
    }
    SetCpuPolicy(p);
    TRACE(process, "cpu policy: vendor=%s hide_hypervisor=%d", p.vendor.empty() ? "(real)" : p.vendor.c_str(),
          p.hide_hypervisor);
}
#endif

// ---- Entering the guest on a thread ------------------------------------------------

// Commits `bytes` of stack below the current frame: Wine's signal_start_thread
// clears 60 KiB below the CONTEXT before switching to it.
__declspec(noinline) void ProbeStack(std::size_t bytes) {
    volatile std::uint8_t marker = 0;
    auto sp = reinterpret_cast<std::uintptr_t>(&marker) & ~std::uintptr_t(0xfff);
    const std::uintptr_t limit = static_cast<std::uintptr_t>(__readgsqword(teb::DeallocationStack)) + 3 * 0x1000;
    for (std::uintptr_t a = sp; a > limit && sp - a <= bytes; a -= 0x1000)
        (void)*reinterpret_cast<volatile std::uint8_t*>(a);
}

[[noreturn]] __declspec(noinline) void EnterGuest(void* start, void* arg) {
    ProbeStack(0x14000);
    CONTEXT ctx;   // CONTEXT is declared 16-byte aligned
    std::memset(&ctx, 0, sizeof ctx);
    RtlCaptureContext(&ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.EFlags &= ~0x500u;   // TF, DF
    const auto top = static_cast<std::uint64_t>(__readgsqword(teb::StackBase));
    ctx.Rsp = ((top - 0x100) & ~std::uint64_t(0xf)) - 8;   // as if entered by `call`
    ctx.Rip = reinterpret_cast<std::uint64_t>(G().user_thread_start);
    ctx.Rcx = reinterpret_cast<std::uint64_t>(start);
    ctx.Rdx = reinterpret_cast<std::uint64_t>(arg);
    TRACE(process, "thread %04lx entering guest: start=%p arg=%p", GetCurrentThreadId(), start, arg);
    const auto ldr_init = G().ldr_init;
    // The guest starts with NO TLS vector, like a fresh thread, not with the host's: ThreadState::host_tls
    // already holds the host's (AttachThreadState) and core/asm swaps it back in on every syscall. Wine's
    // loader installs its own vector in alloc_thread_tls, but before that, while it resolves the static
    // imports, alloc_tls_slot runs for every imported DLL with a TLS directory and frees and replaces slot i
    // of the vector it finds in the TEB ("if (!teb->ThreadLocalStoragePointer) continue" is its only guard).
    // With the host's vector there, that freed the host's own TLS blocks: 0xC0000374 in the host heap on real
    // Windows as soon as a guest imported a DLL with TLS (user32 -> native advapi32 -> msvcrt). FLS is left as
    // it is: Wine replaces FlsSlots unconditionally in its thread init.
    // Nothing between this store and the call touches TLS (ldr_init was read above).
    __writegsqword(teb::ThreadLocalStoragePointer, 0);
    ldr_init(&ctx, 0, 0, 0);
    ERR(process, "guest LdrInitializeThunk returned");
    detail::GuestThreadExit(st::Unsuccessful);
}

DWORD WINAPI GuestThreadProc(void* p) {
    auto* gs = static_cast<GuestStart*>(p);
    void* start = gs->start;
    void* arg = gs->arg;
    std::uint32_t flags = gs->flags;
    const bool wait_for_peb = gs->wait_for_peb, clear_skip = gs->clear_skip_attach;
    delete gs;
    if (!AttachThreadState(kThreadGuest | flags))
        WARN(process, "no thread state: guest/host TEB view switching disabled on this thread");
    if (clear_skip) {
        auto* tf = reinterpret_cast<USHORT*>(static_cast<std::uintptr_t>(__readgsqword(0x30)) + kTebSameTebFlags);
        *tf = static_cast<USHORT>(*tf & ~kSkipThreadAttach);
    }
    if (wait_for_peb) {
        // The host's own thread initialisation has run by now (it looks at
        // Peb->ImageBaseAddress); let Launch take the PEB over.
        SetEvent(G().thread_ready);
        WaitForSingleObject(G().peb_ready, INFINITE);
    }
    EnterGuest(start, arg);
    return 0;   // not reached
}

std::wstring Quote(const std::wstring& s) {
    return s.find_first_of(L" \t") == std::wstring::npos ? s : L"\"" + s + L"\"";
}

NTSTATUS MapGuestNtdll(const std::wstring& root) {
#if WIN10XBOX_HAVE_VFS
    std::wstring path = vfs::BuiltinDllPath(L"ntdll");
    if (path.empty()) path = root + L"\\Windows\\builtin\\ntdll.dll";
#else
    std::wstring path = root + L"\\Windows\\builtin\\ntdll.dll";
#endif
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        ERR(process, "guest ntdll not found at %ls: put Wine's PE builtins (wine-pe/out/builtin/*.dll) into "
                     "<machine>\\Windows\\builtin", path.c_str());
        return st::DllNotFound;
    }
    std::shared_ptr<ImageSection> sec;
    NTSTATUS s = CreateImageSectionFromFile(f, &sec);
    CloseHandle(f);
    if (s) { ERR(process, "%ls: not a usable image (%08lx)", path.c_str(), static_cast<unsigned long>(s)); return s; }
    sec->guest_path = L"C:\\Windows\\System32\\ntdll.dll";

    MapOptions mo;
    mo.keep_writable = true;
    mo.is_guest_ntdll = true;
    ImageView* view = nullptr;
    s = MapImageView(sec, mo, &view);
    if (!NtSuccess(s)) { ERR(process, "mapping guest ntdll failed: %08lx", static_cast<unsigned long>(s)); return s; }
    if (!view->stubs_rewritten) ERR(process, "guest ntdll: no syscall stub was rewritten");

    ApplyRtlForwarding(view->image);

    void* base = view->image.base;
    if (auto* p = static_cast<void**>(loader::FindExport(base, "__wine_unix_call_dispatcher")))
        *p = reinterpret_cast<void*>(&UnixCallDispatcher);
    else
        ERR(process, "guest ntdll has no __wine_unix_call_dispatcher: not a Wine PE ntdll?");
    if (auto* p = static_cast<std::uint64_t*>(loader::FindExport(base, "__wine_unixlib_handle")))
        *p = NtdllUnixLibHandle();
    G().ldr_init = reinterpret_cast<PFN_LdrInitializeThunk>(loader::FindExport(base, "LdrInitializeThunk"));
    G().user_thread_start = loader::FindExport(base, "RtlUserThreadStart");
    if (!G().ldr_init || !G().user_thread_start) {
        ERR(process, "guest ntdll lacks LdrInitializeThunk/RtlUserThreadStart");
        return st::InvalidImageFormat;
    }
    s = FinalizeView(view);
    if (!NtSuccess(s)) return s;
    TRACE(process, "guest ntdll at %p: %zu stubs, LdrInitializeThunk=%p", base, view->stubs,
          reinterpret_cast<void*>(G().ldr_init));
    return st::Success;
}

NTSTATUS MapMainImage(const std::wstring& guest_path, void** base_out, std::shared_ptr<ImageSection>* sec_out) {
    std::wstring nt = L"\\??\\" + guest_path;
    UNICODE_STRING us{static_cast<USHORT>(nt.size() * 2), static_cast<USHORT>(nt.size() * 2), nt.data()};
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io{};
    HANDLE file = nullptr;
    NTSTATUS s = Xl_NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               0x20 /*FILE_SYNCHRONOUS_IO_NONALERT*/ | 0x40 /*FILE_NON_DIRECTORY_FILE*/);
    if (s) { ERR(process, "cannot open %ls: %08lx", guest_path.c_str(), static_cast<unsigned long>(s)); return s; }
    HANDLE section = nullptr;
    s = Xl_NtCreateSection(&section, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_EXECUTE_READ, 0x1000000, file);
    Host().NtClose(file);
    if (s) { ERR(process, "%ls: not an image (%08lx)", guest_path.c_str(), static_cast<unsigned long>(s)); return s; }
    auto sec = ImageSectionFromHandle(section);
    std::uint16_t machine, characteristics;
    std::memcpy(&machine, sec->info + 0x30, 2);
    std::memcpy(&characteristics, sec->info + 0x2C, 2);
    if (machine != IMAGE_FILE_MACHINE_AMD64) {
        ERR(process, "%ls: machine %04x is not x64 (32-bit guests are not supported, D6)", guest_path.c_str(), machine);
        Xl_NtClose(section);
        return st::InvalidImageWin32;
    }
    if (characteristics & IMAGE_FILE_DLL) {
        ERR(process, "%ls is a DLL, not an executable", guest_path.c_str());
        Xl_NtClose(section);
        return st::InvalidImageFormat;
    }
    sec->guest_path = guest_path;
    void* base = nullptr;
    SIZE_T size = 0;
    s = Xl_NtMapViewOfSection(section, reinterpret_cast<HANDLE>(-1), &base, 0, 0, nullptr, &size, 1 /*ViewShare*/, 0,
                              PAGE_EXECUTE_READ);
    Xl_NtClose(section);
    if (!NtSuccess(s)) { ERR(process, "mapping %ls failed: %08lx", guest_path.c_str(), static_cast<unsigned long>(s)); return s; }
    std::uint64_t reserve;
    std::memcpy(&reserve, sec->info + 0x10, 8);
    G().stack_reserve = std::max<std::size_t>(static_cast<std::size_t>(reserve), 1 << 20);
    *base_out = base;
    *sec_out = sec;
    return st::Success;
}

NTSTATUS BuildProcessParameters(const LaunchOptions& o, const std::wstring& image) {
    std::wstring cmdline = o.command_line.empty() ? Quote(image) : o.command_line;
    std::wstring curdir = o.current_directory.empty() ? DefaultCurrentDirectory() : o.current_directory;
    if (curdir.empty() || curdir.back() != L'\\') curdir += L'\\';
    std::vector<std::wstring> env = o.environment.empty() ? DefaultEnvironment() : o.environment;
    auto& block = G().environment;
    block.clear();
    for (auto& e : env) { block.insert(block.end(), e.begin(), e.end()); block.push_back(0); }
    block.push_back(0);
    if (block.size() == 1) block.push_back(0);

    std::wstring title = image, desktop = L"WinSta0\\Default", empty;
    // MaximumLength leaves room for the terminating NUL (the std::wstring's own): RtlCreateProcessParametersEx packs
    // the strings back to back by MaximumLength, so without it the command line runs into the next string and a
    // program that reads GetCommandLineW as a C string (every CRT's argv) sees the image path and the desktop name
    // appended to its last argument. Windows always NUL-terminates these.
    auto us = [](std::wstring& s) {
        const USHORT len = static_cast<USHORT>(s.size() * 2);
        return UNICODE_STRING{len, static_cast<USHORT>(len ? len + 2 : 0), s.data()};
    };
    std::wstring img = image;
    UNICODE_STRING u_image = us(img), u_dll = us(empty), u_cur = us(curdir), u_cmd = us(cmdline), u_title = us(title),
                   u_desktop = us(desktop), u_shell = us(empty), u_runtime = us(empty);
    void* params = nullptr;
    if (!Host().RtlCreateProcessParametersEx) return st::NotImplemented;
    NTSTATUS s = Host().RtlCreateProcessParametersEx(&params, &u_image, &u_dll, &u_cur, &u_cmd, block.data(), &u_title,
                                                     &u_desktop, &u_shell, &u_runtime, 1 /*NORMALIZED*/);
    if (s) { ERR(process, "RtlCreateProcessParametersEx failed: %08lx", static_cast<unsigned long>(s)); return s; }

    // Wine's init_user_process_params() replaces the block and releases the
    // old one with NtFreeVirtualMemory(MEM_RELEASE), as the kernel-created
    // block of a real process is its own allocation. The host function may
    // use the process heap, so move the (normalized, contiguous) block into a
    // region of its own and rebase the pointers that point into it.
    {
        const ULONG size = At<ULONG>(params, 0);   // MaximumLength
        void* own = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!own) return st::NoMemory;
        std::memcpy(own, params, size);
        const auto lo = reinterpret_cast<std::uintptr_t>(params), hi = lo + size;
        const std::uint32_t header = std::min<std::uint32_t>(size, 0x440) & ~7u;
        for (std::uint32_t off = 0; off < header; off += 8) {
            auto& q = At<std::uintptr_t>(own, off);
            if (q >= lo && q < hi) q = q - lo + reinterpret_cast<std::uintptr_t>(own);
        }
        params = own;   // the heap copy is leaked on purpose: its environment may live next to it
    }

    void* old = PebField<void*>(peb::ProcessParameters);
    // Wine computes the default DLL search path only when DllPath.Buffer is NULL.
    auto& dll = At<UNICODE_STRING>(params, pp::DllPath);
    dll.Buffer = nullptr;
    dll.Length = dll.MaximumLength = 0;
    if (At<SIZE_T>(params, pp::EnvironmentSize) < block.size() * sizeof(wchar_t)) {
        At<void*>(params, pp::Environment) = block.data();
        At<SIZE_T>(params, pp::EnvironmentSize) = block.size() * sizeof(wchar_t);
    }
    if (old) {
        // Inherit the host's console and standard handles so the guest's stdio
        // reaches the same pipe/console. NOTE (real-Windows bring-up): with
        // these inherited the guest runs to completion (exit code is correct)
        // but its stdout is not captured on a real-Windows host; nulling
        // ConsoleHandle instead makes Wine's kernelbase fault. Tracked as the
        // real-Windows guest-stdio follow-up in docs/STATUS.md; under Wine the
        // guest end-to-end path (boot -> printf -> exit) works and is gated on
        // the linux-cross CI job.
        At<HANDLE>(params, pp::ConsoleHandle) = At<HANDLE>(old, pp::ConsoleHandle);
        At<ULONG>(params, pp::ConsoleFlags) = At<ULONG>(old, pp::ConsoleFlags);
        At<HANDLE>(params, pp::StdInput) = At<HANDLE>(old, pp::StdInput);
        At<HANDLE>(params, pp::StdOutput) = At<HANDLE>(old, pp::StdOutput);
        At<HANDLE>(params, pp::StdError) = At<HANDLE>(old, pp::StdError);
        // StartingX .. ShowWindowFlags: window placement and STARTF_* flags
        std::memcpy(static_cast<std::uint8_t*>(params) + pp::StartingX, static_cast<std::uint8_t*>(old) + pp::StartingX,
                    pp::ShowWindowFlags + 4 - pp::StartingX);
    }
    G().params = params;
    PebField<void*>(peb::ProcessParameters) = params;
    TRACE(process, "process parameters: image=%ls cmdline=%ls curdir=%ls (%zu env vars)", image.c_str(), cmdline.c_str(),
          curdir.c_str(), env.size());
    return st::Success;
}

std::wstring NormalizeGuestPath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.rfind(L"\\??\\", 0) == 0 || p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.size() > 1 && p[1] == L':') p[0] = static_cast<wchar_t>(std::towupper(p[0]));
    return p;
}

}  // namespace

int Launch(const LaunchOptions& opts) {
    Globals& g = G();
    AcquireSRWLockExclusive(&g.lock);
    if (g.launched) {
        ReleaseSRWLockExclusive(&g.lock);
        ERR(process, "Launch: a guest process is already running in this host (D1: one per host for now)");
        return static_cast<int>(st::Unsuccessful);
    }
    g.launched = true;
    ReleaseSRWLockExclusive(&g.lock);

    auto& cfg = config::Current();
    if (cfg.machine_root.empty()) { ERR(process, "Launch: config machine_root is empty"); return static_cast<int>(st::InvalidParameter); }
    {
        wchar_t full[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(cfg.machine_root.c_str(), MAX_PATH * 2, full, nullptr);
        if (n && n < MAX_PATH * 2) cfg.machine_root = full;
        while (cfg.machine_root.size() > 3 && cfg.machine_root.back() == L'\\') cfg.machine_root.pop_back();
    }
    const std::wstring root = cfg.machine_root;
    NTSTATUS s = dispatch::Initialize();
    if (s) return static_cast<int>(s);
#if WIN10XBOX_HAVE_VFS
    // machine.json is parsed by the VFS; a malformed file refuses to start.
    if ((s = vfs::Initialize(root))) { ERR(process, "vfs::Initialize(%ls) failed: %08lx", root.c_str(), static_cast<unsigned long>(s)); return static_cast<int>(s); }
    const vfs::MachineInfo mi = vfs::GetMachineInfo();
    cfg.os_major = mi.windows.major;
    cfg.os_minor = mi.windows.minor;
    cfg.os_build = mi.windows.build;
    cfg.os_ubr = mi.windows.ubr;
#endif
#if WIN10XBOX_HAVE_REGISTRY
    if ((s = registry::Initialize(root))) WARN(process, "registry::Initialize failed: %08lx", static_cast<unsigned long>(s));
#endif
#if WIN10XBOX_HAVE_CLOCK
    {
        clock::Settings cs;
#if WIN10XBOX_HAVE_VFS
        cs.scale = mi.clock.scale;
        cs.offset_100ns = mi.clock.offset_ms * 10000;
        cs.frozen = mi.clock.frozen;
#endif
        if ((s = clock::Initialize(cs))) WARN(process, "clock::Initialize failed: %08lx", static_cast<unsigned long>(s));
    }
#endif
#if WIN10XBOX_HAVE_TOKEN
    {
        token::Identity id;
#if WIN10XBOX_HAVE_VFS
        id.user_name = mi.identity.user_name;
        id.domain = mi.identity.domain;
        id.user_sid = mi.identity.user_sid;
        id.computer_name = mi.identity.computer_name;
#endif
        if ((s = token::Initialize(id))) WARN(process, "token::Initialize failed: %08lx", static_cast<unsigned long>(s));
    }
#endif
    g.wine_host = HostExport("wine_get_version") != nullptr;
    if (g.wine_host) TRACE(process, "host ntdll is Wine's: development run");
    {
        // Test hook, read here because the guest's environment block replaces the host's during the take-over.
        wchar_t v[8];
        const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEST_LEAK_GUEST_ACTCTX", v, 8);
        g.leak_actctx = n != 0 && n < 8 && v[0] != L'0';
    }
#if WIN10XBOX_HAVE_VFS
    {
        CpuPolicy cp;
        cp.hide_hypervisor = mi.cpu.hide_hypervisor;
        cp.vendor.assign(mi.cpu.vendor.begin(), mi.cpu.vendor.end());
        if (!cp.vendor.empty() && cp.vendor.size() != 12) {
            WARN(process, "machine.json cpu.vendor \"%s\" is not 12 characters, using the real vendor", cp.vendor.c_str());
            cp.vendor.clear();
        }
        SetCpuPolicy(cp);
        for (const auto& [name, value] : mi.syscall_overrides) {
            std::string n(name.begin(), name.end()), v(value.begin(), value.end());
            Policy p;
            if (!ParsePolicy(v.c_str(), &p)) { WARN(process, "syscall_overrides.%s: bad policy \"%s\"", n.c_str(), v.c_str()); continue; }
            if (NTSTATUS ps = SetPolicy(n.c_str(), p))
                WARN(process, "syscall_overrides.%s=%s not applied: %08lx", n.c_str(), v.c_str(), static_cast<unsigned long>(ps));
        }
    }
#else
    LoadCpuPolicy(root);
#endif
    RegisterNtdllUnixLib();

    if ((s = MapGuestNtdll(root))) return static_cast<int>(s);

    const std::wstring image = NormalizeGuestPath(opts.image_path);
    g.image_path = image;
    void* exe_base = nullptr;
    std::shared_ptr<ImageSection> exe;
    if ((s = MapMainImage(image, &exe_base, &exe))) { g.image_path.clear(); return static_cast<int>(s); }
    SetMainImage(exe);

    std::uint32_t entry_rva;
    {
        auto* b = static_cast<std::uint8_t*>(exe_base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
        entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    }
    void* entry = static_cast<std::uint8_t*>(exe_base) + entry_rva;
    g.exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.thread_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.peb_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto* gs = new GuestStart{entry, CurrentPeb(), kThreadMain};
    if (opts.run_on_new_thread) {
        // Created before the PEB take-over: the host's per-thread init must see the host PEB.
        gs->wait_for_peb = true;
        DWORD tid = 0;
        g.main_thread = CreateThread(nullptr, g.stack_reserve, &GuestThreadProc, gs, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
        if (!g.main_thread) {
            delete gs;
            ERR(process, "CreateThread failed: %lu", GetLastError());
            return static_cast<int>(st::NoMemory);
        }
        WaitForSingleObject(g.thread_ready, INFINITE);
    }

    // ---- PEB take-over (E2). Restored by GuestExit / WaitForGuest.
    SavePebFields();
    if ((s = BuildProcessParameters(opts, image))) {
        RestorePebFields();
        if (g.main_thread) { TerminateThread(g.main_thread, static_cast<DWORD>(s)); }
        return static_cast<int>(s);
    }
    PebField<void*>(peb::ImageBaseAddress) = exe_base;
    PebField<ULONG>(peb::OSMajorVersion) = cfg.os_major;
    PebField<ULONG>(peb::OSMinorVersion) = cfg.os_minor;
    PebField<USHORT>(peb::OSBuildNumber) = static_cast<USHORT>(cfg.os_build);
    PebField<USHORT>(peb::OSCSDVersion) = 0;
    PebField<ULONG>(peb::OSPlatformId) = VER_PLATFORM_WIN32_NT;

    TRACE(process, "launching %ls: base %p entry %p", image.c_str(), exe_base, entry);
    if (!opts.run_on_new_thread) {
        gs->flags |= kThreadCallerOwned;
        GuestThreadProc(gs);   // never returns
    }
    SetEvent(g.peb_ready);
    if (!opts.wait) return 0;
    return WaitForGuest(INFINITE);
}

int WaitForGuest(DWORD timeout_ms) {
    Globals& g = G();
    if (!g.main_thread || !g.exit_event) return static_cast<int>(st::InvalidHandle);
    HANDLE hs[2] = {g.exit_event, g.main_thread};
    DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
    if (w == WAIT_TIMEOUT) return static_cast<int>(STATUS_TIMEOUT);
    if (w == WAIT_OBJECT_0 + 1 && !g.exited) {
        DWORD code = 0;
        GetExitCodeThread(g.main_thread, &code);
        WARN(process, "guest main thread ended without ExitProcess (code %lu)", code);
        g.exit_code = static_cast<LONG>(code);
        RestorePebFields();
    }
    WaitForSingleObject(g.main_thread, 5000);   // GuestExit signals, then terminates the thread
    return static_cast<int>(g.exit_code);
}

[[noreturn]] void GuestExit(std::uint32_t exit_code) {
    Globals& g = G();
    InterlockedExchange(&g.exit_code, static_cast<LONG>(exit_code));
    InterlockedExchange(&g.exited, 1);
    TRACE(process, "guest exit, code %u", exit_code);
    TerminateOtherGuestThreads(static_cast<NTSTATUS>(exit_code));
    RestorePebFields();
    ThreadState* self = CurrentThreadState();
    const bool caller_owned = self && (self->flags & kThreadCallerOwned);
    if (g.exit_event) SetEvent(g.exit_event);
    DetachThreadState();
    if (caller_owned) {
        dispatch::Shutdown();
        ExitProcess(exit_code);
    }
    for (;;) Host().NtTerminateThread(reinterpret_cast<HANDLE>(-2), static_cast<NTSTATUS>(exit_code));
}

std::uint32_t GuestExitCode() { return static_cast<std::uint32_t>(G().exit_code); }

bool IsGuestThread() {
    ThreadState* st = CurrentThreadState();
    return st && (st->flags & kThreadGuest);
}

std::wstring GuestImagePath() { return G().image_path; }

bool MapHostDirectory(wchar_t letter, const std::wstring& host_dir) {
#if WIN10XBOX_HAVE_VFS
    (void)letter; (void)host_dir;
    return false;
#else
    SetFallbackDrive(letter, host_dir);
    return true;
#endif
}

}  // namespace win10xbox::process

namespace win10xbox::dispatch::detail {

NTSTATUS CreateGuestThread(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PVOID start, PVOID arg,
                           ULONG flags, SIZE_T zero_bits, SIZE_T stack_size, SIZE_T max_stack, PVOID attr_list) {
    auto* gs = new process::GuestStart{start, arg, 0};
    // A Wine host ntdll runs its thread attach against Peb->ImageBaseAddress
    // (now the guest EXE, unknown to it): skip the host's attach there and
    // restore the flag for the guest's loader in GuestThreadProc.
    if (process::G().wine_host && !(flags & 0x2 /*THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH*/)) {
        flags |= 0x2;
        gs->clear_skip_attach = true;
    }
    // Host RtlUserThreadStart -> BaseThreadInitThunk -> GuestThreadProc(gs):
    // host per-thread init runs first, then Wine's LdrInitializeThunk.
    NTSTATUS s = Host().NtCreateThreadEx(handle, access, attr, reinterpret_cast<HANDLE>(-1),
                                         reinterpret_cast<void*>(&process::GuestThreadProc), gs, flags, zero_bits,
                                         stack_size, max_stack, attr_list);
    if (!NtSuccess(s)) delete gs;
    return s;
}

// The guest's RtlUserThreadStart thunk, captured while mapping the guest ntdll
// (process::G()). Defined here, in detail, so NoteGuestEntry (nt_section.cpp) can
// recognise the launch continuation; external linkage matches internal.h.
void* GuestEntryThunk() { return process::G().user_thread_start; }

[[noreturn]] void GuestThreadExit(NTSTATUS code) {
    ThreadState* self = CurrentThreadState();
    std::size_t guests = GuestThreads().size();
    if (self && (self->flags & kThreadGuest) && guests <= 1) process::GuestExit(static_cast<std::uint32_t>(code));
    DetachThreadState();
    for (;;) Host().NtTerminateThread(reinterpret_cast<HANDLE>(-2), code);
}

std::vector<std::wstring> HostEnvironmentEntries() {
    // The host's own parameter block: the one the PEB held before the take-over (saved), or the PEB's current one
    // when no guest has launched in this process.
    const process::SavedPeb& saved = process::G().peb;
    const auto* params = static_cast<const std::uint8_t*>(saved.saved ? saved.params : PebField<void*>(peb::ProcessParameters));
    std::vector<std::wstring> out;
    if (!params) return out;
    const auto* env = *reinterpret_cast<const wchar_t* const*>(params + process::pp::Environment);
    if (!env) return out;
    for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) out.emplace_back(p);
    return out;
}

void TerminateOtherGuestThreads(NTSTATUS code) {
    const DWORD self = GetCurrentThreadId();
    for (ThreadState* t : GuestThreads()) {
        if (t->tid == self) continue;
        TRACE(process, "terminating guest thread %04lx", t->tid);
        if (t->thread) Host().NtTerminateThread(t->thread, code);
        ForgetGuestThread(t->tid);
    }
}

}  // namespace win10xbox::dispatch::detail
