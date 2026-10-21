// win10xbox loader: syscall thunk rewriting.
//
// Thunk shapes (all x64, ids are real Windows 10 numbers, E1 in DECISIONS.md):
//
//   Windows 10 ntdll/win32u (24 bytes)         Wine 11 PE ntdll (32 bytes, asm.h __ASM_SYSCALL_FUNC)
//   00: 4C 8B D1              mov r10,rcx      00: 4C 8B D1
//   03: B8 id32               mov eax,id       03: B8 id32
//   08: F6 04 25 08 03 FE 7F 01  test [7FFE0308h],1   08: (same)
//   10: 75 03                 jne +3           10: 75 03
//   12: 0F 05                 syscall          12: 0F 05
//   14: C3                    ret              14: C3
//   15: CD 2E                 int 2Eh          15: EB 01          jmp +1
//   17: C3                    ret              17: C3
//                                              18: FF 14 25 00 10 FE 7F  call [7FFE1000h]
//                                              1F: C3
//
// Rewrite: bytes [8,22) become FF 25 00 00 00 00 <imm64 target> (jmp [rip+0]).
// The first 8 bytes (mov r10,rcx; mov eax,id) are preserved so the dispatcher
// entry sees eax = id, r10 = first argument and the return address on the stack
// exactly as the kernel would.
#include "internal.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "win10xbox/trace.h"

namespace win10xbox::loader {

namespace detail {

namespace {
constexpr std::uint8_t kHead[3]    = {0x4C, 0x8B, 0xD1};
constexpr std::uint8_t kTest[8]    = {0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01};
constexpr std::uint8_t kTailWin[]  = {0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};
constexpr std::uint8_t kTailWine[] = {0x75, 0x03, 0x0F, 0x05, 0xC3, 0xEB, 0x01, 0xC3,
                                      0xFF, 0x14, 0x25, 0x00, 0x10, 0xFE, 0x7F, 0xC3};
constexpr std::uint8_t kJmpRip0[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
}  // namespace

ThunkForm MatchSyscallThunk(const std::uint8_t* p, std::size_t avail, std::uint32_t* id, std::size_t* len) {
    if (avail < 24) return ThunkForm::None;
    if (std::memcmp(p, kHead, 3) != 0 || p[3] != 0xB8) return ThunkForm::None;
    std::uint32_t v;
    std::memcpy(&v, p + 4, 4);
    if (std::memcmp(p + 8, kJmpRip0, 6) == 0) {   // already rewritten
        if (id) *id = v;
        if (len) *len = 24;
        return ThunkForm::Rewritten;
    }
    if (std::memcmp(p + 8, kTest, 8) != 0) return ThunkForm::None;
    if (std::memcmp(p + 16, kTailWin, sizeof(kTailWin)) == 0) {
        if (id) *id = v;
        if (len) *len = 24;
        return ThunkForm::Windows;
    }
    if (avail >= 32 && std::memcmp(p + 16, kTailWine, sizeof(kTailWine)) == 0) {
        if (id) *id = v;
        if (len) *len = 32;
        return ThunkForm::Wine;
    }
    return ThunkForm::None;
}

}  // namespace detail

namespace {

bool IsSyscallName(const std::string& n) {
    return n.size() > 2 && (n[0] == 'N' || n[0] == 'Z') && (n[1] == 't' || n[1] == 'w') &&
           ((n[0] == 'N' && n[1] == 't') || (n[0] == 'Z' && n[1] == 'w')) && n[2] >= 'A' && n[2] <= 'Z';
}

}  // namespace

std::size_t RewriteSyscallStubs(const MappedImage& img, void* target, std::vector<StubPatch>* patched,
                                std::vector<std::string>* unmatched) {
    if (!img.base || !target) return 0;
    std::vector<detail::NamedExport> exports;
    if (!detail::EnumNamedExports(img.base, &exports)) {
        WARN(loader, "image %p has no export table", img.base);
        return 0;
    }
    // Group Nt*/Zw* aliases by thunk address: one rewrite, one report.
    std::map<std::uint32_t, std::vector<std::string>> by_rva;
    for (auto& e : exports) {
        if (!IsSyscallName(e.name)) continue;
        if (e.forwarder || e.rva == 0) {
            if (unmatched) unmatched->push_back(e.name);
            continue;
        }
        by_rva[e.rva].push_back(e.name);
    }

    std::size_t count = 0;
    auto* base = static_cast<std::uint8_t*>(img.base);
    for (auto& [rva, names] : by_rva) {
        std::uint8_t* thunk = base + rva;
        std::size_t avail = rva < img.size ? img.size - rva : 0;
        std::uint32_t id = 0;
        std::size_t len = 0;
        detail::ThunkForm form = detail::MatchSyscallThunk(thunk, avail, &id, &len);
        if (form == detail::ThunkForm::None) {
            for (auto& n : names) {
                TRACE(loader, "%s at %p does not match a known syscall thunk", n.c_str(), thunk);
                if (unmatched) unmatched->push_back(n);
            }
            continue;
        }
        std::uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
        std::uint64_t t = reinterpret_cast<std::uintptr_t>(target);
        std::memcpy(patch + 6, &t, 8);
        {
            detail::ScopedWritable w(thunk + 8, sizeof(patch));
            if (!w.ok()) {
                ERR(loader, "cannot make %p writable", thunk);
                for (auto& n : names)
                    if (unmatched) unmatched->push_back(n);
                continue;
            }
            std::memcpy(thunk + 8, patch, sizeof(patch));
        }
        detail::AddProtectedRange(thunk, len);
        ++count;
        if (patched) {
            // Prefer the Nt* spelling when both aliases exist.
            const std::string* pick = &names[0];
            for (auto& n : names)
                if (n[0] == 'N') {
                    pick = &n;
                    break;
                }
            patched->push_back({*pick, id, thunk});
        }
        TRACE(loader, "%s id=0x%x %s -> %p", names[0].c_str(), id,
              form == detail::ThunkForm::Wine ? "wine" : form == detail::ThunkForm::Rewritten ? "re-patched" : "windows",
              target);
    }
    return count;
}

}  // namespace win10xbox::loader
