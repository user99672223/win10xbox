// Internal path helpers for the VFS (string-only, no I/O).
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::vfs::detail {

inline bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

// Case folding used for every path comparison (a simplified NTFS $UpCase):
// ASCII, Latin-1 Supplement, Latin Extended-A, basic Greek and Cyrillic.
// Anything else folds to itself. Deterministic and locale independent.
wchar_t FoldChar(wchar_t c);
bool EqualNoCase(std::wstring_view a, std::wstring_view b);
// True if `s` begins with `prefix` (case-insensitive, no boundary check).
bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix);
// True if `s` begins with `prefix` and the prefix ends at a component
// boundary (end of string or a separator follows).
bool StartsWithComponentNoCase(std::wstring_view s, std::wstring_view prefix);
std::wstring Fold(std::wstring_view s);

// Splits `s` on '\\' and '/', dropping empty and "." components and applying
// ".." lexically (clamped at the root). Views point into `s`.
// *trailing_sep is set only if the input literally ends in a separator
// ("C:\a\." is "C:\a", like GetFullPath).
void NormalizeComponents(std::wstring_view s, std::vector<std::wstring_view>* out, bool* trailing_sep);

// A host path split into a root token and components, for prefix matching.
// root is "D:" (drive) or "\\server\share" (UNC), compared case-insensitively.
struct TokPath {
    std::wstring root;
    std::vector<std::wstring> comps;
    bool trailing = false;   // input ended in a separator
};
// Parses an absolute host Win32 path (also accepting "\\?\D:\" and
// "\\?\UNC\srv\share" forms and '/' separators), normalising "." and "..".
// Returns false for anything without a drive or UNC root.
bool ParseHostPath(std::wstring_view s, TokPath* out);
// "D:" + "\a\b" form; a bare root gives "D:" or "\\srv\share" (no trailing '\').
std::wstring JoinTokPath(const TokPath& p);

}  // namespace win10xbox::vfs::detail
