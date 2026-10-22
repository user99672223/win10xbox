// VFS unit tests: path translation, DLL overrides, pass-through namespaces,
// untranslate round trips, OBJECT_ATTRIBUTES rewriting, machine.json parsing.
// Fixtures: tests/fixtures/vfs/{machine,machine_alt,bad_json}.
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "check.h"
#include "win10xbox/vfs.h"
#include "../../core/vfs/json.h"
#include "../../core/vfs/path.h"

#include <cstring>
#include <string>

using namespace win10xbox::vfs;

#define CHECK_W(a, b) do { std::wstring _a = (a), _b = (b); if (_a != _b) { ++::w10x_test::Failures(); \
    std::printf("  FAIL %s:%d: %s == %s\n    got:  %ls\n    want: %ls\n", __FILE__, __LINE__, #a, #b, \
                _a.c_str(), _b.c_str()); } } while (0)

namespace {

std::wstring g_root;   // host root of the "machine" fixture, as normalised by Initialize

std::wstring FixtureDir(const char* sub) {
    std::string base = WIN10XBOX_FIXTURES_DIR;
    std::wstring w(base.begin(), base.end());
    if (!w.empty() && w[0] == L'/') w = L"Z:" + w;   // Wine: unix root is drive Z:
    for (auto& c : w) if (c == L'/') c = L'\\';
    std::string s = sub;
    return w + L"\\vfs\\" + std::wstring(s.begin(), s.end());
}

void UseMachine() {
    NTSTATUS s = Initialize(FixtureDir("machine"));
    CHECK_EQ(s, STATUS_SUCCESS);
    g_root = MachineRoot();
}

std::wstring HostWin32(const std::wstring& rest) { return g_root + rest; }
std::wstring HostNt(const std::wstring& rest) { return L"\\??\\" + g_root + rest; }

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           win10xbox::vfs::detail::EqualNoCase(std::wstring_view(s).substr(s.size() - suffix.size()), suffix);
}

void ExpectNt(int line, std::wstring_view guest, NTSTATUS want, const std::wstring& want_host) {
    std::wstring out = L"<untouched>";
    NTSTATUS s = TranslateNtPath(guest, &out);
    if (s != want) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: TranslateNtPath(%ls) = %08lx, want %08lx\n", line,
                    std::wstring(guest).c_str(), static_cast<unsigned long>(s), static_cast<unsigned long>(want));
    } else if (want == STATUS_SUCCESS && out != want_host) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: TranslateNtPath(%ls)\n    got:  %ls\n    want: %ls\n", line,
                    std::wstring(guest).c_str(), out.c_str(), want_host.c_str());
    }
}

void ExpectWin32(int line, std::wstring_view guest, NTSTATUS want, const std::wstring& want_host) {
    std::wstring out = L"<untouched>";
    NTSTATUS s = TranslateWin32Path(guest, &out);
    if (s != want) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: TranslateWin32Path(%ls) = %08lx, want %08lx\n", line,
                    std::wstring(guest).c_str(), static_cast<unsigned long>(s), static_cast<unsigned long>(want));
    } else if (want == STATUS_SUCCESS && out != want_host) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: TranslateWin32Path(%ls)\n    got:  %ls\n    want: %ls\n", line,
                    std::wstring(guest).c_str(), out.c_str(), want_host.c_str());
    }
}

void ExpectUntranslate(int line, bool nt, std::wstring_view host, NTSTATUS want, const std::wstring& want_guest) {
    std::wstring out;
    NTSTATUS s = nt ? UntranslateNtPath(host, &out) : UntranslateWin32Path(host, &out);
    if (s != want) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: Untranslate(%ls) = %08lx, want %08lx\n", line, std::wstring(host).c_str(),
                    static_cast<unsigned long>(s), static_cast<unsigned long>(want));
    } else if (out != want_guest) {
        ++w10x_test::Failures();
        std::printf("  FAIL line %d: Untranslate(%ls)\n    got:  %ls\n    want: %ls\n", line,
                    std::wstring(host).c_str(), out.c_str(), want_guest.c_str());
    }
}

}  // namespace

#define NT_OK(g, host)     ExpectNt(__LINE__, g, STATUS_SUCCESS, host)
#define NT_ST(g, status)   ExpectNt(__LINE__, g, status, L"")
#define W32_OK(g, host)    ExpectWin32(__LINE__, g, STATUS_SUCCESS, host)
#define W32_ST(g, status)  ExpectWin32(__LINE__, g, status, L"")
#define UN_NT(h, guest)    ExpectUntranslate(__LINE__, true, h, STATUS_SUCCESS, guest)
#define UN_W32(h, guest)   ExpectUntranslate(__LINE__, false, h, STATUS_SUCCESS, guest)
#define UN_NO_NT(h)        ExpectUntranslate(__LINE__, true, h, STATUS_NOT_SUPPORTED, h)
#define UN_NO_W32(h)       ExpectUntranslate(__LINE__, false, h, STATUS_NOT_SUPPORTED, h)

// ------------------------------------------------------------------ json

TEST(vfs_json_parses_values) {
    using namespace win10xbox::vfs::json;
    Value v;
    std::string err;
    const char* text = "\xEF\xBB\xBF{ \"a\": 1, \"b\": [true, false, null, \"x\\u00e9\\ud83d\\ude00\\n\"], "
                       "\"c\": {\"d\": -1.5e2}, \"e\": \"\" }";
    CHECK(Parse(text, &v, &err));
    CHECK(v.IsObject());
    CHECK(v.Find("a") && v.Find("a")->IsNumber() && v.Find("a")->num == 1.0);
    const Value* b = v.Find("b");
    CHECK(b && b->items.size() == 4);
    if (b && b->items.size() == 4) {
        CHECK(b->items[0].IsBool() && b->items[0].b);
        CHECK(b->items[1].IsBool() && !b->items[1].b);
        CHECK(b->items[2].type == Value::Type::Null);
        CHECK(b->items[3].str == std::string("x\xC3\xA9\xF0\x9F\x98\x80\n"));
    }
    CHECK(v.Find("c") && v.Find("c")->Find("d") && v.Find("c")->Find("d")->num == -150.0);
    CHECK(v.Find("e") && v.Find("e")->IsString() && v.Find("e")->str.empty());
    CHECK(v.Find("zzz") == nullptr);
    CHECK(v.members.size() == 4 && v.members[0].first == "a");   // source order kept
}

TEST(vfs_json_rejects_malformed) {
    using namespace win10xbox::vfs::json;
    const char* bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} x", "\"abc", "\"\\q\"", "01", "{'a':1}",
                         "\"\\ud800\"", "\"a\tb\"", "[1 2]", "{\"a\" 1}", "tru", "-", "1.", "1e"};
    for (const char* t : bad) {
        Value v;
        std::string err;
        CHECK_MSG(!Parse(t, &v, &err), "accepted: %s", t);
        CHECK_MSG(!err.empty(), "no message for: %s", t);
    }
    std::string deep(200, '[');
    Value v;
    std::string err;
    CHECK(!Parse(deep, &v, &err));
}

// ------------------------------------------------------------------ helpers

TEST(vfs_case_folding) {
    using win10xbox::vfs::detail::EqualNoCase;
    using win10xbox::vfs::detail::FoldChar;
    CHECK(EqualNoCase(L"Windows", L"WINDOWS"));
    CHECK(EqualNoCase(L"\u00e9t\u00e9", L"\u00c9T\u00c9"));        // Latin-1
    CHECK(EqualNoCase(L"\u0101", L"\u0100"));                      // Latin Extended-A (odd lower)
    CHECK(EqualNoCase(L"\u013a", L"\u0139"));                      // (even lower)
    CHECK(EqualNoCase(L"\u03b1\u03c2", L"\u0391\u03a3"));          // Greek incl. final sigma
    CHECK(EqualNoCase(L"\u0434\u0451", L"\u0414\u0401"));          // Cyrillic
    CHECK(!EqualNoCase(L"abc", L"abd"));
    CHECK(!EqualNoCase(L"abc", L"abcd"));
    CHECK_EQ(FoldChar(0xF7), wchar_t(0xF7));   // division sign is not a letter
    CHECK_EQ(FoldChar(0xDF), wchar_t(0xDF));   // sharp s has no single-char upper
    CHECK_EQ(FoldChar(L'['), L'[');
}

TEST(vfs_normalize_components) {
    using win10xbox::vfs::detail::NormalizeComponents;
    std::vector<std::wstring_view> c;
    bool t = false;
    NormalizeComponents(L"a\\b/c", &c, &t);
    CHECK(c.size() == 3 && !t);
    NormalizeComponents(L"..\\..\\a", &c, &t);
    CHECK(c.size() == 1 && c[0] == L"a");
    NormalizeComponents(L"a\\.\\b\\", &c, &t);
    CHECK(c.size() == 2 && t);
    NormalizeComponents(L"a\\..", &c, &t);
    CHECK(c.empty() && !t);
    NormalizeComponents(L"\\\\a\\\\b", &c, &t);
    CHECK(c.size() == 2 && c[0] == L"a" && c[1] == L"b");
    NormalizeComponents(L"a\\...\\b", &c, &t);
    CHECK(c.size() == 3 && c[1] == L"...");
    NormalizeComponents(L"", &c, &t);
    CHECK(c.empty() && !t);
}

// ------------------------------------------------------------------ initialize

TEST(vfs_initialize_reads_machine_json) {
    UseMachine();
    CHECK(EndsWithNoCase(g_root, L"\\vfs\\machine"));
    CHECK(g_root.size() > 3 && g_root.back() != L'\\');
    auto drives = Drives();
    CHECK(drives.size() == 2);
    if (drives.size() == 2) {
        CHECK_EQ(drives[0].letter, L'C');
        CHECK_W(drives[0].host_root, g_root);
        CHECK_EQ(drives[1].letter, L'D');
        CHECK_W(drives[1].host_root, g_root + L"\\extra_drive");
    }
    MachineInfo mi = GetMachineInfo();
    CHECK_W(mi.identity.computer_name, L"TESTPC");
    CHECK_W(mi.identity.user_name, L"User");
    CHECK_W(mi.identity.user_sid, L"S-1-5-21-1-2-3-1001");
    CHECK_EQ(mi.windows.build, 19045u);
    CHECK_EQ(mi.windows.ubr, 3803u);
    CHECK_W(mi.windows.display_version, L"22H2");
    CHECK(mi.cpu.hide_hypervisor);
    CHECK(mi.clock.scale == 1.0 && !mi.clock.frozen);
    // idempotent
    UseMachine();
    CHECK(Drives().size() == 2);
}

TEST(vfs_initialize_defaults_without_machine_json) {
    std::wstring dir = FixtureDir("does_not_exist");
    CHECK_EQ(Initialize(dir), STATUS_SUCCESS);
    auto drives = Drives();
    CHECK(drives.size() == 1);
    CHECK(MachineRoot() == dir);
    if (!drives.empty()) { CHECK_EQ(drives[0].letter, L'C'); CHECK_W(drives[0].host_root, dir); }
    CHECK(OverrideFor(L"anything") == DllOverride::NativeThenBuiltin);
    CHECK(OverrideFor(L"ntdll") == DllOverride::Builtin);
    CHECK_W(GuestUserProfile(), L"C:\\Users\\User");
    MachineInfo mi = GetMachineInfo();
    CHECK_EQ(mi.windows.build, 19045u);
    CHECK_W(mi.cpu.vendor, L"AuthenticAMD");
    std::wstring out;
    CHECK_EQ(TranslateNtPath(L"\\??\\C:\\x", &out), STATUS_SUCCESS);
    CHECK_W(out, L"\\??\\" + dir + L"\\x");
    CHECK_EQ(TranslateNtPath(L"\\??\\D:\\x", &out), STATUS_OBJECT_PATH_NOT_FOUND);
    UseMachine();
}

TEST(vfs_initialize_errors_and_root_forms) {
    UseMachine();
    std::wstring before = MachineRoot();
    CHECK_EQ(Initialize(L""), STATUS_INVALID_PARAMETER);
    CHECK_W(MachineRoot(), before);   // unchanged

    // Malformed machine.json: defaults installed, error reported.
    CHECK_EQ(Initialize(FixtureDir("bad_json")), STATUS_INVALID_PARAMETER);
    std::wstring out;
    CHECK_EQ(TranslateNtPath(L"\\??\\C:\\x", &out), STATUS_SUCCESS);
    CHECK(Drives().size() == 1);

    // Equivalent spellings of the same root normalise identically.
    std::wstring dir = FixtureDir("machine");
    std::wstring fwd = dir;
    for (auto& c : fwd) if (c == L'\\') c = L'/';
    CHECK_EQ(Initialize(fwd + L"/"), STATUS_SUCCESS);
    CHECK_W(MachineRoot(), before);
    CHECK_EQ(Initialize(L"\\\\?\\" + dir + L"\\Windows\\.."), STATUS_SUCCESS);
    CHECK_W(MachineRoot(), before);

    // A relative root becomes absolute.
    CHECK_EQ(Initialize(L"."), STATUS_SUCCESS);
    std::wstring r = MachineRoot();
    CHECK(r.size() > 3 && r[1] == L':' && r.back() != L'\\');
    UseMachine();
}

TEST(vfs_initialize_alt_machine) {
    CHECK_EQ(Initialize(FixtureDir("machine_alt")), STATUS_SUCCESS);
    std::wstring root = MachineRoot();
    MachineInfo mi = GetMachineInfo();
    CHECK_EQ(mi.windows.build, 20348u);
    CHECK_EQ(mi.windows.ubr, 1u);
    CHECK_W(mi.windows.product_name, L"Windows Server 2022");
    CHECK_W(mi.windows.csd, L"SP \u00e9");
    CHECK_W(mi.identity.user_name, L"Alt");
    CHECK_W(mi.identity.domain, L"WORKGROUP");
    CHECK(mi.clock.scale == 2.5 && mi.clock.offset_ms == -1500 && mi.clock.frozen);
    CHECK(!mi.cpu.hide_hypervisor);
    CHECK_W(mi.cpu.vendor, L"GenuineIntel");
    CHECK(mi.syscall_overrides.size() == 1);
    if (mi.syscall_overrides.size() == 1) {
        CHECK_W(mi.syscall_overrides[0].first, L"NtQuerySystemInformation");
        CHECK_W(mi.syscall_overrides[0].second, L"emulate");
    }
    CHECK_W(GuestUserProfile(), L"C:\\Users\\Alt");
    CHECK_W(GuestTempDirectory(), L"C:\\Users\\Alt\\AppData\\Local\\Temp");

    // Drives: C remapped, "e" normalised, an "F:" key spelled with a trailing backslash accepted and
    // "../machine" resolved lexically; "9:" and the non-string "G:" are ignored.
    auto d = Drives();
    CHECK(d.size() == 3);
    if (d.size() == 3) {
        CHECK_EQ(d[0].letter, L'C');
        CHECK_W(d[0].host_root, root + L"\\croot");
        CHECK_EQ(d[1].letter, L'E');
        CHECK_W(d[1].host_root, root + L"\\sub\\e");
        CHECK_EQ(d[2].letter, L'F');
        CHECK(EndsWithNoCase(d[2].host_root, L"\\vfs\\machine"));
    }
    // Volume numbering follows letter order after C: Volume2 = E, Volume3 = F.
    std::wstring out;
    CHECK_EQ(TranslateNtPath(L"\\Device\\HarddiskVolume2\\x", &out), STATUS_SUCCESS);
    CHECK_W(out, L"\\??\\" + root + L"\\sub\\e\\x");
    CHECK_EQ(TranslateNtPath(L"\\Device\\HarddiskVolume3\\x", &out), STATUS_SUCCESS);
    CHECK(out.find(L"\\vfs\\machine\\x") != std::wstring::npos);
    CHECK_EQ(TranslateNtPath(L"\\Device\\HarddiskVolume4\\x", &out), STATUS_OBJECT_PATH_NOT_FOUND);

    // Overrides: "*" = builtin,native; bad value ignored; short/upper-case spellings accepted.
    CHECK(OverrideFor(L"whatever") == DllOverride::BuiltinThenNative);
    CHECK(OverrideFor(L"bad") == DllOverride::BuiltinThenNative);
    CHECK(OverrideFor(L"short") == DllOverride::Native);
    CHECK(OverrideFor(L"upper") == DllOverride::Builtin);
    CHECK(OverrideFor(L"blank") == DllOverride::Disabled);
    CHECK(OverrideFor(L"kernel32") == DllOverride::Builtin);

    // The native/builtin dirs follow the remapped C:.
    CHECK_W(BuiltinDllPath(L"a"), root + L"\\croot\\Windows\\builtin\\a.dll");
    CHECK_W(NativeDllPath(L"a.dll"), root + L"\\croot\\Windows\\System32\\a.dll");
    W32_OK(L"C:\\Windows\\System32\\a.dll", root + L"\\croot\\Windows\\builtin\\a.dll");   // builtin first
    W32_OK(L"C:\\Users\\x", root + L"\\croot\\Users\\x");
    UN_W32(root + L"\\croot\\Windows\\builtin\\a.dll", L"C:\\Windows\\System32\\a.dll");
    UN_W32(root + L"\\sub\\e\\q.txt", L"E:\\q.txt");
    UN_NO_W32(root + L"\\other");   // the machine root itself is no longer a drive (C: -> croot)
    UseMachine();
}

// ------------------------------------------------------------------ NT translation

TEST(vfs_nt_drive_prefixes) {
    UseMachine();
    const std::wstring want = HostNt(L"\\Users\\User\\marker.txt");
    NT_OK(L"\\??\\C:\\Users\\User\\marker.txt", want);
    NT_OK(L"\\DosDevices\\C:\\Users\\User\\marker.txt", want);
    NT_OK(L"\\GLOBAL??\\C:\\Users\\User\\marker.txt", want);
    NT_OK(L"\\\\?\\C:\\Users\\User\\marker.txt", want);
    NT_OK(L"\\??\\c:\\Users\\User\\marker.txt", want);                     // drive letter case
    NT_OK(L"\\dosdevices\\C:\\Users\\User\\marker.txt", want);              // prefix case
    NT_OK(L"\\??\\C:\\USERS\\user\\MARKER.TXT", HostNt(L"\\USERS\\user\\MARKER.TXT"));   // keeps caller spelling
    NT_OK(L"\\??\\D:\\hello.txt", HostNt(L"\\extra_drive\\hello.txt"));
    NT_OK(L"\\??\\d:\\", HostNt(L"\\extra_drive\\"));
}

TEST(vfs_nt_systemroot_and_device_volume) {
    UseMachine();
    NT_OK(L"\\SystemRoot", HostNt(L"\\Windows"));
    NT_OK(L"\\SystemRoot\\", HostNt(L"\\Windows\\"));
    NT_OK(L"\\SYSTEMROOT\\System32\\drivers\\x.sys", HostNt(L"\\Windows\\System32\\drivers\\x.sys"));
    NT_OK(L"\\SystemRoot\\..\\Users", HostNt(L"\\Users"));
    NT_ST(L"\\SystemRootX\\foo", STATUS_NOT_SUPPORTED);
    NT_OK(L"\\Device\\HarddiskVolume1\\Users\\User", HostNt(L"\\Users\\User"));
    NT_OK(L"\\Device\\HarddiskVolume1", HostNt(L"\\"));
    NT_OK(L"\\device\\harddiskvolume1\\", HostNt(L"\\"));
    NT_OK(L"\\Device\\HarddiskVolume2\\hello.txt", HostNt(L"\\extra_drive\\hello.txt"));
    NT_ST(L"\\Device\\HarddiskVolume3\\x", STATUS_OBJECT_PATH_NOT_FOUND);
    NT_ST(L"\\Device\\HarddiskVolume0\\x", STATUS_OBJECT_PATH_NOT_FOUND);
    NT_ST(L"\\Device\\HarddiskVolumeShadowCopy1\\x", STATUS_NOT_SUPPORTED);
    NT_ST(L"\\Device\\HarddiskVolume\\x", STATUS_NOT_SUPPORTED);
    NT_ST(L"\\Device\\HarddiskVolume1x\\x", STATUS_NOT_SUPPORTED);
}

TEST(vfs_nt_passthrough_namespaces) {
    UseMachine();
    const wchar_t* pass[] = {
        L"\\??\\PIPE\\foo", L"\\??\\pipe\\", L"\\??\\PIPE", L"\\Device\\NamedPipe\\x", L"\\Device\\NamedPipe",
        L"\\\\.\\pipe\\x", L"\\??\\MAILSLOT\\m", L"\\Device\\Mailslot\\m", L"\\??\\UNC\\srv\\share\\x",
        L"\\??\\NUL", L"\\??\\COM1", L"\\??\\GLOBALROOT\\Device\\Foo", L"\\??\\Volume{abc}\\x",
        L"\\??\\PhysicalDrive0", L"\\Device\\Afd\\Endpoint", L"\\Device\\Mup\\srv\\x", L"\\Device\\KsecDD",
        L"\\Registry\\Machine\\Software", L"\\BaseNamedObjects\\Global\\evt", L"\\KnownDlls\\ntdll.dll",
        L"\\Windows\\Theme", L"C:\\x", L"foo\\bar", L"", L"\\", L"\\??\\", L"\\??\\C", L"\\??\\C:x",
    };
    for (const wchar_t* p : pass) {
        std::wstring out = L"<untouched>";
        NTSTATUS s = TranslateNtPath(p, &out);
        CHECK_MSG(s == STATUS_NOT_SUPPORTED, "%ls -> %08lx", p, static_cast<unsigned long>(s));
        CHECK_MSG(out == L"<untouched>", "output modified for %ls", p);
    }
}

TEST(vfs_nt_unmapped_drive_and_invalid) {
    UseMachine();
    NT_ST(L"\\??\\Z:\\x", STATUS_OBJECT_PATH_NOT_FOUND);
    NT_ST(L"\\??\\E:\\", STATUS_OBJECT_PATH_NOT_FOUND);
    NT_ST(L"\\DosDevices\\X:", STATUS_OBJECT_PATH_NOT_FOUND);
    W32_ST(L"Z:\\x", STATUS_OBJECT_PATH_NOT_FOUND);
    const wchar_t nul_path[] = L"\\??\\C:\\a\0b";
    NT_ST(std::wstring_view(nul_path, sizeof(nul_path) / sizeof(wchar_t) - 1), STATUS_OBJECT_NAME_INVALID);
    NT_ST(std::wstring_view(L"\\??\\PIPE\\a\0b", 12), STATUS_OBJECT_NAME_INVALID);
}

TEST(vfs_nt_drive_roots) {
    UseMachine();
    NT_OK(L"\\??\\C:\\", HostNt(L"\\"));
    NT_OK(L"\\??\\C:", HostNt(L"\\"));
    NT_OK(L"\\??\\C:\\.", HostNt(L"\\"));
    NT_OK(L"\\??\\C:\\..\\..", HostNt(L"\\"));         // ".." is clamped at the root
    NT_OK(L"\\??\\C:\\Users\\..\\..\\..\\", HostNt(L"\\"));
    NT_OK(L"\\??\\D:\\", HostNt(L"\\extra_drive\\"));
    NT_OK(L"\\??\\D:\\..\\..", HostNt(L"\\extra_drive\\"));   // cannot escape the drive's host root
}

TEST(vfs_nt_normalisation) {
    UseMachine();
    const std::wstring want = HostNt(L"\\Users\\User\\marker.txt");
    NT_OK(L"\\??\\C:\\Users\\.\\User\\..\\User\\\\marker.txt", want);
    NT_OK(L"\\??\\C:/Users/User/marker.txt", want);
    NT_OK(L"\\??\\C:\\Users/User\\marker.txt", want);
    NT_OK(L"\\??\\C:\\\\Users\\\\\\User\\marker.txt", want);
    NT_OK(L"\\??\\C:\\Users\\User\\", HostNt(L"\\Users\\User\\"));      // trailing separator kept
    NT_OK(L"\\??\\C:\\Users\\User", HostNt(L"\\Users\\User"));
    NT_OK(L"\\??\\C:\\Users\\User\\.", HostNt(L"\\Users\\User"));
    NT_OK(L"\\??\\C:\\Users\\User\\..", HostNt(L"\\Users"));
    NT_OK(L"\\??\\C:\\a\\...\\b", HostNt(L"\\a\\...\\b"));              // "..." is an ordinary name
    NT_OK(L"\\??\\C:\\dir\\file.txt:stream", HostNt(L"\\dir\\file.txt:stream"));   // ADS passes through
    NT_OK(L"\\SystemRoot/System32\\..\\notepad.exe", HostNt(L"\\Windows\\notepad.exe"));

    // Long paths: an NT result has no \\?\ prefix, the Win32 result gains it.
    std::wstring longcomp(300, L'a');
    NT_OK(L"\\??\\C:\\" + longcomp, HostNt(L"\\" + longcomp));
    std::wstring w32;
    CHECK_EQ(TranslateWin32Path(L"C:\\" + longcomp, &w32), STATUS_SUCCESS);
    CHECK_W(w32, L"\\\\?\\" + HostWin32(L"\\" + longcomp));
}

// ------------------------------------------------------------------ Win32 translation

TEST(vfs_win32_forms) {
    UseMachine();
    const std::wstring want = HostWin32(L"\\Users\\User\\marker.txt");
    W32_OK(L"C:\\Users\\User\\marker.txt", want);
    W32_OK(L"c:/users/User/../User/./marker.txt", HostWin32(L"\\users\\User\\marker.txt"));
    W32_OK(L"\\\\.\\C:\\Users\\User\\marker.txt", want);
    W32_OK(L"\\\\?\\C:\\Users\\User\\marker.txt", L"\\\\?\\" + want);   // extended in, extended out
    W32_OK(L"\\\\?\\C:\\Users\\.\\User", L"\\\\?\\" + HostWin32(L"\\Users\\User"));
    W32_OK(L"C:\\", HostWin32(L"\\"));
    W32_OK(L"C:\\..", HostWin32(L"\\"));
    W32_OK(L"D:\\hello.txt", HostWin32(L"\\extra_drive\\hello.txt"));
    W32_OK(L"C:\\Users\\User\\", HostWin32(L"\\Users\\User\\"));

    // Relative, drive-relative and drive-less rooted paths need GuestFullPath first.
    W32_ST(L"foo\\bar", STATUS_OBJECT_NAME_INVALID);
    W32_ST(L"C:foo", STATUS_OBJECT_NAME_INVALID);
    W32_ST(L"C:", STATUS_OBJECT_NAME_INVALID);
    W32_ST(L"\\foo", STATUS_OBJECT_NAME_INVALID);
    W32_ST(L"", STATUS_OBJECT_NAME_INVALID);
    // Namespaces we do not own.
    W32_ST(L"\\\\.\\pipe\\x", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\.\\PIPE\\x", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\.\\COM1", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\server\\share\\x", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\?\\UNC\\server\\share\\x", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\?\\GLOBALROOT\\Device\\x", STATUS_NOT_SUPPORTED);
    W32_ST(L"\\\\?\\Volume{1234}\\x", STATUS_NOT_SUPPORTED);
}

// ------------------------------------------------------------------ DLL overrides

TEST(vfs_override_lookup) {
    UseMachine();
    CHECK(OverrideFor(L"native_only") == DllOverride::NativeThenBuiltin);   // "*"
    CHECK(OverrideFor(L"native_only.dll") == DllOverride::NativeThenBuiltin);
    CHECK(OverrideFor(L"USER32") == DllOverride::Builtin);
    CHECK(OverrideFor(L"User32.DLL") == DllOverride::Builtin);
    // gdi32 is in the GUI stack, always builtin (E1) although the fixture asks for builtin,native
    CHECK(OverrideFor(L"gdi32") == DllOverride::Builtin);
    CHECK(OverrideFor(L"win32u") == DllOverride::Builtin);
    CHECK(OverrideFor(L"imm32.dll") == DllOverride::Builtin);
    CHECK(OverrideFor(L"d3d9") == DllOverride::Disabled);
    CHECK(OverrideFor(L"msvcrt") == DllOverride::Native);
    // E1: ntdll/kernelbase/kernel32 are always builtin, whatever machine.json says
    // (the fixture asks for ntdll=native and KERNEL32.DLL=disabled).
    CHECK(OverrideFor(L"ntdll") == DllOverride::Builtin);
    CHECK(OverrideFor(L"NTDLL.DLL") == DllOverride::Builtin);
    CHECK(OverrideFor(L"kernel32") == DllOverride::Builtin);
    CHECK(OverrideFor(L"kernelbase.dll") == DllOverride::Builtin);
}

TEST(vfs_override_paths) {
    UseMachine();
    CHECK_W(NativeDllPath(L"native_only"), HostWin32(L"\\Windows\\System32\\native_only.dll"));
    CHECK_W(NativeDllPath(L"native_only.dll"), HostWin32(L"\\Windows\\System32\\native_only.dll"));
    CHECK_W(NativeDllPath(L"NATIVE_ONLY"), HostWin32(L"\\Windows\\System32\\NATIVE_ONLY.dll"));
    CHECK_W(NativeDllPath(L"builtin_only"), L"");
    CHECK_W(BuiltinDllPath(L"builtin_only"), HostWin32(L"\\Windows\\builtin\\builtin_only.dll"));
    CHECK_W(BuiltinDllPath(L"native_only"), L"");
    CHECK_W(BuiltinDllPath(L"both"), HostWin32(L"\\Windows\\builtin\\both.dll"));
    CHECK_W(BuiltinDllPath(L"ntdll"), HostWin32(L"\\Windows\\builtin\\ntdll.dll"));
    CHECK_W(BuiltinDllPath(L"nope"), L"");
    CHECK_W(BuiltinDllPath(L"..\\both"), L"");
    CHECK_W(BuiltinDllPath(L"sub/both"), L"");
    CHECK_W(BuiltinDllPath(L"C:both"), L"");
    CHECK_W(BuiltinDllPath(L""), L"");
}

TEST(vfs_override_translation) {
    UseMachine();
    const std::wstring sys = L"\\??\\C:\\Windows\\System32\\";
    const std::wstring nat = L"\\Windows\\System32\\";
    const std::wstring bi = L"\\Windows\\builtin\\";

    // "*": native,builtin -> native if present, else builtin, else the native path.
    NT_OK(sys + L"native_only.dll", HostNt(nat + L"native_only.dll"));
    NT_OK(sys + L"both.dll", HostNt(nat + L"both.dll"));
    NT_OK(sys + L"builtin_only.dll", HostNt(bi + L"builtin_only.dll"));
    NT_OK(sys + L"ghost.dll", HostNt(nat + L"ghost.dll"));
    // builtin
    NT_OK(sys + L"user32.dll", HostNt(bi + L"user32.dll"));
    // builtin,native -> builtin if present, else native
    NT_OK(sys + L"gdi32.dll", HostNt(bi + L"gdi32.dll"));
    NT_OK(sys + L"bn_native_only.dll", HostNt(nat + L"bn_native_only.dll"));
    // native, with no fallback even though the builtin exists
    NT_OK(sys + L"msvcrt.dll", HostNt(nat + L"msvcrt.dll"));
    NT_OK(sys + L"only_b.dll", HostNt(nat + L"only_b.dll"));   // native requested, absent: path still the native one
    // disabled: the file does not exist for the guest
    NT_ST(sys + L"d3d9.dll", STATUS_OBJECT_NAME_NOT_FOUND);
    // E1: always builtin although machine.json says native / disabled
    NT_OK(sys + L"ntdll.dll", HostNt(bi + L"ntdll.dll"));
    NT_OK(sys + L"kernel32.dll", HostNt(bi + L"kernel32.dll"));
    NT_OK(sys + L"kernelbase.dll", HostNt(bi + L"kernelbase.dll"));
    // other executable module extensions follow the same rule
    NT_OK(sys + L"tool.exe", HostNt(nat + L"tool.exe"));

    // Case-insensitive on every component and through the other prefixes.
    NT_OK(L"\\DosDevices\\c:\\WINDOWS\\system32\\USER32.DLL", HostNt(bi + L"USER32.DLL"));
    NT_OK(L"\\SystemRoot\\System32\\user32.dll", HostNt(bi + L"user32.dll"));
    NT_OK(L"\\Device\\HarddiskVolume1\\Windows\\System32\\user32.dll", HostNt(bi + L"user32.dll"));
    NT_OK(L"\\??\\C:/Windows/./System32/../System32/user32.dll", HostNt(bi + L"user32.dll"));

    // Win32 entry point and ResolveSystemModule agree.
    W32_OK(L"C:\\Windows\\System32\\user32.dll", HostWin32(bi + L"user32.dll"));
    W32_ST(L"C:\\Windows\\System32\\d3d9.dll", STATUS_OBJECT_NAME_NOT_FOUND);
    std::wstring p;
    CHECK_EQ(ResolveSystemModule(L"user32", &p), STATUS_SUCCESS);
    CHECK_W(p, HostWin32(bi + L"user32.dll"));
    CHECK_EQ(ResolveSystemModule(L"both.dll", &p), STATUS_SUCCESS);
    CHECK_W(p, HostWin32(nat + L"both.dll"));
    CHECK_EQ(ResolveSystemModule(L"d3d9", &p), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_EQ(ResolveSystemModule(L"a\\b.dll", &p), STATUS_OBJECT_NAME_INVALID);

    // Only exactly <C:\Windows\System32>\<module> is routed.
    NT_OK(L"\\??\\C:\\Windows\\user32.dll", HostNt(L"\\Windows\\user32.dll"));
    NT_OK(L"\\??\\C:\\Windows\\System32\\drivers\\user32.dll", HostNt(L"\\Windows\\System32\\drivers\\user32.dll"));
    NT_OK(L"\\??\\C:\\Windows\\System32\\user32.dll\\", HostNt(L"\\Windows\\System32\\user32.dll\\"));
    NT_OK(L"\\??\\C:\\Windows\\System32\\user32.txt", HostNt(nat + L"user32.txt"));
    NT_OK(L"\\??\\C:\\Windows\\System32\\", HostNt(nat));
    NT_OK(L"\\??\\C:\\Windows\\System32", HostNt(L"\\Windows\\System32"));
    NT_OK(L"\\??\\D:\\Windows\\System32\\user32.dll", HostNt(L"\\extra_drive\\Windows\\System32\\user32.dll"));
    // Direct access to the builtin dir is just a normal path.
    NT_OK(L"\\??\\C:\\Windows\\builtin\\user32.dll", HostNt(bi + L"user32.dll"));
}

// ------------------------------------------------------------------ untranslate

TEST(vfs_untranslate_basic) {
    UseMachine();
    UN_W32(HostWin32(L"\\Users\\User\\marker.txt"), L"C:\\Users\\User\\marker.txt");
    UN_NT(HostNt(L"\\Users\\User\\marker.txt"), L"\\??\\C:\\Users\\User\\marker.txt");
    UN_W32(HostNt(L"\\Users\\User"), L"C:\\Users\\User");                // NT input to the Win32 function
    UN_NT(HostWin32(L"\\Users\\User"), L"\\??\\C:\\Users\\User");       // and vice versa
    UN_W32(L"\\\\?\\" + HostWin32(L"\\Users"), L"\\\\?\\C:\\Users");     // \\?\ is preserved for Win32
    UN_NT(L"\\\\?\\" + HostWin32(L"\\Users"), L"\\??\\C:\\Users");
    UN_W32(HostWin32(L""), L"C:\\");
    UN_W32(HostWin32(L"\\"), L"C:\\");
    UN_NT(L"\\??\\" + g_root, L"\\??\\C:\\");
    UN_W32(HostWin32(L"\\Users\\User\\"), L"C:\\Users\\User\\");
    UN_W32(HostWin32(L"\\Users\\..\\Windows"), L"C:\\Windows");
    // Case-insensitive host prefix; remainder keeps its spelling.
    std::wstring upper = g_root;
    for (auto& c : upper) c = win10xbox::vfs::detail::FoldChar(c);
    UN_W32(upper + L"\\users\\User", L"C:\\users\\User");
    // Mixed slashes in the host path.
    std::wstring fwd = g_root;
    for (auto& c : fwd) if (c == L'\\') c = L'/';
    UN_W32(fwd + L"/Users/User", L"C:\\Users\\User");
    // The most specific root wins: D: lives inside C:'s host tree.
    UN_W32(HostWin32(L"\\extra_drive\\hello.txt"), L"D:\\hello.txt");
    UN_W32(HostWin32(L"\\extra_drive"), L"D:\\");
    UN_W32(HostWin32(L"\\extra_drive_other\\x"), L"C:\\extra_drive_other\\x");   // component boundary
}

TEST(vfs_untranslate_builtin_dir_maps_to_system32) {
    UseMachine();
    UN_W32(HostWin32(L"\\Windows\\builtin\\both.dll"), L"C:\\Windows\\System32\\both.dll");
    UN_NT(HostNt(L"\\Windows\\builtin\\ntdll.dll"), L"\\??\\C:\\Windows\\System32\\ntdll.dll");
    UN_W32(HostWin32(L"\\Windows\\builtin"), L"C:\\Windows\\System32");
    UN_W32(HostWin32(L"\\Windows\\BUILTIN\\en-US\\x.mui"), L"C:\\Windows\\System32\\en-US\\x.mui");
    UN_W32(HostWin32(L"\\Windows\\builtinx\\a"), L"C:\\Windows\\builtinx\\a");
    UN_W32(HostWin32(L"\\Windows\\System32\\both.dll"), L"C:\\Windows\\System32\\both.dll");
}

TEST(vfs_untranslate_outside_mappings) {
    UseMachine();
    UN_NO_W32(g_root + L"_other\\x");                   // sibling sharing a prefix
    UN_NO_W32(L"Z:\\definitely\\elsewhere\\x");
    UN_NO_NT(L"\\??\\Z:\\definitely\\elsewhere\\x");
    UN_NO_NT(L"\\??\\UNC\\srv\\share\\x");
    UN_NO_W32(L"\\\\srv\\share\\x");
    UN_NO_NT(L"\\Device\\HarddiskVolume3\\x");
    UN_NO_W32(L"relative\\path");
    UN_NO_W32(L"");
    UN_NO_NT(L"\\??\\PIPE\\x");
}

TEST(vfs_roundtrip_guest_host_guest) {
    UseMachine();
    const wchar_t* paths[] = {
        L"C:\\", L"C:\\Users\\User\\marker.txt", L"C:\\Users\\User\\", L"D:\\hello.txt", L"D:\\",
        L"C:\\Windows\\System32\\native_only.dll", L"C:\\Windows\\System32\\builtin_only.dll",
        L"C:\\Windows\\System32\\user32.dll", L"C:\\Windows\\", L"C:\\Some Dir\\caf\u00e9.txt",
    };
    for (const wchar_t* g : paths) {
        std::wstring host, back;
        CHECK_MSG(TranslateWin32Path(g, &host) == STATUS_SUCCESS, "translate %ls", g);
        CHECK_MSG(UntranslateWin32Path(host, &back) == STATUS_SUCCESS, "untranslate %ls", host.c_str());
        CHECK_MSG(back == g, "win32 round trip %ls -> %ls -> %ls", g, host.c_str(), back.c_str());

        std::wstring nt_guest = std::wstring(L"\\??\\") + g, nt_host, nt_back;
        CHECK_MSG(TranslateNtPath(nt_guest, &nt_host) == STATUS_SUCCESS, "nt translate %ls", g);
        CHECK_MSG(UntranslateNtPath(nt_host, &nt_back) == STATUS_SUCCESS, "nt untranslate %ls", nt_host.c_str());
        CHECK_MSG(nt_back == nt_guest, "nt round trip %ls -> %ls -> %ls", nt_guest.c_str(), nt_host.c_str(), nt_back.c_str());
    }
}

// ------------------------------------------------------------------ OBJECT_ATTRIBUTES

namespace {
OBJECT_ATTRIBUTES MakeAttr(UNICODE_STRING* us, HANDLE root = nullptr, ULONG attrs = 0x40) {
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    oa.RootDirectory = root;
    oa.ObjectName = us;
    oa.Attributes = attrs;   // OBJ_CASE_INSENSITIVE
    oa.SecurityDescriptor = reinterpret_cast<PVOID>(0x1234);
    oa.SecurityQualityOfService = reinterpret_cast<PVOID>(0x5678);
    return oa;
}
UNICODE_STRING MakeUs(const std::wstring& s) {
    UNICODE_STRING us{};
    us.Buffer = const_cast<PWSTR>(s.data());
    us.Length = static_cast<USHORT>(s.size() * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    return us;
}
}  // namespace

TEST(vfs_translate_object_attributes_rewrites_name) {
    UseMachine();
    std::wstring guest = L"\\??\\C:\\Users\\User\\marker.txt";
    UNICODE_STRING us = MakeUs(guest);
    OBJECT_ATTRIBUTES oa = MakeAttr(&us);
    TranslatedAttributes ta;
    CHECK_EQ(TranslateObjectAttributes(&oa, &ta), STATUS_SUCCESS);
    CHECK(ta.translated);
    std::wstring want = HostNt(L"\\Users\\User\\marker.txt");
    CHECK(ta.attr.ObjectName == &ta.name);
    CHECK(ta.attr.ObjectName != &us);
    CHECK_EQ(static_cast<size_t>(ta.name.Length), want.size() * sizeof(wchar_t));
    CHECK_EQ(static_cast<size_t>(ta.name.MaximumLength), want.size() * sizeof(wchar_t) + sizeof(wchar_t));
    CHECK(ta.name.Buffer != nullptr);
    if (ta.name.Buffer) {
        CHECK(std::wstring(ta.name.Buffer, ta.name.Length / sizeof(wchar_t)) == want);
        CHECK_EQ(ta.name.Buffer[ta.name.Length / sizeof(wchar_t)], L'\0');
    }
    // everything else is copied verbatim
    CHECK_EQ(ta.attr.Length, static_cast<ULONG>(sizeof(OBJECT_ATTRIBUTES)));
    CHECK(ta.attr.RootDirectory == nullptr);
    CHECK_EQ(ta.attr.Attributes, 0x40ul);
    CHECK(ta.attr.SecurityDescriptor == reinterpret_cast<PVOID>(0x1234));
    CHECK(ta.attr.SecurityQualityOfService == reinterpret_cast<PVOID>(0x5678));
    // the guest's own UNICODE_STRING is not modified
    CHECK(us.Buffer == guest.data() && us.Length == guest.size() * sizeof(wchar_t));

    // A name that is not NUL-terminated and is longer than Length: Length rules.
    std::wstring buf = L"\\??\\D:\\hello.txtGARBAGE";
    UNICODE_STRING us2 = MakeUs(buf);
    us2.Length = static_cast<USHORT>(std::wstring(L"\\??\\D:\\hello.txt").size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES oa2 = MakeAttr(&us2);
    TranslatedAttributes ta2;
    CHECK_EQ(TranslateObjectAttributes(&oa2, &ta2), STATUS_SUCCESS);
    CHECK(ta2.translated);
    CHECK(ta2.storage == HostNt(L"\\extra_drive\\hello.txt"));

    // A DLL under an override is routed.
    std::wstring dll = L"\\??\\C:\\Windows\\System32\\user32.dll";
    UNICODE_STRING us3 = MakeUs(dll);
    OBJECT_ATTRIBUTES oa3 = MakeAttr(&us3);
    TranslatedAttributes ta3;
    CHECK_EQ(TranslateObjectAttributes(&oa3, &ta3), STATUS_SUCCESS);
    CHECK(ta3.storage == HostNt(L"\\Windows\\builtin\\user32.dll"));
}

TEST(vfs_translate_object_attributes_leaves_others_alone) {
    UseMachine();
    // Name relative to a (host) directory handle: untouched, handle preserved.
    std::wstring rel = L"Users\\User\\marker.txt";
    UNICODE_STRING us = MakeUs(rel);
    HANDLE fake = reinterpret_cast<HANDLE>(0x44);
    OBJECT_ATTRIBUTES oa = MakeAttr(&us, fake);
    TranslatedAttributes ta;
    CHECK_EQ(TranslateObjectAttributes(&oa, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated);
    CHECK(ta.attr.ObjectName == &us);
    CHECK(ta.attr.RootDirectory == fake);

    // Even if the name looks absolute, a RootDirectory means "do not rewrite".
    std::wstring abs = L"\\??\\C:\\x";
    UNICODE_STRING us_abs = MakeUs(abs);
    OBJECT_ATTRIBUTES oa_abs = MakeAttr(&us_abs, fake);
    CHECK_EQ(TranslateObjectAttributes(&oa_abs, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated && ta.attr.ObjectName == &us_abs);

    // Namespaces we do not own pass through successfully, untouched.
    std::wstring pipe = L"\\??\\PIPE\\mypipe";
    UNICODE_STRING us_pipe = MakeUs(pipe);
    OBJECT_ATTRIBUTES oa_pipe = MakeAttr(&us_pipe);
    CHECK_EQ(TranslateObjectAttributes(&oa_pipe, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated && ta.attr.ObjectName == &us_pipe);
    std::wstring ev = L"\\BaseNamedObjects\\evt";
    UNICODE_STRING us_ev = MakeUs(ev);
    OBJECT_ATTRIBUTES oa_ev = MakeAttr(&us_ev);
    CHECK_EQ(TranslateObjectAttributes(&oa_ev, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated && ta.attr.ObjectName == &us_ev);

    // No name / empty name / null attributes.
    OBJECT_ATTRIBUTES oa_none = MakeAttr(nullptr);
    CHECK_EQ(TranslateObjectAttributes(&oa_none, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated && ta.attr.ObjectName == nullptr);
    UNICODE_STRING empty{};
    OBJECT_ATTRIBUTES oa_empty = MakeAttr(&empty);
    CHECK_EQ(TranslateObjectAttributes(&oa_empty, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated);
    CHECK_EQ(TranslateObjectAttributes(nullptr, &ta), STATUS_SUCCESS);
    CHECK(!ta.translated && ta.attr.ObjectName == nullptr && ta.attr.Length == 0);
}

TEST(vfs_translate_object_attributes_errors) {
    UseMachine();
    std::wstring z = L"\\??\\Z:\\x";
    UNICODE_STRING us = MakeUs(z);
    OBJECT_ATTRIBUTES oa = MakeAttr(&us);
    TranslatedAttributes ta;
    CHECK_EQ(TranslateObjectAttributes(&oa, &ta), STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(!ta.translated);

    std::wstring dis = L"\\??\\C:\\Windows\\System32\\d3d9.dll";
    UNICODE_STRING us_d = MakeUs(dis);
    OBJECT_ATTRIBUTES oa_d = MakeAttr(&us_d);
    CHECK_EQ(TranslateObjectAttributes(&oa_d, &ta), STATUS_OBJECT_NAME_NOT_FOUND);

    OBJECT_ATTRIBUTES small = MakeAttr(&us);
    small.Length = 8;
    CHECK_EQ(TranslateObjectAttributes(&small, &ta), STATUS_INVALID_PARAMETER);

    // The result must fit a UNICODE_STRING (max 32767 chars).
    std::wstring huge = L"\\??\\C:\\" + std::wstring(33000, L'a');
    UNICODE_STRING us_h{};
    us_h.Buffer = const_cast<PWSTR>(huge.data());
    us_h.Length = 0xFFFE;   // 32767 chars: the longest legal input; the host prefix pushes the result over
    us_h.MaximumLength = 0xFFFE;
    OBJECT_ATTRIBUTES oa_h = MakeAttr(&us_h);
    NTSTATUS s = TranslateObjectAttributes(&oa_h, &ta);
    CHECK_MSG(s == STATUS_NAME_TOO_LONG || s == STATUS_SUCCESS, "unexpected %08lx", static_cast<unsigned long>(s));
    if (s == STATUS_SUCCESS) CHECK(ta.name.Length <= 0xFFFE);
}

// ------------------------------------------------------------------ GuestFullPath & guest dirs

TEST(vfs_guest_full_path) {
    UseMachine();
    const std::wstring cwd = L"C:\\Users\\User";
    auto full = [&](std::wstring_view p, std::wstring_view c, std::wstring want, NTSTATUS st = STATUS_SUCCESS) {
        std::wstring out = L"<untouched>";
        NTSTATUS s = GuestFullPath(p, c, &out);
        CHECK_MSG(s == st, "GuestFullPath(%ls) = %08lx", std::wstring(p).c_str(), static_cast<unsigned long>(s));
        if (st == STATUS_SUCCESS) CHECK_MSG(out == want, "GuestFullPath(%ls) = %ls, want %ls",
                                            std::wstring(p).c_str(), out.c_str(), want.c_str());
    };
    full(L"foo\\bar", cwd, L"C:\\Users\\User\\foo\\bar");
    full(L"..\\x", cwd, L"C:\\Users\\x");
    full(L".", cwd, L"C:\\Users\\User");
    full(L"\\Windows\\", cwd, L"C:\\Windows\\");
    full(L"D:\\a\\..\\b", cwd, L"D:\\b");
    full(L"C:rel", cwd, L"C:\\Users\\User\\rel");     // drive-relative on the cwd's drive
    full(L"D:rel", cwd, L"D:\\rel");                  // on another drive: that drive's root
    full(L"C:", cwd, L"C:\\Users\\User");
    full(L"d:/x/./y", cwd, L"D:\\x\\y");
    full(L"..\\..\\..\\..", cwd, L"C:\\");
    full(L"\\\\?\\C:\\a/b", cwd, L"\\\\?\\C:\\a\\b");
    full(L"\\\\srv\\share/x", cwd, L"\\\\srv\\share\\x");
    full(L"C:\\x", L"", L"C:\\x");                    // absolute needs no cwd
    full(L"foo", L"", L"", STATUS_OBJECT_NAME_INVALID);
    full(L"foo", L"relative\\cwd", L"", STATUS_OBJECT_NAME_INVALID);
    full(L"\\foo", L"", L"", STATUS_OBJECT_NAME_INVALID);
    full(L"", cwd, L"", STATUS_OBJECT_NAME_INVALID);
}

TEST(vfs_guest_directories) {
    UseMachine();
    CHECK_W(GuestSystemRoot(), L"C:\\Windows");
    CHECK_W(GuestSystemDirectory(), L"C:\\Windows\\System32");
    CHECK_W(GuestUserProfile(), L"C:\\Users\\User");
    CHECK_W(GuestTempDirectory(), L"C:\\Users\\User\\AppData\\Local\\Temp");
    // The guest directories translate into the fixture machine.
    std::wstring host;
    CHECK_EQ(TranslateWin32Path(GuestUserProfile() + L"\\marker.txt", &host), STATUS_SUCCESS);
    CHECK_W(host, HostWin32(L"\\Users\\User\\marker.txt"));
    DWORD attr = GetFileAttributesW(host.c_str());
    CHECK(attr != INVALID_FILE_ATTRIBUTES);   // the fixture marker really exists on the host
}

TEST(vfs_fixture_files_exist_on_host) {
    UseMachine();
    const wchar_t* files[] = {L"\\Windows\\System32\\native_only.dll", L"\\Windows\\builtin\\builtin_only.dll",
                              L"\\Users\\User\\marker.txt", L"\\extra_drive\\hello.txt"};
    for (const wchar_t* f : files) {
        DWORD a = GetFileAttributesW(HostWin32(f).c_str());
        CHECK_MSG(a != INVALID_FILE_ATTRIBUTES, "missing fixture file %ls", f);
    }
}
