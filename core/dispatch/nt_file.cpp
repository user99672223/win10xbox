// File-system translators: guest paths in OBJECT_ATTRIBUTES go through the
// VFS (core/vfs) before the host sees them; names coming back are mapped to
// the guest view. Without the VFS module a minimal fallback maps the guest
// C: drive to config::Current().machine_root (plus drives added with
// SetFallbackDrive) and applies the E1 builtin rule for system DLLs.
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif
#if WIN10XBOX_HAVE_AFD
#include "win10xbox/afd.h"
#endif
#if WIN10XBOX_HAVE_VFS
#include "win10xbox/vfs.h"
#endif
#include <cstring>
#include <cwctype>
#include <functional>
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
constexpr ULONG kFileAllInformation = 18;
constexpr ULONG kFileOpenByFileId = 0x00002000;   // CreateOptions: the name is an 8 or 16 byte file ID, not a path

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

static NTSTATUS TranslateCaptured(const OBJECT_ATTRIBUTES* in, TranslatedOA* out, ULONG create_options);

// The caller's attributes and name are read once, here; what follows works on the copy. Memory that cannot be
// read is an access violation, a Length the structure may not have is the host's to refuse. A name that goes
// through the object manager tree is then resolved there; what it leads to outside the tree (a guest-made link to
// \Device\Null) is translated like any other name.
NTSTATUS TranslateOA(const OBJECT_ATTRIBUTES* user_in, TranslatedOA* out, ULONG create_options) {
    out->attr = nullptr;
    out->keep.reset();
    if (!user_in) return st::Success;
    probe::ObjectAttributes cap;
    if (NTSTATUS cs = probe::CaptureObjectAttributes(user_in, &cap)) return cs == st::InvalidParameter ? st::Success : cs;
    const OBJECT_ATTRIBUTES* in = &cap.oa;
    if (!in->ObjectName) return st::Success;
    NTSTATUS route_status = st::Success;
    std::wstring absolute;
    switch (ResolveFileNameThroughNamespace(in, &route_status, &absolute)) {
    case 1:
        return route_status;
    case 2: {
        struct Routed {
            OBJECT_ATTRIBUTES oa;
            UNICODE_STRING us;
            std::wstring name;
            TranslatedOA inner;
        };
        auto r = std::make_shared<Routed>();
        r->name = std::move(absolute);
        r->us.Buffer = r->name.data();
        r->us.Length = static_cast<USHORT>(r->name.size() * sizeof(wchar_t));
        r->us.MaximumLength = static_cast<USHORT>(r->us.Length + sizeof(wchar_t));
        r->oa = *in;
        r->oa.RootDirectory = nullptr;
        r->oa.ObjectName = &r->us;
        if (NTSTATUS s = TranslateCaptured(&r->oa, &r->inner, create_options)) return s;
        out->attr = r->inner.attr ? r->inner.attr : &r->oa;
        out->keep = r;
        return st::Success;
    }
    default:
        return TranslateCaptured(in, out, create_options);
    }
}

static NTSTATUS TranslateCaptured(const OBJECT_ATTRIBUTES* in, TranslatedOA* out, ULONG create_options) {
    out->attr = nullptr;
    out->keep.reset();
    if (!in->ObjectName) return st::Success;
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
        NTSTATUS s = vfs::TranslateObjectAttributes(&abs, t.get(), create_options);
        if (s) return s;
        if (t->translated) {
            out->attr = &t->attr;
            out->keep = t;
        }
        return st::Success;
    }
#else
    (void)create_options;
    if (in->RootDirectory) return st::Success;   // relative to a host handle
#endif
#if WIN10XBOX_HAVE_VFS
    auto t = std::make_shared<vfs::TranslatedAttributes>();
    NTSTATUS s = vfs::TranslateObjectAttributes(in, t.get(), create_options);
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
    // FILE_OPEN_BY_FILE_ID: the "name" is a binary file ID (relative to a directory or volume handle), not a path.
    if (!(options & kFileOpenByFileId))
        if (NTSTATUS s = TranslateOA(attr, &t, options)) return s;
    NTSTATUS r = Host().NtCreateFile(handle, access, t.Get(attr), io, alloc_size, attributes, sharing, disposition,
                                     options, ea, ea_length);
    // FILE_CREATE on a name that already exists is STATUS_OBJECT_NAME_COLLISION on Windows, directory or not (the
    // host answers FILE_IS_A_DIRECTORY for a directory). Wine's GetTempFileNameW stops on any other error and
    // would hand back the directory's name as a fresh file.
    constexpr ULONG kFileCreate = 2, kFileNonDirectoryFile = 0x40;
    if (r == static_cast<NTSTATUS>(0xC00000BA) && disposition == kFileCreate && (options & kFileNonDirectoryFile))
        r = static_cast<NTSTATUS>(0xC0000035);
    return r;
}

NTSTATUS NTAPI Xl_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                             ULONG sharing, ULONG options) {
#if WIN10XBOX_HAVE_AFD
    if (afd::IsAfdDeviceName(attr)) return afd::Open(handle, io);   // sockets: core/afd
#endif
    TranslatedOA t;
    if (!(options & kFileOpenByFileId))   // the name is a file ID, see Xl_NtCreateFile
        if (NTSTATUS s = TranslateOA(attr, &t, options)) return s;
    // Wine's SetFileAttributesW (git hides ".git" with it) opens with SYNCHRONIZE alone and then sets
    // FileBasicInformation, which Wine's server allows; Windows demands FILE_WRITE_ATTRIBUTES for that. Ask for it,
    // and fall back to the caller's access when the file will not grant it (a read-only ACL).
    if (access == SYNCHRONIZE) {
        constexpr ACCESS_MASK kFileWriteAttributes = 0x100;
        NTSTATUS s = Host().NtOpenFile(handle, access | kFileWriteAttributes, t.Get(attr), io, sharing, options);
        if (s != st::AccessDenied) return s;
    }
    NTSTATUS r = Host().NtOpenFile(handle, access, t.Get(attr), io, sharing, options);
    return r;
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
    if (timeout) {
        // Read once; a pointer that cannot be read is an access violation (the host would say the same).
        LARGE_INTEGER requested;
        if (NTSTATUS cs = probe::Load<LARGE_INTEGER>(timeout, &requested)) return cs;
        if (requested.QuadPart == 0) {
            default_wait.QuadPart = -50 * 10000;   // 50 ms, relative
            timeout = &default_wait;
        }
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

// Where the FILE_NAME_INFORMATION lives in the structure of `klass` (FileNameInformation, its normalized twin, and
// FileAllInformation, whose last member it is) and the smallest buffer Windows accepts for the class (sizeof the
// structure, so room for one character of the name).
static bool NameInfoLayout(ULONG klass, ULONG* info_offset, ULONG* min_length) {
    switch (klass) {
    case kFileNameInformation: case kFileNormalizedNameInformation: *info_offset = 0; *min_length = 8; return true;
    case kFileAllInformation: *info_offset = 96; *min_length = 104; return true;
    default: return false;
    }
}

// The name in these classes is the host's path of the file on its volume, and the host writes all of it: the guest's
// view of the name is a different string of a different length, so the host answers into a buffer of ours (a path is
// at most 32767 characters) and the reply to the caller is built from that: the other members as the host answered
// them, FileNameLength and FileName in the guest's view, the length checks and status of the caller's own buffer
// (STATUS_BUFFER_OVERFLOW with as many whole characters as fit, Information the bytes written).
static NTSTATUS NTAPI Xl_NtQueryInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer, ULONG length,
                                                ULONG klass) {
    ULONG info_off = 0, min_length = 0;
    if (!NameInfoLayout(klass, &info_off, &min_length))
        return Host().NtQueryInformationFile(file, io, buffer, length, klass);
    if (length < min_length) return st::InfoLengthMismatch;
    const ULONG name_off = info_off + 4;   // offsetof(FILE_NAME_INFORMATION, FileName) within the structure
    constexpr ULONG kMaxNameBytes = 65536;
    constexpr NTSTATUS kUntouched = static_cast<NTSTATUS>(0x0DEFACED);
    std::vector<std::uint8_t> reply(name_off + kMaxNameBytes);
    IO_STATUS_BLOCK host_io;
    host_io.Status = kUntouched;
    host_io.Information = 0;
    NTSTATUS s = Host().NtQueryInformationFile(file, &host_io, reply.data(), static_cast<ULONG>(reply.size()), klass);
    if (s != st::Success && s != st::BufferOverflow) {
        if (host_io.Status != kUntouched)
            if (NTSTATUS ws = probe::Store<IO_STATUS_BLOCK>(io, host_io)) return ws;
        return s;
    }
    ULONG host_bytes = 0;
    std::memcpy(&host_bytes, reply.data() + info_off, sizeof host_bytes);
    if (host_bytes > kMaxNameBytes) host_bytes = kMaxNameBytes;
    std::wstring name(host_bytes / sizeof(wchar_t), L'\0');
    std::memcpy(name.data(), reply.data() + name_off, name.size() * sizeof(wchar_t));
    std::wstring host = HostPathOfHandle(file), guest;
    if (!host.empty()) {
        if (UntranslateHostWin32Path(host, &guest) && guest.size() >= 2)
            name = guest.substr(2);   // drive-relative: "\Windows\x.dll"
        else
            WARN(dispatch, "file name %ls is outside the guest's drives", host.c_str());
    }
    const ULONG bytes = static_cast<ULONG>(name.size() * sizeof(wchar_t));
    const ULONG room = length - name_off;
    const ULONG copy = bytes < room ? bytes : room & ~1u;
    std::vector<std::uint8_t> image(name_off + copy);
    std::memcpy(image.data(), reply.data(), name_off);   // the other members of the structure, as the host answered them
    std::memcpy(image.data() + info_off, &bytes, sizeof bytes);
    std::memcpy(image.data() + name_off, name.data(), copy);
    if (NTSTATUS ws = probe::CopyToUser(buffer, image.data(), image.size())) return ws;
    s = copy < bytes ? st::BufferOverflow : st::Success;
    IO_STATUS_BLOCK done{};
    done.Status = s;
    done.Information = name_off + copy;
    if (NTSTATUS ws = probe::Store<IO_STATUS_BLOCK>(io, done)) return ws;
    return s;
}

// Wine's GetFinalPathNameByHandleW (git's getcwd and realpath) asks for ObjectNameInformation and only accepts an
// NT path in the "\\??\\C:\\..." form, which is what Wine's own ntdll answers for a file handle. The host answers
// "\\Device\\HarddiskVolumeN\\<host path>"; give a file handle the guest's name instead. Other objects, and files
// outside every guest drive, keep the host's answer.
NTSTATUS NTAPI QueryObjectHostFile(HANDLE h, ULONG klass, PVOID buffer, ULONG length, PULONG ret_len) {
    constexpr ULONG kObjectNameInformation = 1;
#if WIN10XBOX_HAVE_REGISTRY
    if (registry::IsKeyHandle(h)) return registry::Reg_NtQueryObject(h, klass, buffer, length, ret_len);   // a key is no host object
#endif
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
                    if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
                    if (ret_len)
                        if (NTSTATUS ps = probe::Store<ULONG>(ret_len, need)) return ps;
                    if (length < need) return st::BufferOverflow;
                    std::vector<std::uint8_t> image(need);
                    auto* out = reinterpret_cast<UNICODE_STRING*>(image.data());
                    std::memcpy(image.data() + sizeof(UNICODE_STRING), nt.c_str(), bytes + sizeof(wchar_t));
                    out->Buffer = reinterpret_cast<wchar_t*>(static_cast<std::uint8_t*>(buffer) + sizeof(UNICODE_STRING));
                    out->Length = static_cast<USHORT>(bytes);
                    out->MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
                    return probe::CopyToUser(buffer, image.data(), image.size());
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
    // The caller's header and target name are copied once; a buffer that cannot be read is an access violation.
    std::uint8_t header[kHeader];
    if (NTSTATUS cs = probe::CopyFromUser(header, buffer, kHeader)) return cs;
    HANDLE root;
    ULONG name_bytes;
    std::memcpy(&root, header + 8, sizeof root);
    std::memcpy(&name_bytes, header + 16, sizeof name_bytes);
    if (root || name_bytes > length - kHeader)   // relative to a host handle, or malformed: the host decides
        return Host().NtSetInformationFile(file, io, buffer, length, klass);
    std::wstring target(name_bytes / 2, L'\0');
    if (NTSTATUS cs = probe::CopyFromUser(target.data(), static_cast<const std::uint8_t*>(buffer) + kHeader,
                                          target.size() * sizeof(wchar_t)))
        return cs;
    const std::uint8_t* in = header;
    std::wstring host;
    NTSTATUS s = TranslateGuestNtPath(target, &host);
    if (s == st::NotSupported) return Host().NtSetInformationFile(file, io, buffer, length, klass);
    if (s) return s;
    const ULONG new_bytes = static_cast<ULONG>(host.size() * sizeof(wchar_t));
    std::vector<std::uint8_t> out(kHeader + new_bytes + sizeof(wchar_t), 0);
    std::memcpy(out.data(), in, 16);   // flags + RootDirectory (padding included)
    std::memcpy(out.data() + 16, &new_bytes, sizeof new_bytes);
    std::memcpy(out.data() + kHeader, host.data(), new_bytes);
    TRACE(dispatch, "NtSetInformationFile class %lu: target '%ls' -> '%ls'", static_cast<unsigned long>(klass),
          target.c_str(), host.c_str());
    return Host().NtSetInformationFile(file, io, out.data(), static_cast<ULONG>(out.size()), klass);
}

// ---- Reparse points ---------------------------------------------------------
// A junction or symbolic link stores its target as an NT name, and the host's NTFS resolves that name itself: a
// guest's "\??\C:\Users\User\x" would be looked up on the host's own C: drive. FSCTL_SET_REPARSE_POINT therefore
// gets the host's name for a drive-letter target, and FSCTL_GET_REPARSE_POINT the guest's name back. Everything else
// (relative targets, volume GUIDs, other tags, names outside every guest drive) reaches the host exactly as written.
constexpr ULONG kFsctlSetReparsePoint = 0x000900A4;
constexpr ULONG kFsctlGetReparsePoint = 0x000900A8;
constexpr ULONG kReparseTagMountPoint = 0xA0000003;
constexpr ULONG kReparseTagSymlink = 0xA000000C;
constexpr ULONG kSymlinkFlagRelative = 1;
constexpr std::size_t kMaxReparseBuffer = 16 * 1024;   // MAXIMUM_REPARSE_DATA_BUFFER_SIZE

bool RewriteReparseTarget(const std::uint8_t* in, std::size_t len,
                          const std::function<bool(std::wstring_view, std::wstring*)>& map,
                          std::vector<std::uint8_t>* out) {
    // REPARSE_DATA_BUFFER: { ULONG Tag; USHORT DataLength; USHORT Reserved; then, for both tags, USHORT SubstituteNameOffset,
    // SubstituteNameLength, PrintNameOffset, PrintNameLength; a symbolic link adds ULONG Flags; WCHAR PathBuffer[] }.
    if (!in || len < 8) return false;
    ULONG tag = 0;
    USHORT data_len = 0;
    std::memcpy(&tag, in, 4);
    std::memcpy(&data_len, in + 4, 2);
    const bool link = tag == kReparseTagSymlink;
    if (tag != kReparseTagMountPoint && !link) return false;
    const std::size_t hdr = link ? 20 : 16;   // start of PathBuffer
    const std::size_t end = 8 + static_cast<std::size_t>(data_len);
    if (end > len || end < hdr) return false;
    USHORT so = 0, sl = 0, po = 0, pl = 0;
    std::memcpy(&so, in + 8, 2);
    std::memcpy(&sl, in + 10, 2);
    std::memcpy(&po, in + 12, 2);
    std::memcpy(&pl, in + 14, 2);
    ULONG flags = 0;
    if (link) std::memcpy(&flags, in + 16, 4);
    if (flags & kSymlinkFlagRelative) return false;   // a relative target has no drive to map
    if (((so | sl | po | pl) & 1) || hdr + so + sl > end || hdr + po + pl > end) return false;
    std::wstring substitute(sl / sizeof(wchar_t), L'\0');
    std::memcpy(substitute.data(), in + hdr + so, sl);
    std::wstring mapped;
    if (!map(substitute, &mapped)) return false;
    const std::size_t new_sl = mapped.size() * sizeof(wchar_t);
    // The layout the host demands: substitute name at offset 0, the print name right behind it, both NUL-terminated.
    const std::size_t total = hdr + new_sl + sizeof(wchar_t) + pl + sizeof(wchar_t);
    if (total - 8 > 0xFFFF || total > kMaxReparseBuffer) return false;
    out->assign(total, 0);
    std::uint8_t* o = out->data();
    const USHORT new_len = static_cast<USHORT>(total - 8), new_so = 0, new_sl16 = static_cast<USHORT>(new_sl),
                 new_po = static_cast<USHORT>(new_sl + sizeof(wchar_t));
    std::memcpy(o, &tag, 4);
    std::memcpy(o + 4, &new_len, 2);
    std::memcpy(o + 6, in + 6, 2);   // Reserved
    std::memcpy(o + 8, &new_so, 2);
    std::memcpy(o + 10, &new_sl16, 2);
    std::memcpy(o + 12, &new_po, 2);
    std::memcpy(o + 14, &pl, 2);
    if (link) std::memcpy(o + 16, &flags, 4);
    std::memcpy(o + hdr, mapped.data(), new_sl);
    std::memcpy(o + hdr + new_po, in + hdr + po, pl);
    return true;
}

namespace {

// "\??\C:\dir\" -> the host NT name of the same place. The drive root goes through the VFS and the rest is appended
// as written: unlike Win32 paths, NT names are not normalised, and a target such as "...\d\..\d" has to stay invalid
// for the host's NTFS to answer it the way it answers on Windows.
bool GuestReparseNameToHost(std::wstring_view in, std::wstring* out) {
    if (in.size() < 7 || !IStartsWith(in, L"\\??\\") || !std::iswalpha(in[4]) || in[5] != L':' || in[6] != L'\\')
        return false;
    std::wstring root;
    if (TranslateGuestNtPath(in.substr(0, 7), &root) != st::Success || root.empty()) return false;
    if (root.back() != L'\\') root += L'\\';
    root.append(in.substr(7));
    *out = std::move(root);
    return true;
}

bool HostReparseNameToGuest(std::wstring_view in, std::wstring* out) {
    if (!IStartsWith(in, L"\\??\\")) return false;
    std::wstring_view drive_path = in.substr(4);
    if (drive_path.size() < 3 || drive_path[1] != L':') return false;   // \??\Volume{..}\, \??\UNC\..: the host's own
    std::wstring guest;
    if (!UntranslateHostWin32Path(drive_path, &guest) || guest.size() < 3) return false;
    *out = L"\\??\\" + guest;
    return true;
}

using PFN_NtFsControlFile = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID,
                                             ULONG);
}  // namespace

static NTSTATUS NTAPI Xl_NtFsControlFile(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                                         ULONG code, PVOID in, ULONG in_length, PVOID out, ULONG out_length) {
    auto host = reinterpret_cast<PFN_NtFsControlFile>(HostExport("NtFsControlFile"));
    if (!host) return st::NotImplemented;
    if (code == kFsctlSetReparsePoint && in && in_length >= 8 && in_length <= kMaxReparseBuffer) {
        // The caller's buffer is copied once; a buffer that cannot be read is an access violation.
        std::vector<std::uint8_t> request(in_length), host_buffer;
        if (NTSTATUS cs = probe::CopyFromUser(request.data(), in, in_length)) return cs;
        if (RewriteReparseTarget(request.data(), request.size(), GuestReparseNameToHost, &host_buffer))
            return host(file, event, apc, apc_context, io, code, host_buffer.data(),
                        static_cast<ULONG>(host_buffer.size()), out, out_length);
        return host(file, event, apc, apc_context, io, code, in, in_length, out, out_length);
    }
    NTSTATUS s = host(file, event, apc, apc_context, io, code, in, in_length, out, out_length);
    if (code != kFsctlGetReparsePoint || !out || (s != st::Success && s != st::BufferOverflow)) return s;
    // The host answered with its own name. Ask once more, for the whole buffer, to see whether the target maps back
    // to a guest path (and how long that answer is); an answer that stays as it is is the one already delivered.
    std::vector<std::uint8_t> whole(kMaxReparseBuffer), guest_buffer;
    IO_STATUS_BLOCK host_io{};
    if (host(file, nullptr, nullptr, nullptr, &host_io, code, nullptr, 0, whole.data(), static_cast<ULONG>(whole.size())) !=
            st::Success ||
        host_io.Information > whole.size() ||
        !RewriteReparseTarget(whole.data(), static_cast<std::size_t>(host_io.Information), HostReparseNameToGuest,
                              &guest_buffer))
        return s;
    const ULONG copy = static_cast<ULONG>(guest_buffer.size() < out_length ? guest_buffer.size() : out_length);
    if (NTSTATUS ws = probe::CopyToUser(out, guest_buffer.data(), copy)) return ws;
    s = copy < guest_buffer.size() ? st::BufferOverflow : st::Success;
    IO_STATUS_BLOCK done{};
    done.Status = s;
    done.Information = copy;
    if (NTSTATUS ws = probe::Store<IO_STATUS_BLOCK>(io, done)) return ws;
    return s;
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
        if (io) {
            IO_STATUS_BLOCK done{};
            done.Status = s;
            if (NTSTATUS ws = probe::Store<IO_STATUS_BLOCK>(io, done)) return ws;
        }
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
        IO_STATUS_BLOCK done{};
        done.Status = s;
        if (NTSTATUS ws = probe::Store<IO_STATUS_BLOCK>(io, done)) return ws;
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
        {"NtLockFile", reinterpret_cast<void*>(&Xl_NtLockFile)},
        {"NtUnlockFile", reinterpret_cast<void*>(&Xl_NtUnlockFile)},
        {"NtFsControlFile", reinterpret_cast<void*>(&Xl_NtFsControlFile)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
