// File-system translators: guest paths in OBJECT_ATTRIBUTES go through the
// VFS (core/vfs) before the host sees them; names coming back are mapped to
// the guest view. Without the VFS module a minimal fallback maps the guest
// C: drive to config::Current().machine_root (plus drives added with
// SetFallbackDrive) and applies the E1 builtin rule for system DLLs.
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_AFD
#include "win10xbox/afd.h"
#endif
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
    // the GUI stack is Wine's too: core/user implements Wine's win32u contract (E1, docs/GUI.md)
    if (IEquals(base, L"win32u") || IEquals(base, L"user32") || IEquals(base, L"gdi32") || IEquals(base, L"imm32"))
        return builtin;
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
constexpr ULONG kFileBasicInformation = 4;
constexpr ULONG kFileRenameInformation = 10;
constexpr ULONG kFileLinkInformation = 11;
constexpr ULONG kFileRenameInformationBypassAccessCheck = 56;
constexpr ULONG kFileLinkInformationBypassAccessCheck = 57;
constexpr ULONG kFileRenameInformationEx = 65;
constexpr ULONG kFileRenameInformationExBypassAccessCheck = 66;
constexpr ULONG kFileLinkInformationEx = 72;
constexpr ULONG kFileLinkInformationExBypassAccessCheck = 73;

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

#if WIN10XBOX_HAVE_VFS
namespace {
// A name relative to a directory handle opens the file the absolute name opens, so it has to see the same machine:
// the builtin DLL rule for C:\Windows\System32\*.dll and the name checks live in the VFS, which only sees absolute
// names. Builds that absolute guest NT name from the directory's host path.
//   STATUS_SUCCESS       *nt is the absolute name
//   STATUS_NOT_SUPPORTED the host resolves the name itself, as it always did: the handle is not a directory with a
//                        known guest path (the console's AppContainer cannot ask), or the name is rooted
//   other                the name is invalid under a file-system directory: an empty, "." or ".." component, or "/"
//                        (the answer of NTFS; a host that predates it says PATH_NOT_FOUND for ".\x")
NTSTATUS RootRelativeGuestName(const OBJECT_ATTRIBUTES* in, std::wstring* nt) {
    std::wstring_view rel = OAName(in);
    if (rel.empty() || rel.front() == L'\\') return st::NotSupported;
    std::wstring host = HostPathOfHandle(in->RootDirectory);
    if (host.empty()) return st::NotSupported;
    const DWORD attrs = GetFileAttributesW(host.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return st::NotSupported;
    std::wstring guest;
    if (!UntranslateHostWin32Path(host, &guest) || guest.size() < 3 || guest[1] != L':') return st::NotSupported;
    std::wstring_view rest = rel;
    if (rest.back() == L'\\') rest.remove_suffix(1);
    for (std::size_t b = 0; b <= rest.size();) {
        std::size_t e = rest.find(L'\\', b);
        if (e == std::wstring_view::npos) e = rest.size();
        std::wstring_view c = rest.substr(b, e - b);
        if (c.empty() || c == L"." || c == L".." || c.find(L'/') != std::wstring_view::npos)
            return st::ObjectNameInvalid;
        b = e + 1;
    }
    *nt = L"\\??\\" + guest;
    if (nt->back() != L'\\') *nt += L'\\';
    nt->append(rel);
    return st::Success;
}
}  // namespace
#endif

NTSTATUS TranslateOA(const OBJECT_ATTRIBUTES* in, TranslatedOA* out) {
    out->attr = nullptr;
    out->keep.reset();
    if (!in || !in->ObjectName) return st::Success;
#if WIN10XBOX_HAVE_VFS
    if (in->RootDirectory) {
        std::wstring nt;
        if (in->Length < sizeof(OBJECT_ATTRIBUTES)) return st::Success;
        if (NTSTATUS rs = RootRelativeGuestName(in, &nt)) return rs == st::NotSupported ? st::Success : rs;
        UNICODE_STRING us{};
        us.Buffer = nt.data();
        us.Length = static_cast<USHORT>(nt.size() * sizeof(wchar_t));
        us.MaximumLength = us.Length;
        OBJECT_ATTRIBUTES abs = *in;
        abs.RootDirectory = nullptr;
        abs.ObjectName = &us;
        auto t = std::make_shared<vfs::TranslatedAttributes>();
        NTSTATUS s = vfs::TranslateObjectAttributes(&abs, t.get());
        if (s) return s;
        if (t->translated) {
            out->attr = &t->attr;
            out->keep = t;
        }
        return st::Success;
    }
#else
    if (in->RootDirectory) return st::Success;   // relative to a host handle
#endif
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
#if WIN10XBOX_HAVE_AFD
    if (afd::IsAfdDeviceName(attr)) return afd::Open(handle, io);   // sockets: core/afd
#endif
    TranslatedOA t;
    if (NTSTATUS s = TranslateOA(attr, &t)) return s;
    // Wine's SetFileAttributesW (git hides ".git" with it) opens with SYNCHRONIZE alone and then sets
    // FileBasicInformation, which Wine's server allows; Windows demands FILE_WRITE_ATTRIBUTES for that. Ask for it,
    // and fall back to the caller's access when the file will not grant it (a read-only ACL).
    if (access == SYNCHRONIZE) {
        constexpr ACCESS_MASK kFileWriteAttributes = 0x100;
        NTSTATUS s = Host().NtOpenFile(handle, access | kFileWriteAttributes, t.Get(attr), io, sharing, options);
        if (s != st::AccessDenied) return s;
    }
    return Host().NtOpenFile(handle, access, t.Get(attr), io, sharing, options);
}

// Pipes live in the host's own \\Device\\NamedPipe: the name is not a file-system path and passes unchanged. What is
// translated is the default time-out. Wine's kernelbase (CreatePipe) passes 0, its NMPWAIT_USE_DEFAULT_WAIT, meaning
// "the pipe's default"; Windows' npfs refuses an explicit zero interval, and CreatePipe retries until the call succeeds
// (hence an endless loop of STATUS_INVALID_PARAMETER). kernel32 passes the documented default, 50 ms, for the same case.
// The ULONG parameters are also handed on with clean upper halves (the guest's 64-bit argument slots are not).
static NTSTATUS NTAPI Xl_NtCreateNamedPipeFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr,
                                               PIO_STATUS_BLOCK io, ULONG sharing, ULONG disposition, ULONG options,
                                               ULONG pipe_type, ULONG read_mode, ULONG completion_mode,
                                               ULONG max_instances, ULONG inbound_quota, ULONG outbound_quota,
                                               PLARGE_INTEGER timeout) {
    LARGE_INTEGER default_wait;
    if (timeout && timeout->QuadPart == 0) {
        default_wait.QuadPart = -50 * 10000;   // 50 ms, relative
        timeout = &default_wait;
    }
    return Host().NtCreateNamedPipeFile(handle, access, attr, io, sharing, disposition, options, pipe_type, read_mode,
                                        completion_mode, max_instances, inbound_quota, outbound_quota, timeout);
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

// Wine's GetFinalPathNameByHandleW (git's getcwd and realpath) asks for ObjectNameInformation and only accepts an
// NT path in the "\\??\\C:\\..." form, which is what Wine's own ntdll answers for a file handle. The host answers
// "\\Device\\HarddiskVolumeN\\<host path>"; give a file handle the guest's name instead. Other objects, and files
// outside every guest drive, keep the host's answer.
static NTSTATUS NTAPI Xl_NtQueryObject(HANDLE h, ULONG klass, PVOID buffer, ULONG length, PULONG ret_len) {
    constexpr ULONG kObjectNameInformation = 1;
    if (klass == kObjectNameInformation) {
        alignas(8) std::uint8_t probe[4096];
        ULONG probe_len = 0;
        if (Host().NtQueryObject(h, klass, probe, sizeof probe, &probe_len) == st::Success) {
            const auto* us = reinterpret_cast<const UNICODE_STRING*>(probe);
            const std::wstring_view name(us->Buffer ? us->Buffer : L"", us->Buffer ? us->Length / sizeof(wchar_t) : 0);
            if (IStartsWith(name, L"\\Device\\HarddiskVolume") || IStartsWith(name, L"\\Device\\Mup")) {
                std::wstring host = HostPathOfHandle(h), guest;
                if (!host.empty() && UntranslateHostWin32Path(host, &guest) && guest.size() >= 2) {
                    const std::wstring nt = L"\\??\\" + guest;
                    const ULONG bytes = static_cast<ULONG>(nt.size() * sizeof(wchar_t));
                    const ULONG need = static_cast<ULONG>(sizeof(UNICODE_STRING)) + bytes + sizeof(wchar_t);
                    if (ret_len) *ret_len = need;
                    if (length < need) return st::BufferOverflow;
                    auto* out = static_cast<UNICODE_STRING*>(buffer);
                    auto* text = reinterpret_cast<wchar_t*>(out + 1);
                    std::memcpy(text, nt.c_str(), bytes + sizeof(wchar_t));
                    out->Buffer = text;
                    out->Length = static_cast<USHORT>(bytes);
                    out->MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
                    return st::Success;
                }
            }
        }
    }
    return Host().NtQueryObject(h, klass, buffer, length, ret_len);
}

// FILE_RENAME_INFORMATION / FILE_LINK_INFORMATION carry the target as an NT path inside the buffer (Wine's
// MoveFileExW and CreateHardLinkW send "\??\C:\..."), so the host would look for it on its own C: drive. Same
// layout for both: { BOOLEAN/ULONG Flags; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[] }.
static NTSTATUS NTAPI Xl_NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer, ULONG length,
                                              ULONG klass) {
    switch (klass) {
    case kFileRenameInformation: case kFileLinkInformation:
    case kFileRenameInformationBypassAccessCheck: case kFileLinkInformationBypassAccessCheck:
    case kFileRenameInformationEx: case kFileRenameInformationExBypassAccessCheck:
    case kFileLinkInformationEx: case kFileLinkInformationExBypassAccessCheck:
        break;
    case kFileBasicInformation: {
        // Wine's SetFileAttributesW opens the file with SYNCHRONIZE only and sets FileBasicInformation on that handle
        // (its server checks no access there); Windows wants FILE_WRITE_ATTRIBUTES on the handle. When the host
        // refuses, set the information through a handle of the same file that has it (what ReOpenFile does).
        NTSTATUS s = Host().NtSetInformationFile(file, io, buffer, length, klass);
        if (s != st::AccessDenied) return s;
        UNICODE_STRING empty = {};
        OBJECT_ATTRIBUTES oa = {};
        oa.Length = sizeof oa;
        oa.RootDirectory = file;
        oa.ObjectName = &empty;
        oa.Attributes = 0x40;   // OBJ_CASE_INSENSITIVE
        HANDLE again = nullptr;
        IO_STATUS_BLOCK open_io = {};
        constexpr ULONG kShareAll = 7, kOpenReparsePoint = 0x200000, kSynchronousIoNonalert = 0x20,
                        kBackupIntent = 0x4000;
        if (Host().NtOpenFile(&again, FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, &oa, &open_io, kShareAll,
                              kOpenReparsePoint | kSynchronousIoNonalert | kBackupIntent) != st::Success)
            return s;
        s = Host().NtSetInformationFile(again, io, buffer, length, klass);
        Host().NtClose(again);
        return s;
    }
    default:
        return Host().NtSetInformationFile(file, io, buffer, length, klass);
    }
    constexpr ULONG kHeader = 20;   // offsetof(FILE_RENAME_INFORMATION, FileName) on x64
    if (!buffer || length < kHeader) return Host().NtSetInformationFile(file, io, buffer, length, klass);
    auto* in = static_cast<const std::uint8_t*>(buffer);
    HANDLE root;
    ULONG name_bytes;
    std::memcpy(&root, in + 8, sizeof root);
    std::memcpy(&name_bytes, in + 16, sizeof name_bytes);
    if (root || name_bytes > length - kHeader)   // relative to a host handle, or malformed: the host decides
        return Host().NtSetInformationFile(file, io, buffer, length, klass);
    std::wstring host;
    NTSTATUS s = TranslateGuestNtPath(std::wstring_view(reinterpret_cast<const wchar_t*>(in + kHeader), name_bytes / 2), &host);
    if (s == st::NotSupported) return Host().NtSetInformationFile(file, io, buffer, length, klass);
    if (s) return s;
    const ULONG new_bytes = static_cast<ULONG>(host.size() * sizeof(wchar_t));
    std::vector<std::uint8_t> out(kHeader + new_bytes + sizeof(wchar_t), 0);
    std::memcpy(out.data(), in, 16);   // flags + RootDirectory (padding included)
    std::memcpy(out.data() + 16, &new_bytes, sizeof new_bytes);
    std::memcpy(out.data() + kHeader, host.data(), new_bytes);
    TRACE(dispatch, "NtSetInformationFile class %lu: target '%.*ls' -> '%ls'", static_cast<unsigned long>(klass),
          static_cast<int>(name_bytes / 2), reinterpret_cast<const wchar_t*>(in + kHeader), host.c_str());
    return Host().NtSetInformationFile(file, io, out.data(), static_cast<ULONG>(out.size()), klass);
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

// ---- Byte-range locks -------------------------------------------------------
// Wine's kernelbase (LockFile/LockFileEx) calls NtLockFile with IoStatusBlock == NULL, which Wine's own ntdll accepts
// and Windows' does not: the I/O manager probes the pointer and answers STATUS_ACCESS_VIOLATION. SQLite's win32 VFS
// takes its locks with LockFileEx, so on real Windows every lock failed and it reported "database is locked".
// Windows hosts therefore get a block of ours (heap, because a lock still PENDING on an overlapped handle completes into
// it later; it is released unless the call came back pending). A Wine host is the reverse: Wine's unix NtLockFile
// answers STATUS_NOT_IMPLEMENTED to any non-NULL IoStatusBlock, and Wine 9's NtUnlockFile does the same while Wine
// 11's dereferences it unconditionally, so on a Wine host NtLockFile is called with NULL and the guest's block is
// filled in afterwards, and NtUnlockFile tries the block first and falls back to NULL.
using PFN_NtLockFile = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PLARGE_INTEGER, PLARGE_INTEGER,
                                        PULONG, BOOLEAN, BOOLEAN);
using PFN_NtUnlockFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PLARGE_INTEGER, PLARGE_INTEGER, PULONG);

static bool HostIsWine() { return HostExport("wine_get_version") != nullptr; }

static NTSTATUS NTAPI Xl_NtLockFile(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                                    PLARGE_INTEGER offset, PLARGE_INTEGER length, PULONG key, BOOLEAN fail_immediately,
                                    BOOLEAN exclusive) {
    auto host = reinterpret_cast<PFN_NtLockFile>(HostExport("NtLockFile"));
    if (!host) return st::NotImplemented;
    if (HostIsWine()) {
        NTSTATUS s = host(file, event, apc, apc_context, nullptr, offset, length, key, fail_immediately, exclusive);
        if (io) { io->Status = s; io->Information = 0; }
        return s;
    }
    if (io) return host(file, event, apc, apc_context, io, offset, length, key, fail_immediately, exclusive);
    auto* ours = new IO_STATUS_BLOCK{};
    NTSTATUS s = host(file, event, apc, apc_context, ours, offset, length, key, fail_immediately, exclusive);
    if (s == st::Pending) WARN(dispatch, "NtLockFile without an IoStatusBlock is pending; the lock completes asynchronously");
    else delete ours;
    return s;
}

static NTSTATUS NTAPI Xl_NtUnlockFile(HANDLE file, PIO_STATUS_BLOCK io, PLARGE_INTEGER offset, PLARGE_INTEGER length,
                                      PULONG key) {
    auto host = reinterpret_cast<PFN_NtUnlockFile>(HostExport("NtUnlockFile"));
    if (!host) return st::NotImplemented;
    NTSTATUS s = host(file, io, offset, length, key);
    if (s == st::NotImplemented && io && HostIsWine()) {
        s = host(file, nullptr, offset, length, key);
        io->Status = s;
        io->Information = 0;
    }
    return s;
}

const ImplDef* FileImpls() {
    static const ImplDef k[] = {
        {"NtCreateFile", reinterpret_cast<void*>(&Xl_NtCreateFile)},
        {"NtOpenFile", reinterpret_cast<void*>(&Xl_NtOpenFile)},
        {"NtCreateNamedPipeFile", reinterpret_cast<void*>(&Xl_NtCreateNamedPipeFile)},
        {"NtQueryAttributesFile", reinterpret_cast<void*>(&Xl_NtQueryAttributesFile)},
        {"NtQueryFullAttributesFile", reinterpret_cast<void*>(&Xl_NtQueryFullAttributesFile)},
        {"NtDeleteFile", reinterpret_cast<void*>(&Xl_NtDeleteFile)},
        {"NtQueryInformationFile", reinterpret_cast<void*>(&Xl_NtQueryInformationFile)},
        {"NtSetInformationFile", reinterpret_cast<void*>(&Xl_NtSetInformationFile)},
        {"NtQueryObject", reinterpret_cast<void*>(&Xl_NtQueryObject)},
        {"NtOpenDirectoryObject", reinterpret_cast<void*>(&Xl_NtOpenDirectoryObject)},
        {"NtOpenSymbolicLinkObject", reinterpret_cast<void*>(&Xl_NtOpenSymbolicLinkObject)},
        {"NtLockFile", reinterpret_cast<void*>(&Xl_NtLockFile)},
        {"NtUnlockFile", reinterpret_cast<void*>(&Xl_NtUnlockFile)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
