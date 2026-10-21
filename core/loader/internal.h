// Private helpers shared by mapper.cpp, stubs.cpp and scanner.cpp.
// Not part of the public interface (see core/include/win10xbox/loader.h).
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "win10xbox/loader.h"

namespace win10xbox::loader::detail {

constexpr std::size_t kPageSize = 0x1000;

inline std::uintptr_t RoundUp(std::uintptr_t v, std::uintptr_t a) { return (v + a - 1) & ~(a - 1); }
inline std::uintptr_t RoundDown(std::uintptr_t v, std::uintptr_t a) { return v & ~(a - 1); }

// ---- host ntdll -----------------------------------------------------------
// Resolve an export of the host process' ntdll (never the guest's).
void* HostNtdllProc(const char* name);

// ---- PE helpers on an already-mapped image ---------------------------------
// Validates DOS/NT signatures and PE32+ magic. Returns false on anything odd.
bool GetNtHeaders(const void* base, const IMAGE_NT_HEADERS64** nt);
// Section table of a mapped image (bounds-checked against SizeOfHeaders/image).
bool GetSections(const void* base, const IMAGE_SECTION_HEADER** first, unsigned* count);
// Effective in-memory size of a section (VirtualSize, or SizeOfRawData when 0).
inline std::uint32_t SectionSize(const IMAGE_SECTION_HEADER& s) {
    return s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
}

struct NamedExport {
    std::string   name;
    std::uint16_t ordinal;     // biased (as seen by GetProcAddress by ordinal)
    std::uint32_t rva;         // 0 if the slot is empty
    bool          forwarder;   // rva points inside the export directory
};
// All named exports of a mapped/host-loaded image, in name-table order.
bool EnumNamedExports(const void* base, std::vector<NamedExport>* out);

// ---- Syscall thunk recognition (stubs.cpp) ----------------------------------
enum class ThunkForm : std::uint8_t { None, Windows, Wine, Rewritten };
// Classifies the bytes at p (avail bytes readable). Windows = 24-byte canonical
// form, Wine = 32-byte form, Rewritten = "mov r10,rcx; mov eax,id; jmp [rip+0]"
// as produced by RewriteSyscallStubs. *len gets the full thunk length where
// known (24/32; 24 for Rewritten), *id the syscall number.
ThunkForm MatchSyscallThunk(const std::uint8_t* p, std::size_t avail, std::uint32_t* id, std::size_t* len);

// ---- Temporary write access to (possibly executable) pages --------------------
// Prefers PAGE_EXECUTE_READWRITE for executable pages so concurrently running
// threads never fault; falls back to PAGE_READWRITE if the host refuses RWX
// (jit policy D11). Restores the original protection and flushes the
// instruction cache on destruction. Pages that are already writable are untouched.
class ScopedWritable {
public:
    ScopedWritable(void* p, std::size_t n);
    ~ScopedWritable();
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;
    bool ok() const { return ok_; }
private:
    struct Run { void* base; std::size_t size; DWORD old; };
    std::vector<Run> runs_;
    void*       p_;
    std::size_t n_;
    bool        ok_ = true;
};

// ---- Ranges the scanner must never decode ----------------------------------
// Rewritten syscall thunks (their tails contain an absolute address that would
// decode as random instructions) and regions already patched by the scanner.
void AddProtectedRange(const void* start, std::size_t len);
// True if addr lies inside a protected range; *end (optional) gets its end.
bool IsProtectedRange(const void* addr, const void** end);
void ForgetProtectedRanges(const void* start, std::size_t len);

}  // namespace win10xbox::loader::detail
