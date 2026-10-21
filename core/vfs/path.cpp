#include "path.h"

namespace win10xbox::vfs::detail {

wchar_t FoldChar(wchar_t c) {
    if (c < 0x80) return (c >= L'a' && c <= L'z') ? wchar_t(c - 0x20) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return wchar_t(c - 0x20);   // Latin-1
    if (c == 0xFF) return wchar_t(0x178);
    if (c >= 0x100 && c <= 0x17F) {                                       // Latin Ext-A
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x178 || c == 0x17F) return c;
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
            return (c & 1) ? wchar_t(c - 1) : c;          // odd = lower
        return (c & 1) ? c : wchar_t(c - 1);              // 0x139..0x148, 0x179..0x17E: even = lower
    }
    if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? wchar_t(0x3A3) : wchar_t(c - 0x20);   // Greek
    if (c == 0x3AC) return wchar_t(0x386);
    if (c >= 0x3AD && c <= 0x3AF) return wchar_t(c - 0x25);
    if (c == 0x3CC) return wchar_t(0x38C);
    if (c == 0x3CD || c == 0x3CE) return wchar_t(c - 0x3F);
    if (c >= 0x430 && c <= 0x44F) return wchar_t(c - 0x20);                                  // Cyrillic
    if (c >= 0x450 && c <= 0x45F) return wchar_t(c - 0x50);
    return c;
}

bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (FoldChar(a[i]) != FoldChar(b[i])) return false;
    return true;
}

bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && EqualNoCase(s.substr(0, prefix.size()), prefix);
}

bool StartsWithComponentNoCase(std::wstring_view s, std::wstring_view prefix) {
    if (!StartsWithNoCase(s, prefix)) return false;
    return s.size() == prefix.size() || IsSep(s[prefix.size()]);
}

std::wstring Fold(std::wstring_view s) {
    std::wstring r(s);
    for (auto& ch : r) ch = FoldChar(ch);
    return r;
}

void NormalizeComponents(std::wstring_view s, std::vector<std::wstring_view>* out, bool* trailing_sep) {
    out->clear();
    bool trailing = false;
    size_t i = 0;
    while (i < s.size()) {
        if (IsSep(s[i])) { ++i; trailing = true; continue; }
        size_t j = i;
        while (j < s.size() && !IsSep(s[j])) ++j;
        std::wstring_view c = s.substr(i, j - i);
        i = j;
        trailing = false;
        if (c == L".") continue;
        if (c == L"..") {
            if (!out->empty()) out->pop_back();
            continue;
        }
        out->push_back(c);
    }
    if (trailing_sep) *trailing_sep = trailing;
}

bool ParseHostPath(std::wstring_view s, TokPath* out) {
    std::wstring_view rest;
    std::wstring root;
    if (s.size() >= 4 && IsSep(s[0]) && IsSep(s[1]) && (s[2] == L'?' || s[2] == L'.') && IsSep(s[3])) {
        s.remove_prefix(4);
        if (s.size() >= 4 && StartsWithNoCase(s, L"UNC") && IsSep(s[3])) {
            // \\?\UNC\server\share -> treat as \\server\share
            std::wstring tmp = L"\\\\";
            tmp.append(s.substr(4));
            return ParseHostPath(tmp, out);
        }
    }
    if (s.size() >= 2 && s[1] == L':' &&
        ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))) {
        root.assign(1, s[0]);
        root += L':';
        rest = s.substr(2);
        if (!rest.empty() && !IsSep(rest[0])) return false;   // "D:foo" is drive-relative
    } else if (s.size() >= 2 && IsSep(s[0]) && IsSep(s[1])) {
        // \\server\share[\...]
        std::wstring_view t = s.substr(2);
        size_t a = 0;
        while (a < t.size() && !IsSep(t[a])) ++a;
        if (a == 0 || a >= t.size()) return false;
        size_t b = a + 1;
        while (b < t.size() && !IsSep(t[b])) ++b;
        if (b == a + 1) return false;
        root = L"\\\\";
        root.append(t.substr(0, a));
        root += L'\\';
        root.append(t.substr(a + 1, b - a - 1));
        rest = t.substr(b);
    } else {
        return false;
    }
    std::vector<std::wstring_view> comps;
    bool trailing = false;
    NormalizeComponents(rest, &comps, &trailing);
    out->trailing = trailing;
    out->root = std::move(root);
    out->comps.clear();
    for (auto c : comps) out->comps.emplace_back(c);
    return true;
}

std::wstring JoinTokPath(const TokPath& p) {
    std::wstring r = p.root;
    for (const auto& c : p.comps) { r += L'\\'; r += c; }
    return r;
}

}  // namespace win10xbox::vfs::detail
