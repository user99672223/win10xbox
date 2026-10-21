// Generates the synthetic registry hives used by tests/unit/test_registry.cpp:
//   <out>\Windows\System32\config\SOFTWARE
//   <out>\Windows\System32\config\SYSTEM
//   <out>\Users\User\NTUSER.DAT
// The content is invented for tests (never copied from a Microsoft hive) and
// the output is deterministic, so regenerating produces identical bytes.
//   w10x_mkfixture <output machine dir>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "regf.h"

using namespace win10xbox::registry::regf;

namespace {

constexpr std::uint64_t kStamp = 0x01D9A8F0C3E4A000ull;   // fixed FILETIME for reproducibility

Key* Path(Key* k, const std::wstring& path) {
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find(L'\\', i);
        if (j == std::wstring::npos) j = path.size();
        std::wstring part = path.substr(i, j - i);
        Key* next = nullptr;
        for (auto& c : k->subkeys) if (c->name == part) { next = c.get(); break; }
        k = next ? next : k->AddKey(part);
        i = j + 1;
    }
    return k;
}

Key NewRoot(const wchar_t* name) {
    Key r;
    r.name = name;
    r.timestamp = kStamp;
    return r;
}

bool MakeDirs(std::wstring dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
            std::wstring p = dir.substr(0, i);
            if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

bool Emit(const std::wstring& path, const Key& root, const wchar_t* hive_name) {
    std::vector<std::uint8_t> bytes;
    if (!WriteHive(root, hive_name, &bytes)) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) && w == bytes.size();
    CloseHandle(f);
    std::wprintf(L"wrote %ls (%lu bytes)\n", path.c_str(), static_cast<unsigned long>(bytes.size()));
    return ok;
}

Key MakeSoftware() {
    Key root = NewRoot(L"ROOT");
    Key* cv = Path(&root, L"Microsoft\\Windows NT\\CurrentVersion");
    cv->AddString(L"ProductName", L"Windows 10 Pro");
    cv->AddString(L"CurrentBuild", L"19045");
    cv->AddString(L"CurrentBuildNumber", L"19045");
    cv->AddDword(L"CurrentMajorVersionNumber", 10);
    cv->AddDword(L"CurrentMinorVersionNumber", 0);
    cv->AddString(L"CurrentVersion", L"6.3");
    cv->AddDword(L"UBR", 3803);
    cv->AddString(L"DisplayVersion", L"22H2");
    cv->AddString(L"EditionID", L"Professional");
    cv->AddString(L"SystemRoot", L"C:\\Windows");
    cv->AddString(L"BuildLab", L"19041.vb_release.191206-1406");
    Key* wcv = Path(&root, L"Microsoft\\Windows\\CurrentVersion");
    wcv->AddString(L"ProgramFilesDir", L"C:\\Program Files");

    Key* fx = Path(&root, L"Fixture");
    fx->AddString(L"", L"default value");                         // unnamed value
    fx->AddString(L"String", L"hello");
    fx->AddString(L"Expand", L"%SystemRoot%\\system32", 2);
    {
        std::wstring multi = L"one";
        multi.push_back(L'\0'); multi += L"two"; multi.push_back(L'\0'); multi.push_back(L'\0');
        std::vector<std::uint8_t> d(multi.size() * 2);
        std::memcpy(d.data(), multi.data(), d.size());
        fx->AddValue(L"Multi", 7, std::move(d));
    }
    {
        std::vector<std::uint8_t> q = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        fx->AddValue(L"Qword", 11, std::move(q));
    }
    fx->AddValue(L"Inline", 3, {0xAA, 0xBB, 0xCC});
    fx->AddValue(L"Empty", 3, {});
    {
        std::vector<std::uint8_t> big(40000);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
        fx->AddValue(L"Big", 3, std::move(big));                  // > 16344 bytes: db record
    }
    fx->AddValue(L"Wide-Name-\u00e9", 3, {1, 2, 3, 4, 5, 6});
    Path(fx, L"Charlie");
    Path(fx, L"alpha");
    Path(fx, L"Bravo");
    Path(fx, L"\u00dcn\u00ef")->AddDword(L"Unicode", 1);
    Path(fx, L"WithClass")->class_name = L"FixtureClass";
    Key* many = Path(fx, L"Many");
    for (int i = 0; i < 700; ++i) {                               // > 500 subkeys: ri + several lh lists
        std::wstring nm = std::to_wstring(i);
        while (nm.size() < 4) nm.insert(nm.begin(), L'0');
        many->AddKey(L"K" + nm)->AddDword(L"Index", static_cast<std::uint32_t>(i));
    }
    Key* link = Path(fx, L"Link");
    link->symlink = true;
    {
        std::wstring target = L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        std::vector<std::uint8_t> d(target.size() * 2);
        std::memcpy(d.data(), target.data(), d.size());
        link->AddValue(L"SymbolicLinkValue", 6, std::move(d));
    }
    return root;
}

Key MakeSystem() {
    Key root = NewRoot(L"SYSTEM");
    Key* sel = Path(&root, L"Select");
    sel->AddDword(L"Current", 1);
    sel->AddDword(L"Default", 1);
    sel->AddDword(L"Failed", 0);
    sel->AddDword(L"LastKnownGood", 1);
    Path(&root, L"ControlSet001\\Control\\ComputerName\\ComputerName")->AddString(L"ComputerName", L"FIXTURE-PC");
    Path(&root, L"ControlSet001\\Control\\Session Manager\\Environment")->AddString(L"TEMP", L"C:\\Windows\\Temp");
    Key* svc = Path(&root, L"ControlSet001\\Services\\Fixture\\Parameters");
    svc->AddString(L"ServiceDll", L"%SystemRoot%\\System32\\fixture.dll", 2);
    return root;
}

Key MakeNtUser() {
    Key root = NewRoot(L"ROOT");
    Key* env = Path(&root, L"Environment");
    env->AddString(L"TEMP", L"%USERPROFILE%\\AppData\\Local\\Temp", 2);
    env->AddString(L"TMP", L"%USERPROFILE%\\AppData\\Local\\Temp", 2);
    Path(&root, L"Software\\Fixture\\Setting")->AddDword(L"Level", 7);
    return root;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: w10x_mkfixture <output machine dir>\n");
        return 2;
    }
    std::wstring out = argv[1];
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    std::wstring cfg = out + L"\\Windows\\System32\\config";
    std::wstring usr = out + L"\\Users\\User";
    if (!MakeDirs(cfg) || !MakeDirs(usr)) { std::fwprintf(stderr, L"cannot create directories under %ls\n", out.c_str()); return 1; }
    bool ok = Emit(cfg + L"\\SOFTWARE", MakeSoftware(), L"SOFTWARE");
    ok = Emit(cfg + L"\\SYSTEM", MakeSystem(), L"SYSTEM") && ok;
    ok = Emit(usr + L"\\NTUSER.DAT", MakeNtUser(), L"NTUSER.DAT") && ok;
    return ok ? 0 : 1;
}
