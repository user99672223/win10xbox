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

namespace win10xbox::dispatch::detail {

// ---- NTSTATUS values (own names: windows.h defines some STATUS_* as DWORD) --
namespace st {
constexpr NTSTATUS Success                = 0x00000000;
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
constexpr NTSTATUS BufferTooSmall         = (NTSTATUS)0xC0000023;
constexpr NTSTATUS ObjectTypeMismatch     = (NTSTATUS)0xC0000024;
constexpr NTSTATUS ObjectNameNotFound     = (NTSTATUS)0xC0000034;
constexpr NTSTATUS ObjectPathNotFound     = (NTSTATUS)0xC000003A;
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
}
namespace peb {
constexpr std::uint32_t ImageBaseAddress   = 0x10;
constexpr std::uint32_t Ldr                = 0x18;
constexpr std::uint32_t ProcessParameters  = 0x20;
constexpr std::uint32_t ProcessHeap        = 0x30;
constexpr std::uint32_t FastPebLock        = 0x38;
constexpr std::uint32_t TlsBitmap          = 0x78;
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
}
inline std::uint8_t* CurrentPeb() { return reinterpret_cast<std::uint8_t*>(__readgsqword(teb::Peb)); }
template <class T> T& PebField(std::uint32_t off) { return *reinterpret_cast<T*>(CurrentPeb() + off); }

// ---- Per-thread state (TLS slot; layout shared with core/asm) ---------------
struct ThreadState {
    std::uint64_t host_tls;     // 0x00
    std::uint64_t host_fls;     // 0x08
    std::uint64_t guest_tls;    // 0x10
    std::uint64_t guest_fls;    // 0x18
    std::uint32_t depth;        // 0x20: >0 = host view active
    std::uint32_t flags;        // 0x24
    DWORD         tid;
    HANDLE        thread;       // duplicated host handle (guest threads), may be null
    // Wine debug output (replacements for ntdll's per-TEB debug_info)
    char          dbg_line[1024];
    std::uint32_t dbg_pos;
    std::uint8_t  dbg_class;
    char          dbg_prefix[96];
    char          dbg_strings[1024];
    std::uint32_t dbg_str_pos;
};
static_assert(offsetof(ThreadState, host_tls) == 0x00);
static_assert(offsetof(ThreadState, host_fls) == 0x08);
static_assert(offsetof(ThreadState, guest_tls) == 0x10);
static_assert(offsetof(ThreadState, guest_fls) == 0x18);
static_assert(offsetof(ThreadState, depth) == 0x20);

constexpr std::uint32_t kThreadGuest       = 1u << 0;   // runs guest code
constexpr std::uint32_t kThreadMain        = 1u << 1;   // guest main thread (bootstrap)
constexpr std::uint32_t kThreadCallerOwned = 1u << 2;   // bootstrap ran on the Launch caller's thread

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

// ---- Typed host ntdll entry points used by translators -------------------------
using PFN_NtClose = NTSTATUS(NTAPI*)(HANDLE);
using PFN_NtCreateFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                          ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using PFN_NtOpenFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using PFN_NtQueryAttributesFile = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using PFN_NtDeleteFile = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
using PFN_NtQueryInformationFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
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
    PFN_NtQueryAttributesFile NtQueryAttributesFile;
    PFN_NtQueryAttributesFile NtQueryFullAttributesFile;
    PFN_NtDeleteFile NtDeleteFile;
    PFN_NtQueryInformationFile NtQueryInformationFile;
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
// Subsystem groups compiled out: their Emulate entries pass through to the
// host (registry/token/clock reads must work for Wine's loader_init) except
// the names listed by NeverPassThrough().
bool NeverPassThrough(const char* name);
// Syscalls whose argument count in Wine's table is wrong for Windows (Wine
// lists unimplemented ones as 0); returns 0 if no correction.
std::uint32_t NargsOverride(const char* name);

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
std::wstring HostPathOfHandle(HANDLE file);   // Win32 path, empty on failure
void SetFallbackDrive(wchar_t letter, const std::wstring& host_root);   // no-vfs builds only

// ---- Image sections and views (nt_section.cpp) ---------------------------------
struct ImageSection {
    std::vector<std::uint8_t> bytes;
    std::wstring host_path;     // Win32
    std::wstring guest_path;    // Win32 guest view ("C:\..."), may be empty
    std::uint8_t info[0x40];    // SECTION_IMAGE_INFORMATION as Windows fills it
    std::uint32_t size_of_image = 0;
};
NTSTATUS CreateImageSectionFromFile(HANDLE file, std::shared_ptr<ImageSection>* out);
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
std::uint64_t NtdllUnixLibHandle();
void RegisterNtdllUnixLib();
std::uint64_t UnixLibHandleByName(std::wstring_view module_name);   // 0 if none
// Replacements patched over the guest ntdll's __wine_dbg_* exports.
struct WineDebugChannel { unsigned char flags; char name[15]; };
extern "C" unsigned char __cdecl w10x_wine_dbg_get_channel_flags(WineDebugChannel* channel);
extern "C" int __cdecl w10x_wine_dbg_header(int cls, WineDebugChannel* channel, const char* function);
extern "C" int __cdecl w10x_wine_dbg_output(const char* str);
extern "C" const char* __cdecl w10x_wine_dbg_strdup(const char* str);

// ---- Function-level forwarding (rtl_forward.cpp) -----------------------------
struct ForwardResult { std::size_t forwarded = 0, skipped = 0; };
// Redirects listed guest ntdll exports to the host ntdll (and the Wine debug
// exports to us). The image must still be writable.
ForwardResult ApplyRtlForwarding(const loader::MappedImage& guest_ntdll);
// Writes a 5-byte rel32 jmp at `from` to `to` via a near trampoline. Exposed for tests.
bool PatchJump(void* from, const void* to);

}  // namespace win10xbox::dispatch::detail
