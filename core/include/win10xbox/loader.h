// win10xbox loader: manual PE mapping, syscall-stub rewriting and the code
// scanner. Pure NT/Win32. Owned by core/loader/.
//
// Division of labour with Wine's ntdll (the guest ntdll, see DECISIONS.md E1):
// Wine's loader.c does import resolution, apisets, forwarders, delay-load
// setup, TLS, DllMain ordering and activation contexts. This module does what
// the Wine *unix side* does for images: map the file as an image (sections,
// relocations, protections), register unwind tables with the host, and then
// our own additions: rewrite syscall thunks and patch privileged/identifying
// instructions.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace win10xbox::loader {

// NTSTATUS values returned by this module. Defined here (not via ntstatus.h)
// so callers need no WIN32_NO_STATUS include dance.
inline constexpr NTSTATUS kStatusSuccess              = static_cast<NTSTATUS>(0x00000000);
inline constexpr NTSTATUS kStatusImageNotAtBase       = static_cast<NTSTATUS>(0x40000003);
inline constexpr NTSTATUS kStatusInvalidParameter     = static_cast<NTSTATUS>(0xC000000D);
inline constexpr NTSTATUS kStatusNoMemory             = static_cast<NTSTATUS>(0xC0000017);
inline constexpr NTSTATUS kStatusConflictingAddresses = static_cast<NTSTATUS>(0xC0000018);
inline constexpr NTSTATUS kStatusInvalidImageFormat   = static_cast<NTSTATUS>(0xC000007B);
inline constexpr NTSTATUS kStatusInvalidImageWin32    = static_cast<NTSTATUS>(0xC0000359);  // STATUS_INVALID_IMAGE_WIN_32
inline constexpr NTSTATUS kStatusUnsuccessful         = static_cast<NTSTATUS>(0xC0000001);

struct MappedImage {
    void*         base = nullptr;        // preferred base if free, else relocated
    std::size_t   size = 0;              // SizeOfImage, page-rounded
    void*         entry = nullptr;       // base + AddressOfEntryPoint (nullptr if 0)
    bool          relocated = false;     // true -> caller gets STATUS_IMAGE_NOT_AT_BASE semantics
    bool          is_dll = false;
    std::uint32_t characteristics = 0;   // IMAGE_FILE_HEADER.Characteristics
    std::uint32_t dll_characteristics = 0;
    std::uint64_t image_base_preferred = 0;
    std::wstring  path;                  // NT or Win32 path it was mapped from (diagnostics)
    // Host-side unwind registration for .pdata so the host ntdll can dispatch
    // exceptions/unwind through this image. 0 if the image has no .pdata.
    PRUNTIME_FUNCTION function_table = nullptr;
    std::uint32_t function_count = 0;
};

enum class MapFlags : std::uint32_t {
    None            = 0,
    NoRelocate      = 1u << 0,   // fail (STATUS_CONFLICTING_ADDRESSES) instead of relocating
    ReserveOnly     = 1u << 1,   // map headers+sections but keep pages PAGE_READWRITE (caller patches, then calls FinalizeProtections)
    RegisterUnwind  = 1u << 2,   // call RtlAddFunctionTable on the host for .pdata (default on in MapImage helpers)
};
// What most callers want: relocate if needed, register .pdata with the host.
inline constexpr MapFlags kDefaultMapFlags = static_cast<MapFlags>(1u << 2);
inline MapFlags operator|(MapFlags a, MapFlags b) { return MapFlags(std::uint32_t(a) | std::uint32_t(b)); }
inline bool operator&(MapFlags a, MapFlags b) { return (std::uint32_t(a) & std::uint32_t(b)) != 0; }

// Map a PE image from memory (the whole file contents). On success *out is
// filled and the image is mapped with final section protections unless
// ReserveOnly. Returns an NTSTATUS (STATUS_SUCCESS, STATUS_IMAGE_NOT_AT_BASE
// when relocated and caller passed NoRelocate==false, STATUS_INVALID_IMAGE_FORMAT,
// STATUS_INVALID_IMAGE_WIN_32 for 32-bit images, STATUS_NO_MEMORY...).
// `preferred` may request a specific base (0 = image's own ImageBase).
NTSTATUS MapImageFromMemory(const void* file, std::size_t file_size, MapFlags flags,
                            void* preferred, MappedImage* out);
// Convenience: read the file through the host (Win32 path) and map it.
NTSTATUS MapImageFromFile(const wchar_t* host_path, MapFlags flags, void* preferred, MappedImage* out);
// Apply final section protections (after ReserveOnly mapping + patching).
NTSTATUS FinalizeProtections(const MappedImage& img);
// Unmap and unregister unwind info.
void UnmapImage(MappedImage& img);

// Export lookup on a mapped (or host-loaded) image. Handles forwarders by
// returning the forwarder string in *forwarder (caller resolves) and nullptr.
void* FindExport(const void* image_base, const char* name, std::string* forwarder = nullptr);
void* FindExportByOrdinal(const void* image_base, std::uint16_t ordinal, std::string* forwarder = nullptr);

// ---- Syscall stub rewriting -------------------------------------------------
// Scans the export table of an ntdll/win32u-like image for the canonical
// Windows x64 syscall thunk:
//   4C 8B D1              mov r10, rcx
//   B8 xx xx xx xx        mov eax, <id>
//   F6 04 25 08 03 FE 7F 01   test byte ptr [7FFE0308h], 1
//   75 03                 jne +3
//   0F 05                 syscall
//   C3                    ret
//   CD 2E                 int 2Eh            (Wine: EB 01 / C3 / FF 14 25 00 10 FE 7F)
//   C3                    ret
// and rewrites bytes [8..) of each thunk with an absolute jump to `target`,
// preserving `mov r10,rcx; mov eax,id` so the dispatcher entry sees eax = id
// and r10 = first argument. Thunks that do not match the pattern are reported
// (not patched). Returns the number of stubs patched.
struct StubPatch { std::string name; std::uint32_t id; void* address; };
std::size_t RewriteSyscallStubs(const MappedImage& img, void* target, std::vector<StubPatch>* patched,
                                std::vector<std::string>* unmatched);

// ---- Code scanner -----------------------------------------------------------
// Scans every executable section of a mapped image with Zydis for
// instructions that must be virtualised: cpuid, rdtsc, rdtscp, syscall,
// sysenter, int 2Eh, and absolute reads of KUSER_SHARED_DATA (0x7FFE0000..
// 0x7FFE1000, optional). Each site is redirected to a trampoline that calls
// the registered handler with a CONTEXT-like register block and resumes.
// Two patch strategies: (a) inline `jmp` with relocation of displaced
// instructions when the site and its successors can be moved safely,
// (b) a 1-byte `int3` + vectored-exception-handler fallback otherwise.
// Both are transparent to the caller of ScanAndPatch.
enum class SiteKind : std::uint8_t { Cpuid, Rdtsc, Rdtscp, Syscall, Sysenter, Int2E, KuserRead };

struct ScanRegs {         // what handlers get: mutable GPRs + flags
    std::uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rflags, rip;
};
// Handler returns true if it emulated the instruction (regs updated, rip
// advanced past it is done by the trampoline), false to execute the original.
using SiteHandler = bool (*)(SiteKind kind, ScanRegs* regs, void* ctx);

struct ScanOptions {
    bool patch_cpuid = true, patch_rdtsc = true, patch_syscall = true, patch_kuser_reads = false;
    SiteHandler handler = nullptr;
    void* handler_ctx = nullptr;
};
struct ScanSite { SiteKind kind; void* address; std::uint8_t length; bool patched_inline; };
struct ScanResult { std::vector<ScanSite> sites; std::size_t patched = 0, trapped = 0, skipped = 0; };

NTSTATUS ScanAndPatch(const MappedImage& img, const ScanOptions& opts, ScanResult* result);
// Scan an arbitrary range (e.g. JIT output or a host-loaded module).
NTSTATUS ScanAndPatchRange(void* start, std::size_t size, const ScanOptions& opts, ScanResult* result);
// Forget every trap site / thunk arena / protected range belonging to
// [start, start+size) and free the thunk memory. UnmapImage calls this; call it
// yourself for ranges passed to ScanAndPatchRange once they are unmapped. The
// code in the range must not be executed afterwards (patched sites are gone).
void ReleaseScanSites(const void* start, std::size_t size);
// Install the VEH used by strategy (b). Idempotent; called by ScanAndPatch.
void EnsureTrapHandlerInstalled();

}  // namespace win10xbox::loader
