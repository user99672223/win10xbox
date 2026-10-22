// NLS data sections: NtGetNlsSectionPtr and NtInitializeNlsFiles. Also the
// default locale state (NtQueryDefaultLocale and friends, below the sections).
//
// Wine's ntdll implements both entirely on its unix side, with Wine's own
// `type` numbering (dlls/ntdll/locale_private.h), so the real NT kernel rejects
// them (STATUS_INVALID_PARAMETER_1). They are emulated here the way Wine's
// unix side does it: one call resolves the .nls data file in the GUEST machine
// folder (the virtual C:\), maps it read-only and returns the view. The host
// NtGetNlsSectionPtr is never called.
//
// Files (genuine Windows data, imported by machine/import_windows_dlls.ps1,
// never committed):
//   C:\Windows\System32\{c_NNN,l_intl,locale,normnfc,normnfd,normnfkc,normnfkd,normidna}.nls
//   C:\Windows\Globalization\Sorting\sortdefault.nls   (System32 as fallback)
// A missing file fails with STATUS_OBJECT_NAME_NOT_FOUND; nothing is faked.
//
// Sections are kept for the process lifetime and cached per (type, id), like
// the permanent \NLS\NlsSection* objects Wine opens. Every call maps a fresh
// view of the cached section, because ntdll unmaps the view it was handed in
// some paths (locale.c load_norm_table on a lost race or a failed sanity
// check, RtlGetLocaleFileMappingAddress on a lost race) and that must not pull
// the mapping out from under another caller.
#include "internal.h"
#include <map>

WIN10XBOX_DECLARE_CHANNEL(nls);

namespace win10xbox::dispatch::detail {
namespace {

// Wine's NLS section types (dlls/ntdll/locale_private.h).
enum NlsSectionType : ULONG {
    kNlsSortkeys = 9,
    kNlsCasemap = 10,
    kNlsCodepage = 11,
    kNlsNormalize = 12,
};
// Normalization form ids (winnls.h NORM_FORM; 13 is Wine's IDNA table).
enum NlsNormForm : ULONG {
    kNormC = 0x1,
    kNormD = 0x2,
    kNormKC = 0x5,
    kNormKD = 0x6,
    kNormIdna = 13,
};

constexpr NTSTATUS kStatusInvalidParameter1 = (NTSTATUS)0xC00000EF;
constexpr NTSTATUS kStatusAccessDenied = (NTSTATUS)0xC0000022;
constexpr ULONG kSystemLcid = 0x0409;   // en-US, as machine.json's default locale

struct NlsSection {
    HANDLE mapping = nullptr;   // read-only file mapping, never closed
    SIZE_T size = 0;            // file size
};

SRWLOCK g_nls_lock = SRWLOCK_INIT;
std::map<std::uint64_t, NlsSection>& Sections() { static std::map<std::uint64_t, NlsSection> m; return m; }

// Base file name (no extension) for a section, or an error with Wine's codes.
NTSTATUS NlsFileName(ULONG type, ULONG id, std::wstring* name) {
    switch (type) {
    case kNlsSortkeys:
        if (id) return kStatusInvalidParameter1;
        *name = L"sortdefault";
        return st::Success;
    case kNlsCasemap:
        if (id) return st::Unsuccessful;
        *name = L"l_intl";
        return st::Success;
    case kNlsCodepage: {
        std::wstring digits = std::to_wstring(id);
        *name = L"c_" + std::wstring(digits.size() < 3 ? 3 - digits.size() : 0, L'0') + digits;
        return st::Success;
    }
    case kNlsNormalize:
        switch (id) {
        case kNormC: *name = L"normnfc"; return st::Success;
        case kNormD: *name = L"normnfd"; return st::Success;
        case kNormKC: *name = L"normnfkc"; return st::Success;
        case kNormKD: *name = L"normnfkd"; return st::Success;
        case kNormIdna: *name = L"normidna"; return st::Success;
        default: return st::ObjectNameNotFound;   // Wine: no file for this form
        }
    default:
        return kStatusInvalidParameter1;
    }
}

NTSTATUS StatusFromLastError(DWORD err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return st::ObjectNameNotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return kStatusAccessDenied;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return st::NoMemory;
    default:
        return st::Unsuccessful;
    }
}

// Opens one guest-path file and creates a read-only mapping object for it.
NTSTATUS CreateSectionFor(const std::wstring& guest_path, NlsSection* out) {
    std::wstring host;
    NTSTATUS s = TranslateGuestWin32Path(guest_path, &host);
    if (!NtSuccess(s)) return s;
    HANDLE file = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        TRACE(nls, "%ls -> %ls: error %lu", guest_path.c_str(), host.c_str(), err);
        return StatusFromLastError(err);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || static_cast<ULONGLONG>(size.QuadPart) > (1ull << 30)) {
        const DWORD err = GetLastError();
        CloseHandle(file);
        WARN(nls, "%ls: unusable size", host.c_str());
        return size.QuadPart > 0 ? st::Unsuccessful : StatusFromLastError(err);
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(file);   // the mapping object keeps the file open
    if (!mapping) return StatusFromLastError(err);
    out->mapping = mapping;
    out->size = static_cast<SIZE_T>(size.QuadPart);
    TRACE(nls, "%ls -> %ls (%llu bytes)", guest_path.c_str(), host.c_str(),
          static_cast<unsigned long long>(size.QuadPart));
    return st::Success;
}

// Looks up (or creates) the section for a file name under the guest paths in
// `dirs`, in order; only "not found" moves on to the next directory.
NTSTATUS GetSection(std::uint64_t key, const std::wstring& name, const wchar_t* const* dirs, NlsSection* out) {
    AcquireSRWLockExclusive(&g_nls_lock);
    auto it = Sections().find(key);
    if (it != Sections().end()) {
        *out = it->second;
        ReleaseSRWLockExclusive(&g_nls_lock);
        return st::Success;
    }
    NTSTATUS s = st::ObjectNameNotFound;
    NlsSection sec;
    for (const wchar_t* const* d = dirs; *d; ++d) {
        s = CreateSectionFor(std::wstring(*d) + name + L".nls", &sec);
        if (s != st::ObjectNameNotFound) break;
    }
    if (NtSuccess(s)) {
        Sections()[key] = sec;
        *out = sec;
    }
    ReleaseSRWLockExclusive(&g_nls_lock);
    return s;
}

NTSTATUS MapReadOnly(const NlsSection& sec, void** base) {
    void* view = MapViewOfFile(sec.mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) return StatusFromLastError(GetLastError());
    *base = view;
    return st::Success;
}

const wchar_t* const kSystemDir[] = {L"C:\\Windows\\System32\\", nullptr};
const wchar_t* const kSortingDirs[] = {L"C:\\Windows\\Globalization\\Sorting\\", L"C:\\Windows\\System32\\", nullptr};

NTSTATUS NTAPI Xl_NtGetNlsSectionPtr(ULONG type, ULONG id, PVOID unknown, PVOID* ptr, PSIZE_T size) {
    (void)unknown;
    std::wstring name;
    NTSTATUS s = NlsFileName(type, id, &name);
    if (!NtSuccess(s)) {
        TRACE(nls, "type=%lu id=%lu -> %#lx", type, id, static_cast<unsigned long>(s));
        return s;
    }
    if (!ptr) return st::InvalidParameter;
    NlsSection sec;
    s = GetSection((static_cast<std::uint64_t>(type) << 32) | id, name,
                   type == kNlsSortkeys ? kSortingDirs : kSystemDir, &sec);
    if (NtSuccess(s)) s = MapReadOnly(sec, ptr);
    if (!NtSuccess(s)) {
        WARN(nls, "type=%lu id=%lu (%ls.nls): %#lx", type, id, name.c_str(), static_cast<unsigned long>(s));
        return s;
    }
    if (size) *size = sec.size;
    TRACE(nls, "type=%lu id=%lu (%ls.nls) -> %p, %zu bytes", type, id, name.c_str(), *ptr, sec.size);
    return st::Success;
}

NTSTATUS NTAPI Xl_NtInitializeNlsFiles(PVOID* ptr, PULONG lcid, PLARGE_INTEGER size) {
    // Wine sets the LCID whatever happens to the mapping.
    if (lcid) *lcid = kSystemLcid;
    if (!ptr) return st::InvalidParameter;
    NlsSection sec;
    NTSTATUS s = GetSection(~0ull, L"locale", kSystemDir, &sec);
    if (NtSuccess(s)) s = MapReadOnly(sec, ptr);
    if (!NtSuccess(s)) {
        WARN(nls, "locale.nls: %#lx", static_cast<unsigned long>(s));
        return s;
    }
    if (size) size->QuadPart = static_cast<LONGLONG>(sec.size);
    TRACE(nls, "locale.nls -> %p, %zu bytes, lcid %#lx", *ptr, sec.size, static_cast<unsigned long>(kSystemLcid));
    return st::Success;
}

// ---- Default locale state -------------------------------------------------------
// The values Wine keeps in ntdll's unix/env.c: user and system LCID and the UI
// language. NtSetDefault* store them, NtQueryDefault* answer from them, and no
// host setting is touched. They start at en-US, the machine.json default.
// NtInitializeNlsFiles still reports kSystemLcid whatever is stored here.
constexpr LANGID kSystemLangId = 0x0409;

struct LocaleState {
    LCID user_lcid = kSystemLcid;
    LCID system_lcid = kSystemLcid;
    LANGID ui_language = kSystemLangId;
};
SRWLOCK g_locale_lock = SRWLOCK_INIT;
LocaleState g_locale;

NTSTATUS NTAPI Xl_NtQueryDefaultLocale(BOOLEAN user_profile, PLCID lcid) {
    if (!lcid) return st::InvalidParameter;
    AcquireSRWLockShared(&g_locale_lock);
    const LCID value = user_profile ? g_locale.user_lcid : g_locale.system_lcid;
    ReleaseSRWLockShared(&g_locale_lock);
    *lcid = value;
    TRACE(nls, "NtQueryDefaultLocale(user=%d) -> %#lx", user_profile ? 1 : 0, static_cast<unsigned long>(value));
    return st::Success;
}

NTSTATUS NTAPI Xl_NtSetDefaultLocale(BOOLEAN user_profile, LCID lcid) {
    if (!lcid) return st::InvalidParameter;
    AcquireSRWLockExclusive(&g_locale_lock);
    if (user_profile) g_locale.user_lcid = lcid;
    else g_locale.system_lcid = lcid;
    ReleaseSRWLockExclusive(&g_locale_lock);
    TRACE(nls, "NtSetDefaultLocale(user=%d) <- %#lx", user_profile ? 1 : 0, static_cast<unsigned long>(lcid));
    return st::Success;
}

NTSTATUS NTAPI Xl_NtQueryDefaultUILanguage(LANGID* lang) {
    if (!lang) return st::InvalidParameter;
    AcquireSRWLockShared(&g_locale_lock);
    *lang = g_locale.ui_language;
    ReleaseSRWLockShared(&g_locale_lock);
    return st::Success;
}

NTSTATUS NTAPI Xl_NtSetDefaultUILanguage(LANGID lang) {
    if (!lang) return st::InvalidParameter;
    AcquireSRWLockExclusive(&g_locale_lock);
    g_locale.ui_language = lang;
    ReleaseSRWLockExclusive(&g_locale_lock);
    TRACE(nls, "NtSetDefaultUILanguage <- %#x", static_cast<unsigned>(lang));
    return st::Success;
}

}  // namespace

const ImplDef* NlsImpls() {
    static const ImplDef k[] = {
        {"NtGetNlsSectionPtr", reinterpret_cast<void*>(&Xl_NtGetNlsSectionPtr)},
        {"NtInitializeNlsFiles", reinterpret_cast<void*>(&Xl_NtInitializeNlsFiles)},
        {"NtQueryDefaultLocale", reinterpret_cast<void*>(&Xl_NtQueryDefaultLocale)},
        {"NtSetDefaultLocale", reinterpret_cast<void*>(&Xl_NtSetDefaultLocale)},
        {"NtQueryDefaultUILanguage", reinterpret_cast<void*>(&Xl_NtQueryDefaultUILanguage)},
        {"NtSetDefaultUILanguage", reinterpret_cast<void*>(&Xl_NtSetDefaultUILanguage)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
