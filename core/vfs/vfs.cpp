// VFS implementation. See core/include/win10xbox/vfs.h for the contract.
//
// State lives in one immutable snapshot (State) swapped under an SRW lock by
// Initialize(); every query takes a shared_ptr copy and then works lock-free.
#define WIN32_NO_STATUS
#include "win10xbox/vfs.h"
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "json.h"
#include "path.h"
#include "win10xbox/trace.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <memory>

namespace win10xbox::vfs {

using detail::EqualNoCase;
using detail::Fold;
using detail::IsSep;
using detail::NormalizeComponents;
using detail::ParseHostPath;
using detail::StartsWithComponentNoCase;
using detail::StartsWithNoCase;
using detail::TokPath;

namespace {

constexpr size_t kExtendedPathThreshold = 248;   // see vfs.h: switch to \\?\ at this length
constexpr size_t kMaxUnicodeStringChars = 32767;

struct Drive {
    wchar_t letter;
    std::wstring host_root;   // "D:\machine" / "D:" / "\\srv\share\m"
    TokPath tok;
};

struct State {
    std::wstring machine_root;
    std::vector<Drive> drives;               // sorted by letter, always contains C
    std::wstring c_root;                     // host_root of C:
    TokPath builtin_tok;                     // C root + Windows\builtin
    MachineInfo info;
    DllOverride default_override = DllOverride::NativeThenBuiltin;
    std::vector<std::pair<std::wstring, DllOverride>> overrides;   // key: Fold(name without .dll)
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::shared_ptr<const State> g_state;

std::shared_ptr<const State> Snap() {
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<const State> s = g_state;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

// ---------------------------------------------------------------- strings

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool IsAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
wchar_t UpperLetter(wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - 0x20) : c; }

// ---------------------------------------------------------------- host side

// Adds the \\?\ prefix to a plain host Win32 path (UNC -> \\?\UNC\).
std::wstring ExtendWin32(const std::wstring& p) {
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'?' || p[2] == L'.') && p[3] == L'\\')
        return p;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring MaybeExtend(const std::wstring& p) {
    return p.size() >= kExtendedPathThreshold ? ExtendWin32(p) : p;
}

std::wstring ToNtHost(const std::wstring& win32) {
    if (win32.size() >= 2 && win32[0] == L'\\' && win32[1] == L'\\') return L"\\??\\UNC\\" + win32.substr(2);
    return L"\\??\\" + win32;
}

bool FileExists(const std::wstring& win32) {
    DWORD a = GetFileAttributesW(MaybeExtend(win32).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring BuildHost(const std::wstring& root, const std::vector<std::wstring>& comps, bool trailing) {
    std::wstring out = root;
    for (const auto& c : comps) { out += L'\\'; out += c; }
    if (comps.empty() || trailing) out += L'\\';
    return out;
}

// ---------------------------------------------------------------- DLL overrides

bool IsOverrideExtension(std::wstring_view name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos) return false;
    std::wstring_view e = name.substr(dot + 1);
    for (const wchar_t* x : {L"dll", L"drv", L"ocx", L"acm", L"ax", L"cpl", L"exe"})
        if (EqualNoCase(e, x)) return true;
    return false;
}

std::wstring OverrideKey(std::wstring_view name) {
    if (name.size() >= 4 && EqualNoCase(name.substr(name.size() - 4), L".dll")) name.remove_suffix(4);
    return Fold(name);
}

// E1: Wine's ntdll/kernelbase/kernel32 always. And the GUI stack (E1 extended, docs/GUI.md): core/user
// implements Wine's win32u contract (Wine's syscall numbering, ntuser.h's call codes), and native
// user32/gdi32/win32u/imm32 are clients of a real win32k that an AppContainer guest does not have.
bool IsAlwaysBuiltin(const std::wstring& key) {
    return key == L"NTDLL" || key == L"KERNELBASE" || key == L"KERNEL32" || key == L"WIN32U" || key == L"USER32" ||
           key == L"GDI32" || key == L"IMM32";
}

DllOverride OverrideImpl(const State& st, std::wstring_view name) {
    std::wstring key = OverrideKey(name);
    if (IsAlwaysBuiltin(key)) return DllOverride::Builtin;
    for (const auto& o : st.overrides)
        if (o.first == key) return o.second;
    return st.default_override;
}

bool ValidModuleName(std::wstring_view n) {
    if (n.empty() || n == L"." || n == L"..") return false;
    for (wchar_t c : n)
        if (c == L'\\' || c == L'/' || c == L':' || c == L'\0') return false;
    return true;
}

std::wstring NativePathOf(const State& st, std::wstring_view fname) {
    return st.c_root + L"\\Windows\\System32\\" + std::wstring(fname);
}
std::wstring BuiltinPathOf(const State& st, std::wstring_view fname) {
    return st.c_root + L"\\Windows\\builtin\\" + std::wstring(fname);
}

// Picks the host Win32 path for <System32>\<fname> according to the override.
NTSTATUS ResolveModuleImpl(const State& st, std::wstring_view fname, std::wstring* out) {
    std::wstring native = NativePathOf(st, fname);
    std::wstring builtin = BuiltinPathOf(st, fname);
    switch (OverrideImpl(st, fname)) {
        case DllOverride::Native: *out = native; return STATUS_SUCCESS;
        case DllOverride::Builtin: *out = builtin; return STATUS_SUCCESS;
        case DllOverride::NativeThenBuiltin:
            *out = (FileExists(native) || !FileExists(builtin)) ? native : builtin;
            return STATUS_SUCCESS;
        case DllOverride::BuiltinThenNative:
            *out = (FileExists(builtin) || !FileExists(native)) ? builtin : native;
            return STATUS_SUCCESS;
        case DllOverride::Disabled: return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return STATUS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------- guest path parsing

struct GuestPath {
    wchar_t drive = 0;
    std::vector<std::wstring> comps;
    bool trailing = false;
    bool ext_input = false;   // Win32 input used the \\?\ form
};

void SetComps(GuestPath* g, std::wstring_view s) {
    std::vector<std::wstring_view> v;
    NormalizeComponents(s, &v, &g->trailing);
    g->comps.assign(v.begin(), v.end());
}

// `s` starts with "X:" followed by end or a separator.
bool IsDriveSpec(std::wstring_view s) {
    return s.size() >= 2 && s[1] == L':' && IsAlpha(s[0]) && (s.size() == 2 || IsSep(s[2]));
}

NTSTATUS ParseDriveRest(std::wstring_view s, GuestPath* g) {
    if (!IsDriveSpec(s)) return STATUS_NOT_SUPPORTED;
    g->drive = UpperLetter(s[0]);
    SetComps(g, s.substr(2));
    return STATUS_SUCCESS;
}

// Maps \Device\HarddiskVolumeN (N is 1-based) to a drive letter.
NTSTATUS VolumeToDrive(const State& st, size_t n, wchar_t* letter) {
    if (n == 0) return STATUS_OBJECT_PATH_NOT_FOUND;
    if (n == 1) { *letter = L'C'; return STATUS_SUCCESS; }
    size_t idx = 1;
    for (const auto& d : st.drives) {
        if (d.letter == L'C') continue;
        if (++idx == n) { *letter = d.letter; return STATUS_SUCCESS; }
    }
    return STATUS_OBJECT_PATH_NOT_FOUND;
}

NTSTATUS ClassifyNt(const State& st, std::wstring_view p, GuestPath* g) {
    if (p.find(L'\0') != std::wstring_view::npos) return STATUS_OBJECT_NAME_INVALID;
    static const wchar_t* const kDrivePrefixes[] = {L"\\??\\", L"\\DosDevices\\", L"\\GLOBAL??\\", L"\\\\?\\"};
    for (const wchar_t* pre : kDrivePrefixes) {
        size_t n = wcslen(pre);
        if (StartsWithNoCase(p, pre)) {
            return ParseDriveRest(p.substr(n), g);   // PIPE\, UNC\, MAILSLOT\, NUL, ... -> NOT_SUPPORTED
        }
    }
    if (StartsWithComponentNoCase(p, L"\\SystemRoot")) {
        g->drive = L'C';
        std::wstring tmp = L"Windows";
        if (p.size() > 11) { tmp += L'\\'; tmp.append(p.substr(11)); }
        SetComps(g, tmp);
        return STATUS_SUCCESS;
    }
    static const wchar_t kVol[] = L"\\Device\\HarddiskVolume";
    if (StartsWithNoCase(p, kVol)) {
        std::wstring_view rest = p.substr(wcslen(kVol));
        size_t i = 0;
        size_t n = 0;
        while (i < rest.size() && rest[i] >= L'0' && rest[i] <= L'9') {
            n = n * 10 + static_cast<size_t>(rest[i] - L'0');
            if (n > 100000) n = 100000;
            ++i;
        }
        if (i == 0 || (i < rest.size() && !IsSep(rest[i]))) return STATUS_NOT_SUPPORTED;   // HarddiskVolumeShadowCopy1 ...
        NTSTATUS s = VolumeToDrive(st, n, &g->drive);
        if (s != STATUS_SUCCESS) return s;
        SetComps(g, rest.substr(i));
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS ClassifyWin32(std::wstring_view p, GuestPath* g) {
    if (p.find(L'\0') != std::wstring_view::npos) return STATUS_OBJECT_NAME_INVALID;
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'?' || p[2] == L'.') && p[3] == L'\\') {
        g->ext_input = p[2] == L'?';
        return ParseDriveRest(p.substr(4), g);
    }
    if (IsDriveSpec(p) && p.size() > 2) return ParseDriveRest(p, g);
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return STATUS_NOT_SUPPORTED;   // UNC
    return STATUS_OBJECT_NAME_INVALID;   // relative, drive-relative, rooted without drive
}

const Drive* FindDrive(const State& st, wchar_t letter) {
    for (const auto& d : st.drives)
        if (d.letter == letter) return &d;
    return nullptr;
}

// Guest drive path -> plain host Win32 path (applies DLL overrides).
NTSTATUS BuildHostPlain(const State& st, const GuestPath& g, std::wstring* out) {
    const Drive* d = FindDrive(st, g.drive);
    if (!d) return STATUS_OBJECT_PATH_NOT_FOUND;
    if (g.drive == L'C' && g.comps.size() == 3 && !g.trailing && EqualNoCase(g.comps[0], L"Windows") &&
        EqualNoCase(g.comps[1], L"System32") && IsOverrideExtension(g.comps[2]))
        return ResolveModuleImpl(st, g.comps[2], out);
    *out = BuildHost(d->host_root, g.comps, g.trailing);
    return STATUS_SUCCESS;
}

std::wstring EmitGuest(wchar_t drive, const std::vector<std::wstring_view>& comps, bool trailing) {
    std::wstring r;
    r += drive;
    r += L':';
    for (auto c : comps) { r += L'\\'; r.append(c); }
    if (comps.empty() || trailing) r += L'\\';
    return r;
}

// ---------------------------------------------------------------- machine.json

bool ReadWholeFile(const std::wstring& path, std::string* out, DWORD* err) {
    HANDLE h = CreateFileW(MaybeExtend(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart <= (4 << 20);
    if (!ok) *err = ERROR_FILE_TOO_LARGE;
    if (ok) {
        out->resize(static_cast<size_t>(sz.QuadPart));
        size_t got = 0;
        while (got < out->size()) {
            DWORD n = 0;
            if (!ReadFile(h, &(*out)[got], static_cast<DWORD>(out->size() - got), &n, nullptr) || n == 0) break;
            got += n;
        }
        if (got != out->size()) { *err = GetLastError(); ok = false; }
    }
    CloseHandle(h);
    return ok;
}

bool ParseOverrideValue(const std::string& raw, DllOverride* out) {
    std::string s;
    for (char c : raw) {
        if (c == ' ' || c == '\t') continue;
        s.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    }
    if (s.empty() || s == "d" || s == "disabled") { *out = DllOverride::Disabled; return true; }
    if (s == "n" || s == "native") { *out = DllOverride::Native; return true; }
    if (s == "b" || s == "builtin") { *out = DllOverride::Builtin; return true; }
    if (s == "n,b" || s == "native,builtin") { *out = DllOverride::NativeThenBuiltin; return true; }
    if (s == "b,n" || s == "builtin,native") { *out = DllOverride::BuiltinThenNative; return true; }
    return false;
}

bool GetU32(const json::Value* obj, const char* key, std::uint32_t* out) {
    const json::Value* v = obj ? obj->Find(key) : nullptr;
    if (!v) return true;
    if (!v->IsNumber() || v->num < 0 || v->num > 4294967295.0) return false;
    *out = static_cast<std::uint32_t>(v->num);
    return true;
}
bool GetStr(const json::Value* obj, const char* key, std::wstring* out) {
    const json::Value* v = obj ? obj->Find(key) : nullptr;
    if (!v) return true;
    if (!v->IsString()) return false;
    *out = Widen(v->str);
    return true;
}
bool GetBool(const json::Value* obj, const char* key, bool* out) {
    const json::Value* v = obj ? obj->Find(key) : nullptr;
    if (!v) return true;
    if (!v->IsBool()) return false;
    *out = v->b;
    return true;
}

void ApplyConfig(const json::Value& root, State* st) {
    auto bad = [](const char* section, const char* key) {
        WARN(vfs, "machine.json: bad value for %s.%s, using default", section, key);
    };
    if (!root.IsObject()) { WARN(vfs, "machine.json: top level is not an object"); return; }

    if (const json::Value* w = root.Find("windows"); w && w->IsObject()) {
        auto& o = st->info.windows;
        if (!GetU32(w, "major", &o.major)) bad("windows", "major");
        if (!GetU32(w, "minor", &o.minor)) bad("windows", "minor");
        if (!GetU32(w, "build", &o.build)) bad("windows", "build");
        if (!GetU32(w, "ubr", &o.ubr)) bad("windows", "ubr");
        if (!GetStr(w, "product_name", &o.product_name)) bad("windows", "product_name");
        if (!GetStr(w, "display_version", &o.display_version)) bad("windows", "display_version");
        if (!GetStr(w, "csd", &o.csd)) bad("windows", "csd");
    }
    if (const json::Value* i = root.Find("identity"); i && i->IsObject()) {
        auto& o = st->info.identity;
        if (!GetStr(i, "computer_name", &o.computer_name)) bad("identity", "computer_name");
        if (!GetStr(i, "user_name", &o.user_name) || o.user_name.empty()) {
            bad("identity", "user_name");
            o.user_name = L"User";
        }
        if (!GetStr(i, "user_sid", &o.user_sid)) bad("identity", "user_sid");
        if (!GetStr(i, "domain", &o.domain)) bad("identity", "domain");
    }
    if (const json::Value* c = root.Find("clock"); c && c->IsObject()) {
        auto& o = st->info.clock;
        if (const json::Value* v = c->Find("scale"); v && v->IsNumber() && v->num > 0) o.scale = v->num;
        else if (v) bad("clock", "scale");
        if (const json::Value* v = c->Find("offset_ms"); v && v->IsNumber()) o.offset_ms = static_cast<std::int64_t>(v->num);
        else if (v) bad("clock", "offset_ms");
        if (!GetBool(c, "frozen", &o.frozen)) bad("clock", "frozen");
    }
    if (const json::Value* c = root.Find("cpu"); c && c->IsObject()) {
        auto& o = st->info.cpu;
        if (!GetBool(c, "hide_hypervisor", &o.hide_hypervisor)) bad("cpu", "hide_hypervisor");
        if (!GetStr(c, "vendor", &o.vendor)) bad("cpu", "vendor");
    }
    if (const json::Value* s = root.Find("syscall_overrides"); s && s->IsObject()) {
        for (const auto& m : s->members) {
            if (!m.second.IsString()) { WARN(vfs, "machine.json: syscall_overrides.%s is not a string", m.first.c_str()); continue; }
            st->info.syscall_overrides.emplace_back(Widen(m.first), Widen(m.second.str));
        }
    }
    if (const json::Value* d = root.Find("dll_overrides"); d && d->IsObject()) {
        for (const auto& m : d->members) {
            DllOverride ov;
            if (!m.second.IsString() || !ParseOverrideValue(m.second.str, &ov)) {
                WARN(vfs, "machine.json: dll_overrides.%s: unrecognised value, ignored", m.first.c_str());
                continue;
            }
            std::wstring key = OverrideKey(Widen(m.first));
            if (key == L"*") { st->default_override = ov; continue; }
            if (IsAlwaysBuiltin(key)) {
                WARN(vfs, "machine.json: dll_overrides.%s ignored: ntdll/kernelbase/kernel32 and the GUI stack (win32u/user32/gdi32/imm32) are always builtin (E1)",
                     m.first.c_str());
                continue;
            }
            bool replaced = false;
            for (auto& e : st->overrides)
                if (e.first == key) { e.second = ov; replaced = true; }
            if (!replaced) st->overrides.emplace_back(std::move(key), ov);
        }
    }
}

// Reads the "drives" object into st->drives (relative host paths are relative
// to the machine root). C: defaults to the machine root.
void ApplyDrives(const json::Value& root, State* st, const TokPath& machine_tok) {
    std::map<wchar_t, TokPath> map;
    map[L'C'] = machine_tok;
    const json::Value* d = root.IsObject() ? root.Find("drives") : nullptr;
    if (d && d->IsObject()) {
        for (const auto& m : d->members) {
            std::wstring key = Widen(m.first);
            if (key.size() >= 2 && key.back() == L'\\') key.pop_back();
            if (key.size() >= 2 && key.back() == L':') key.pop_back();
            if (key.size() != 1 || !IsAlpha(key[0]) || !m.second.IsString() || m.second.str.empty()) {
                WARN(vfs, "machine.json: drives.%s: bad drive letter or host path, ignored", m.first.c_str());
                continue;
            }
            std::wstring v = Widen(m.second.str);
            TokPath tok;
            if (!ParseHostPath(v, &tok)) {
                if (IsSep(v[0]) || (v.size() >= 2 && v[1] == L':')) {
                    WARN(vfs, "machine.json: drives.%s: path must be absolute or relative to the machine root", m.first.c_str());
                    continue;
                }
                if (!ParseHostPath(detail::JoinTokPath(machine_tok) + L"\\" + v, &tok)) continue;
            }
            map[UpperLetter(key[0])] = std::move(tok);
        }
    }
    for (auto& e : map) {
        Drive dr;
        dr.letter = e.first;
        dr.tok = std::move(e.second);
        dr.host_root = detail::JoinTokPath(dr.tok);
        st->drives.push_back(std::move(dr));
    }
}

// ---------------------------------------------------------------- reverse mapping

NTSTATUS UntranslateImpl(std::wstring_view in, bool want_nt, std::wstring* out) {
    auto st = Snap();
    auto unsupported = [&]() { *out = std::wstring(in); return STATUS_NOT_SUPPORTED; };
    if (!st) return unsupported();

    std::wstring_view p = in;
    bool ext = false;
    std::wstring unc_tmp;
    if (StartsWithNoCase(p, L"\\??\\")) {
        p.remove_prefix(4);
        if (p.size() >= 4 && StartsWithNoCase(p, L"UNC") && IsSep(p[3])) {
            unc_tmp = L"\\\\" + std::wstring(p.substr(4));
            p = unc_tmp;
        }
    } else if (StartsWithNoCase(p, L"\\\\?\\")) {
        ext = true;
    }
    TokPath hp;
    if (!ParseHostPath(p, &hp)) return unsupported();

    size_t best_len = 0;
    bool found = false;
    std::vector<std::wstring> guest_prefix;
    wchar_t guest_drive = 0;
    size_t skip = 0;

    auto consider = [&](const TokPath& root, wchar_t drive, const std::vector<std::wstring>& prefix) {
        if (!EqualNoCase(root.root, hp.root) || root.comps.size() > hp.comps.size()) return;
        for (size_t i = 0; i < root.comps.size(); ++i)
            if (!EqualNoCase(root.comps[i], hp.comps[i])) return;
        if (found && root.comps.size() <= best_len) return;
        found = true;
        best_len = root.comps.size();
        skip = root.comps.size();
        guest_drive = drive;
        guest_prefix = prefix;
    };
    consider(st->builtin_tok, L'C', {L"Windows", L"System32"});
    for (const auto& d : st->drives) consider(d.tok, d.letter, {});
    if (!found) return unsupported();

    std::vector<std::wstring_view> comps(guest_prefix.begin(), guest_prefix.end());
    for (size_t i = skip; i < hp.comps.size(); ++i) comps.push_back(hp.comps[i]);
    std::wstring g = EmitGuest(guest_drive, comps, hp.trailing);
    if (want_nt) *out = L"\\??\\" + g;
    else if (ext) *out = L"\\\\?\\" + g;
    else *out = std::move(g);
    return STATUS_SUCCESS;
}

}  // namespace

// ================================================================ public API

NTSTATUS Initialize(const std::wstring& machine_root) {
    if (machine_root.empty()) return STATUS_INVALID_PARAMETER;
    auto st = std::make_shared<State>();

    std::wstring r = machine_root;
    std::replace(r.begin(), r.end(), L'/', L'\\');
    TokPath mt;
    if (!ParseHostPath(r, &mt)) {
        wchar_t buf[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(r.c_str(), static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
        if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]) || !ParseHostPath(std::wstring_view(buf, n), &mt)) {
            ERR(vfs, "cannot resolve machine root '%ls'", machine_root.c_str());
            return STATUS_INVALID_PARAMETER;
        }
    }
    st->machine_root = detail::JoinTokPath(mt);

    NTSTATUS status = STATUS_SUCCESS;
    json::Value doc;
    std::string text;
    DWORD err = 0;
    std::wstring json_path = st->machine_root + L"\\machine.json";
    if (!ReadWholeFile(json_path, &text, &err)) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            TRACE(vfs, "no machine.json in '%ls': using defaults", st->machine_root.c_str());
        else
            WARN(vfs, "cannot read '%ls' (error %lu): using defaults", json_path.c_str(), err);
    } else {
        std::string perr;
        if (!json::Parse(text, &doc, &perr)) {
            ERR(vfs, "'%ls': %s", json_path.c_str(), perr.c_str());
            doc = json::Value();
            status = STATUS_INVALID_PARAMETER;
        }
    }

    ApplyConfig(doc, st.get());
    ApplyDrives(doc, st.get(), mt);
    const Drive* c = FindDrive(*st, L'C');
    st->c_root = c->host_root;
    st->builtin_tok = c->tok;
    st->builtin_tok.comps.push_back(L"Windows");
    st->builtin_tok.comps.push_back(L"builtin");

    for (const auto& d : st->drives)
        TRACE(vfs, "drive %c: -> %ls", static_cast<char>(d.letter), d.host_root.c_str());
    TRACE(vfs, "machine root '%ls', user '%ls'", st->machine_root.c_str(), st->info.identity.user_name.c_str());

    AcquireSRWLockExclusive(&g_lock);
    g_state = std::move(st);
    ReleaseSRWLockExclusive(&g_lock);
    return status;
}

std::wstring MachineRoot() {
    auto st = Snap();
    return st ? st->machine_root : std::wstring();
}

std::vector<DriveMapping> Drives() {
    std::vector<DriveMapping> r;
    if (auto st = Snap())
        for (const auto& d : st->drives) r.push_back({d.letter, d.host_root});
    return r;
}

MachineInfo GetMachineInfo() {
    auto st = Snap();
    return st ? st->info : MachineInfo();
}

DllOverride OverrideFor(std::wstring_view module_name) {
    auto st = Snap();
    if (!st) return IsAlwaysBuiltin(OverrideKey(module_name)) ? DllOverride::Builtin : DllOverride::NativeThenBuiltin;
    return OverrideImpl(*st, module_name);
}

namespace {
std::wstring DllPathIn(std::wstring_view module_name, bool builtin) {
    auto st = Snap();
    if (!st || !ValidModuleName(module_name)) return {};
    std::wstring name(module_name);
    if (name.find(L'.') == std::wstring::npos) name += L".dll";
    std::wstring p = builtin ? BuiltinPathOf(*st, name) : NativePathOf(*st, name);
    return FileExists(p) ? p : std::wstring();
}
}  // namespace

std::wstring BuiltinDllPath(std::wstring_view module_name) { return DllPathIn(module_name, true); }
std::wstring NativeDllPath(std::wstring_view module_name) { return DllPathIn(module_name, false); }

NTSTATUS ResolveSystemModule(std::wstring_view module_name, std::wstring* host_win32_path) {
    auto st = Snap();
    if (!st) return STATUS_UNSUCCESSFUL;
    if (!ValidModuleName(module_name)) return STATUS_OBJECT_NAME_INVALID;
    std::wstring name(module_name);
    if (name.find(L'.') == std::wstring::npos) name += L".dll";
    std::wstring p;
    NTSTATUS s = ResolveModuleImpl(*st, name, &p);
    if (s == STATUS_SUCCESS) *host_win32_path = std::move(p);
    return s;
}

NTSTATUS TranslateNtPath(std::wstring_view guest_nt_path, std::wstring* host_nt_path) {
    auto st = Snap();
    if (!st) { ERR(vfs, "TranslateNtPath before Initialize"); return STATUS_UNSUCCESSFUL; }
    GuestPath g;
    NTSTATUS s = ClassifyNt(*st, guest_nt_path, &g);
    if (s == STATUS_SUCCESS) {
        std::wstring plain;
        s = BuildHostPlain(*st, g, &plain);
        if (s == STATUS_SUCCESS) *host_nt_path = ToNtHost(plain);
    }
    TRACE(vfs, "nt '%ls' -> '%ls' (%08lx)", std::wstring(guest_nt_path).c_str(),
          s == STATUS_SUCCESS ? host_nt_path->c_str() : L"", static_cast<unsigned long>(s));
    return s;
}

NTSTATUS TranslateWin32Path(std::wstring_view guest_win32_path, std::wstring* host_win32_path) {
    auto st = Snap();
    if (!st) { ERR(vfs, "TranslateWin32Path before Initialize"); return STATUS_UNSUCCESSFUL; }
    GuestPath g;
    NTSTATUS s = ClassifyWin32(guest_win32_path, &g);
    if (s == STATUS_SUCCESS) {
        std::wstring plain;
        s = BuildHostPlain(*st, g, &plain);
        if (s == STATUS_SUCCESS) *host_win32_path = g.ext_input ? ExtendWin32(plain) : MaybeExtend(plain);
    }
    TRACE(vfs, "win32 '%ls' -> '%ls' (%08lx)", std::wstring(guest_win32_path).c_str(),
          s == STATUS_SUCCESS ? host_win32_path->c_str() : L"", static_cast<unsigned long>(s));
    return s;
}

NTSTATUS UntranslateNtPath(std::wstring_view host_nt_path, std::wstring* guest_nt_path) {
    return UntranslateImpl(host_nt_path, true, guest_nt_path);
}

NTSTATUS UntranslateWin32Path(std::wstring_view host_win32_path, std::wstring* guest_win32_path) {
    return UntranslateImpl(host_win32_path, false, guest_win32_path);
}

NTSTATUS TranslateObjectAttributes(const OBJECT_ATTRIBUTES* in, TranslatedAttributes* out) {
    out->attr = OBJECT_ATTRIBUTES{};
    out->name = UNICODE_STRING{};
    out->storage.clear();
    out->translated = false;
    if (!in) return STATUS_SUCCESS;
    if (in->Length < sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    out->attr = *in;
    if (!in->ObjectName || in->RootDirectory || !in->ObjectName->Buffer || in->ObjectName->Length == 0)
        return STATUS_SUCCESS;   // relative to a (host) handle, or nameless: nothing to rewrite

    std::wstring_view guest(in->ObjectName->Buffer, in->ObjectName->Length / sizeof(wchar_t));
    std::wstring host;
    NTSTATUS s = TranslateNtPath(guest, &host);
    if (s == STATUS_NOT_SUPPORTED) return STATUS_SUCCESS;   // namespace we do not own: pass through
    if (s != STATUS_SUCCESS) return s;
    if (host.size() > kMaxUnicodeStringChars - 1) return STATUS_NAME_TOO_LONG;
    out->storage = std::move(host);
    out->name.Buffer = out->storage.data();
    out->name.Length = static_cast<USHORT>(out->storage.size() * sizeof(wchar_t));
    out->name.MaximumLength = static_cast<USHORT>(out->name.Length + sizeof(wchar_t));
    out->attr.ObjectName = &out->name;
    out->translated = true;
    return STATUS_SUCCESS;
}

NTSTATUS GuestFullPath(std::wstring_view path, std::wstring_view cwd, std::wstring* absolute) {
    if (path.find(L'\0') != std::wstring_view::npos) return STATUS_OBJECT_NAME_INVALID;
    if (path.empty()) return STATUS_OBJECT_NAME_INVALID;
    // \\?\ , \\.\ and UNC paths are not resolved against anything.
    if (path.size() >= 2 && IsSep(path[0]) && IsSep(path[1])) {
        std::wstring r(path);
        std::replace(r.begin(), r.end(), L'/', L'\\');
        *absolute = std::move(r);
        return STATUS_SUCCESS;
    }
    // Parse cwd lazily; it is only needed for relative forms.
    wchar_t cwd_drive = 0;
    std::wstring cwd_rest;
    if (IsDriveSpec(cwd) && cwd.size() > 2) {
        cwd_drive = UpperLetter(cwd[0]);
        cwd_rest.assign(cwd.substr(2));
    }

    wchar_t drive = 0;
    std::wstring joined;   // components string to normalise
    if (IsDriveSpec(path) && path.size() > 2) {
        drive = UpperLetter(path[0]);
        joined.assign(path.substr(2));
    } else if (path.size() >= 2 && path[1] == L':' && IsAlpha(path[0])) {   // "C:foo" or "C:"
        drive = UpperLetter(path[0]);
        if (cwd_drive == drive) {
            joined = cwd_rest;
            if (path.size() > 2) joined += L'\\';
        }
        joined.append(path.substr(2));
    } else if (IsSep(path[0])) {
        if (!cwd_drive) return STATUS_OBJECT_NAME_INVALID;
        drive = cwd_drive;
        joined.assign(path);
    } else {
        if (!cwd_drive) return STATUS_OBJECT_NAME_INVALID;
        drive = cwd_drive;
        joined = cwd_rest + L"\\" + std::wstring(path);
    }
    std::vector<std::wstring_view> comps;
    bool trailing = false;
    NormalizeComponents(joined, &comps, &trailing);
    *absolute = EmitGuest(drive, comps, trailing);
    return STATUS_SUCCESS;
}

std::wstring GuestSystemRoot() { return L"C:\\Windows"; }
std::wstring GuestSystemDirectory() { return L"C:\\Windows\\System32"; }

std::wstring GuestUserProfile() {
    auto st = Snap();
    return L"C:\\Users\\" + (st ? st->info.identity.user_name : MachineInfo().identity.user_name);
}

std::wstring GuestTempDirectory() { return GuestUserProfile() + L"\\AppData\\Local\\Temp"; }

}  // namespace win10xbox::vfs
