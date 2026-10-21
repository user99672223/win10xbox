// Private helpers for the loader module. See internal.h.
#include "internal.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace win10xbox::loader::detail {

void* HostNtdllProc(const char* name) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(ntdll, name));
}

bool GetNtHeaders(const void* base, const IMAGE_NT_HEADERS64** nt_out) {
    if (!base) return false;
    auto* b = static_cast<const std::uint8_t*>(base);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < 0 || dos->e_lfanew > 0x10000000) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    *nt_out = nt;
    return true;
}

bool GetSections(const void* base, const IMAGE_SECTION_HEADER** first, unsigned* count) {
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!GetNtHeaders(base, &nt)) return false;
    auto* opt = reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader);
    *first = reinterpret_cast<const IMAGE_SECTION_HEADER*>(opt + nt->FileHeader.SizeOfOptionalHeader);
    *count = nt->FileHeader.NumberOfSections;
    return true;
}

// ---- ScopedWritable ------------------------------------------------------
static bool IsWritableProt(DWORD p) {
    p &= 0xFF;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}
static bool IsExecProt(DWORD p) {
    p &= 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

ScopedWritable::ScopedWritable(void* p, std::size_t n) : p_(p), n_(n) {
    std::uintptr_t cur = RoundDown(reinterpret_cast<std::uintptr_t>(p), kPageSize);
    std::uintptr_t end = RoundUp(reinterpret_cast<std::uintptr_t>(p) + n, kPageSize);
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
            ok_ = false;
            break;
        }
        std::uintptr_t rend = std::min<std::uintptr_t>(
            end, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (!IsWritableProt(mbi.Protect)) {
            DWORD old = 0;
            bool exec = IsExecProt(mbi.Protect);
            void* rb = reinterpret_cast<void*>(cur);
            std::size_t rs = static_cast<std::size_t>(rend - cur);
            bool done = VirtualProtect(rb, rs, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old) != 0;
            if (!done && exec) done = VirtualProtect(rb, rs, PAGE_READWRITE, &old) != 0;
            if (!done) {
                ok_ = false;
                break;
            }
            runs_.push_back({rb, rs, old});
        }
        cur = rend;
    }
}

ScopedWritable::~ScopedWritable() {
    for (auto it = runs_.rbegin(); it != runs_.rend(); ++it) {
        DWORD ignore = 0;
        VirtualProtect(it->base, it->size, it->old, &ignore);
    }
    FlushInstructionCache(GetCurrentProcess(), p_, n_);
}

// ---- protected ranges -----------------------------------------------------
namespace {
SRWLOCK g_prot_lock = SRWLOCK_INIT;
std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& Ranges() {
    static std::vector<std::pair<std::uintptr_t, std::uintptr_t>> r;
    return r;
}
struct Exclusive {
    Exclusive() { AcquireSRWLockExclusive(&g_prot_lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(&g_prot_lock); }
};
struct Shared {
    Shared() { AcquireSRWLockShared(&g_prot_lock); }
    ~Shared() { ReleaseSRWLockShared(&g_prot_lock); }
};
}  // namespace

void AddProtectedRange(const void* start, std::size_t len) {
    if (!len) return;
    Exclusive lk;
    auto& r = Ranges();
    std::uintptr_t b = reinterpret_cast<std::uintptr_t>(start), e = b + len;
    // Merge with any overlapping/adjacent ranges.
    auto it = std::lower_bound(r.begin(), r.end(), std::make_pair(b, std::uintptr_t(0)));
    if (it != r.begin() && std::prev(it)->second >= b) --it;
    auto first = it;
    while (it != r.end() && it->first <= e) {
        b = std::min(b, it->first);
        e = std::max(e, it->second);
        ++it;
    }
    it = r.erase(first, it);
    r.insert(it, std::make_pair(b, e));
}

bool IsProtectedRange(const void* addr, const void** end) {
    Shared lk;
    auto& r = Ranges();
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
    auto it = std::upper_bound(r.begin(), r.end(), std::make_pair(a, ~std::uintptr_t(0)));
    if (it == r.begin()) return false;
    --it;
    if (a >= it->first && a < it->second) {
        if (end) *end = reinterpret_cast<const void*>(it->second);
        return true;
    }
    return false;
}

void ForgetProtectedRanges(const void* start, std::size_t len) {
    Exclusive lk;
    auto& r = Ranges();
    std::uintptr_t b = reinterpret_cast<std::uintptr_t>(start), e = b + len;
    r.erase(std::remove_if(r.begin(), r.end(),
                           [&](const auto& pr) { return pr.first < e && pr.second > b; }),
            r.end());
}

}  // namespace win10xbox::loader::detail
