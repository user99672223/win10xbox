// File-system translators: guest paths in OBJECT_ATTRIBUTES go through the
// VFS (core/vfs) before the host sees them; names coming back are mapped to
// the guest view. Without the VFS module a minimal fallback maps the guest
// C: drive to config::Current().machine_root (plus drives added with
// SetFallbackDrive) and applies the E1 builtin rule for system DLLs.
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_VFS
#include "win10xbox/vfs.h"
#endif
#include <cstring>
#include <cwctype>
#include <map>

namespace win10xbox::dispatch::detail {
namespace {

bool IStartsWith(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
    return true;
}
#if !WIN10XBOX_HAVE_VFS
bool IEquals(std::wstring_view a, std::wstring_view b) { return a.size() == b.size() && IStartsWith(a, b); }

struct OwnedOA {
    OBJECT_ATTRIBUTES attr{};
    UNICODE_STRING name{};
    std::wstring storage;
};

std::shared_ptr<OwnedOA> MakeOA(const OBJECT_ATTRIBUTES* in, std::wstring path) {
    auto o = std::make_shared<OwnedOA>();
    o->storage = std::move(path);
    o->attr = *in;
    o->name.Buffer = o->storage.data();
    o->name.Length = static_cast<USHORT>(o->storage.size() * sizeof(wchar_t));
    o->name.MaximumLength = o->name.Length;
    o->attr.ObjectName = &o->name;
    return o;
}

SRWLOCK g_drive_lock = SRWLOCK_INIT;
std::map<wchar_t, std::wstring>& FallbackDrives() { static std::map<wchar_t, std::wstring> m; return m; }

std::wstring StripTrailingSlash(std::wstring s) {
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// Host root for a guest drive letter; empty if unmapped.
std::wstring DriveRoot(wchar_t letter) {
    letter = static_cast<wchar_t>(std::towupper(letter));
    if (letter == L'C') return StripTrailingSlash(config::Current().machine_root);
    AcquireSRWLockShared(&g_drive_lock);
    auto it = FallbackDrives().find(letter);
    std::wstring r = it != FallbackDrives().end() ? it->second : std::wstring();
    ReleaseSRWLockShared(&g_drive_lock);
    return r;
}

bool FileExists(const std::wstring& win32) { return GetFileAttributesW(win32.c_str()) != INVALID_FILE_ATTRIBUTES; }

// rest: path below the drive root, starting with '\' or empty.
std::wstring ApplyDllRule(wchar_t letter, std::wstring_view rest) {
    std::wstring root = DriveRoot(letter);
    std::wstring out = root + std::wstring(rest);
    if (std::towupper(letter) != L'C') return out;
    static const std::wstring_view kSys = L"\\windows\\system32\\";
    if (!IStartsWith(rest, kSys)) return out;
    std::wstring_view file = rest.substr(kSys.size());
    if (file.find(L'\\') != std::wstring_view::npos || file.size() < 5 || !IEquals(file.substr(file.size() - 4), L".dll"))
        return out;
    std::wstring builtin = root + L"\\Windows\\builtin\\" + std::wstring(file);
    std::wstring native = root + L"\\Windows\\System32\\" + std::wstring(file);
    std::wstring_view base = file.substr(0, file.size() - 4);
    if (IEquals(base, L"ntdll") || IEquals(base, L"kernel32") || IEquals(base, L"kernelbase")) return builtin;
    if (FileExists(native)) return native;
    if (FileExists(builtin)) return builtin;
    return native;
}

// Win32 host path -> NT host path.
std::wstring Win32ToNt(const std::wstring& w) {
    if (IStartsWith(w, L"\\\\?\\")) return L"\\??\\" + w.substr(4);
    if (w.size() > 2 && w[0] == L'\\' && w[1] == L'\\') return L"\\??\\UNC\\" + w.substr(2);
    return L"\\??\\" + w;
}

NTSTATUS FallbackTranslateWin32(std::wstring_view guest, std::wstring* host) {
    if (IStartsWith(guest, L"\\\\?\\")) guest.remove_prefix(4);
    if (guest.size() < 2 || guest[1] != L':' || !std::iswalpha(guest[0])) return st::NotSupported;
    if (guest.size() > 2 && guest[2] != L'\\' && guest[2] != L'/') return st::ObjectPathNotFound;
    if (DriveRoot(guest[0]).empty()) return st::ObjectPathNotFound;
    *host = ApplyDllRule(guest[0], guest.substr(2));
    return st::Success;
}

NTSTATUS FallbackTranslateNt(std::wstring_view guest, std::wstring* host) {
    static const std::wstring_view kPrefixes[] = {L"\\??\\", L"\\DosDevices\\", L"\\GLOBAL??\\"};
    for (auto p : kPrefixes) {
        if (!IStartsWith(guest, p)) continue;
        std::wstring_view rest = guest.substr(p.size());
        if (rest.size() < 2 || rest[1] != L':' || !std::iswalpha(rest[0])) return st::NotSupported;
        std::wstring win32;
        NTSTATUS s = FallbackTranslateWin32(rest, &win32);
        if (s) return s;
        *host = Win32ToNt(win32);
        return st::Success;
    }
    if (IStartsWith(guest, L"\\SystemRoot\\") || IEquals(guest, L"\\SystemRoot")) {
        std::wstring win32;
        std::wstring g = L"C:\\Windows" + std::wstring(guest.substr(11));
        NTSTATUS s = FallbackTranslateWin32(g, &win32);
        if (s) return s;
        *host = Win32ToNt(win32);
        return st::Success;
    }
    return st::NotSupported;
}
#endif  // !WIN10XBOX_HAVE_VFS

constexpr ULONG kFileNameInformation = 9;
constexpr ULONG kFileNormalizedNameInformation = 48;

bool IsKnownDllsName(const OBJECT_ATTRIBUTES* oa) {
    if (!oa || oa->RootDirectory) return false;
    std::wstring_view n = OAName(oa);
    return IStartsWith(n, L"\\KnownDlls");
}

}  // namespace

std::wstring_view OAName(const OBJECT_ATTRIBUTES* oa) {
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer) return {};
    return std::wstring_view(oa->ObjectName->Buffer, oa->ObjectName->Length / sizeof(wchar_t));
}

void SetFallbackDrive(wchar_t letter, const std::wstring& host_root) {
#if WIN10XBOX_HAVE_VFS
    (void)letter; (void)host_root;
    WARN(dispatch, "SetFallbackDrive ignored: the VFS owns drive mappings (machine.json \"drives\")");
#else
    AcquireSRWLockExclusive(&g_drive_lock);
    FallbackDrives()[static_cast<wchar_t>(std::towupper(letter))] = StripTrailingSlash(host_root);
    ReleaseSRWLockExclusive(&g_drive_lock);
#endif
}

NTSTATUS TranslateGuestNtPath(std::wstring_view guest, std::wstring* host) {
#if WIN10XBOX_HAVE_VFS
    return vfs::TranslateNtPath(guest, host);
#else
    return FallbackTranslateNt(guest, host);
#endif
}

NTSTATUS TranslateGuestWin32Path(std::wstring_view guest, std::wstring* host) {
#if WIN10XBOX_HAVE_VFS
    return vfs::TranslateWin32Path(guest, host);
#else
    return FallbackTranslateWin32(guest, host);
#endif
}

bool UntranslateHostWin32Path(std::wstring_view host, std::wstring* guest) {
#if WIN10XBOX_HAVE_VFS
    return vfs::UntranslateWin32Path(host, guest) == st::Success;
#else
    if (IStartsWith(host, L"\\\\?\\")) host.remove_prefix(4);
    std::vector<std::pair<wchar_t, std::wstring>> drives = {{L'C', DriveRoot(L'C')}};
    AcquireSRWLockShared(&g_drive_lock);
    for (auto& d : FallbackDrives()) drives.emplace_back(d.first, d.second);
    ReleaseSRWLockShared(&g_drive_lock);
    // Longest root wins (a drive mapped inside the machine folder).
    std::size_t best = 0;
    for (auto& [letter, root] : drives) {
        if (root.empty() || !IStartsWith(host, root)) continue;
        if (host.size() > root.size() && host[root.size()] != L'\\') continue;
        if (root.size() < best) continue;
        best = root.size();
        std::wstring rest(host.substr(root.size()));
        *guest = std::wstring(1, letter) + L":" + (rest.empty() ? L"\\" : rest);
    }
    return best != 0;
#endif
}

std::wstring GuestWin32ToDevicePath(std::wstring_view g) {
    if (g.size() < 2 || g[1] != L':') return std::wstring(g);
    int volume = 1 + (std::towupper(g[0]) - L'C');
    if (volume < 1) volume += 26;
    return L"\\Device\\HarddiskVolume" + std::to_wstring(volume) + std::wstring(g.substr(2));
}

// WIN10XBOX_TEST_NO_HOST_PATHS=1 makes HostPathOfHandle fail the way it does in
// the console's AppContainer (GetFinalPathNameByHandleW(VOLUME_NAME_DOS) cannot
// reach the mount manager there), so Wine and desktop-Windows runs can exercise
// the "host path unknown" paths that only the console hits otherwise. Test hook.
static bool HostPathsForcedUnknown() {
    static const bool forced = [] {
        wchar_t v[8];
        const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEST_NO_HOST_PATHS", v, 8);
        return n != 0 && n < 8 && v[0] != L'0';
    }();
    return forced;
}

std::wstring HostPathOfHandle(HANDLE file, DWORD* error) {
    if (error) *error = 0;
    if (HostPathsForcedUnknown()) {
        if (error) *error = ERROR_ACCESS_DENIED;
        return {};
    }
    wchar_t buf[1024];
    DWORD n = GetFinalPathNameByHandleW(file, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= 1024) {
        if (error) *error = n ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        return {};
    }
    std::wstring s(buf, n);
    if (IStartsWith(s, L"\\\\?\\UNC\\")) return L"\\\\" + s.substr(8);
    if (IStartsWith(s, L"\\\\?\\")) return s.substr(4);
    return s;
}

NTSTATUS TranslateOA(const OBJECT_ATTRIBUTES* in, TranslatedOA* out) {
    out->attr = nullptr;
    out->keep.reset();
    if (!in || !in->ObjectName || in->RootDirectory) return st::Success;   // relative to a host handle
#if WIN10XBOX_HAVE_VFS
    auto t = std::make_shared<vfs::TranslatedAttributes>();
    NTSTATUS s = vfs::TranslateObjectAttributes(in, t.get());
    if (s == st::NotSupported) return st::Success;
    if (s) return s;
    if (t->translated) {
        out->attr = &t->attr;
        out->keep = t;
    }
    return st::Success;
#else
    std::wstring host;
    NTSTATUS s = FallbackTranslateNt(OAName(in), &host);
    if (s == st::NotSupported) return st::Success;
    if (s) return s;
    auto o = MakeOA(in, std::move(host));
    out->attr = &o->attr;
    out->keep = o;
    return st::Success;
#endif
}

// ---- Translators ------------------------------------------------------------

NTSTATUS NTAPI Xl_NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                               PLARGE_INTEGER alloc_size, ULONG attributes, ULONG sharing, ULONG disposition,
                               ULONG options, PVOID ea, ULONG ea_length) {
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    return Host().NtCreateFile(handle, access, t.Get(attr), io, alloc_size, attributes, sharing, disposition, options,
                               ea, ea_length);
}

NTSTATUS NTAPI Xl_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                             ULONG sharing, ULONG options) {
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    return Host().NtOpenFile(handle, access, t.Get(attr), io, sharing, options);
}

static NTSTATUS NTAPI Xl_NtQueryAttributesFile(POBJECT_ATTRIBUTES attr, PVOID info) {
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    return Host().NtQueryAttributesFile(t.Get(attr), info);
}

static NTSTATUS NTAPI Xl_NtQueryFullAttributesFile(POBJECT_ATTRIBUTES attr, PVOID info) {
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    return Host().NtQueryFullAttributesFile(t.Get(attr), info);
}

static NTSTATUS NTAPI Xl_NtDeleteFile(POBJECT_ATTRIBUTES attr) {
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    return Host().NtDeleteFile(t.Get(attr));
}

static NTSTATUS NTAPI Xl_NtQueryInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer, ULONG length,
                                                ULONG klass) {
    NTSTATUS s = Host().NtQueryInformationFile(file, io, buffer, length, klass);
    if (klass != kFileNameInformation && klass != kFileNormalizedNameInformation) return s;
    if (s != st::Success && s != st::BufferOverflow) return s;
    std::wstring host = HostPathOfHandle(file), guest;
    if (host.empty() || !UntranslateHostWin32Path(host, &guest) || guest.size() < 2) {
        if (!host.empty()) WARN(dispatch, "file name %ls is outside the guest's drives", host.c_str());
        return s;
    }
    // FILE_NAME_INFORMATION: drive-relative name ("\Windows\x.dll").
    std::wstring_view name = std::wstring_view(guest).substr(2);
    const ULONG bytes = static_cast<ULONG>(name.size() * sizeof(wchar_t));
    auto* out = static_cast<std::uint8_t*>(buffer);
    const ULONG room = length > 4 ? length - 4 : 0;
    const ULONG copy = bytes < room ? bytes : room & ~1u;
    *reinterpret_cast<ULONG*>(out) = bytes;
    std::memcpy(out + 4, name.data(), copy);
    io->Information = 4 + copy;
    s = copy < bytes ? st::BufferOverflow : st::Success;
    io->Status = s;
    return s;
}

static NTSTATUS NTAPI Xl_NtOpenDirectoryObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr) {
    // E3: no KnownDlls, so Wine's loader searches the file system through the VFS.
    if (IsKnownDllsName(attr)) return st::ObjectNameNotFound;
    return Host().NtOpenDirectoryObject(handle, access, attr);
}

static NTSTATUS NTAPI Xl_NtOpenSymbolicLinkObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr) {
    if (IsKnownDllsName(attr)) return st::ObjectNameNotFound;
    return Host().NtOpenSymbolicLinkObject(handle, access, attr);
}

const ImplDef* FileImpls() {
    static const ImplDef k[] = {
        {"NtCreateFile", reinterpret_cast<void*>(&Xl_NtCreateFile)},
        {"NtOpenFile", reinterpret_cast<void*>(&Xl_NtOpenFile)},
        {"NtQueryAttributesFile", reinterpret_cast<void*>(&Xl_NtQueryAttributesFile)},
        {"NtQueryFullAttributesFile", reinterpret_cast<void*>(&Xl_NtQueryFullAttributesFile)},
        {"NtDeleteFile", reinterpret_cast<void*>(&Xl_NtDeleteFile)},
        {"NtQueryInformationFile", reinterpret_cast<void*>(&Xl_NtQueryInformationFile)},
        {"NtOpenDirectoryObject", reinterpret_cast<void*>(&Xl_NtOpenDirectoryObject)},
        {"NtOpenSymbolicLinkObject", reinterpret_cast<void*>(&Xl_NtOpenSymbolicLinkObject)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
