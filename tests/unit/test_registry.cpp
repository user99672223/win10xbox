// Registry module tests: REGF reading, NT buffer semantics, overlay, persistence.
// Fixture hives are synthetic (tests/fixtures/registry/machine, produced by
// machine/hivex/mkfixture.cpp) -- never Microsoft files.
#include "check.h"
#include "win10xbox/config.h"
#include "win10xbox/registry.h"

#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reg = win10xbox::registry;
namespace nt = win10xbox::registry::nt;

namespace {

constexpr ULONG kRead = 0x20019;        // KEY_READ
constexpr ULONG kAll = 0xF003F;         // KEY_ALL_ACCESS
constexpr ULONG kRegSz = 1, kRegDword = 4;
constexpr const wchar_t* kSoftware = L"\\Registry\\Machine\\SOFTWARE";
constexpr const wchar_t* kUserSid = L"S-1-5-21-1004336348-1177238915-682003330-1001";

// ---- path helpers -------------------------------------------------------------

std::wstring HostPath(const char* p) {
    std::wstring w;
    for (; *p; ++p) w += (*p == '/') ? L'\\' : static_cast<wchar_t>(static_cast<unsigned char>(*p));
    if (!w.empty() && w[0] == L'\\') w = L"Z:" + w;    // Wine: unix absolute paths live on Z:
    return w;
}

std::wstring FixtureRoot() { return HostPath(WIN10XBOX_FIXTURES_DIR) + L"\\registry\\machine"; }

std::wstring TempDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"w10x_reg_" + tag + L"_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void MakeDirs(const std::wstring& d) {
    for (std::size_t i = 3; i <= d.size(); ++i)
        if (i == d.size() || d[i] == L'\\') CreateDirectoryW(d.substr(0, i).c_str(), nullptr);
}

// Copies the fixture machine tree into dir so tests that write files stay off the repo.
void CopyFixture(const std::wstring& dir) {
    const std::wstring src = FixtureRoot();
    for (const wchar_t* rel : {L"\\Windows\\System32\\config\\SOFTWARE", L"\\Windows\\System32\\config\\SYSTEM", L"\\Users\\User\\NTUSER.DAT"}) {
        std::wstring dst = dir + rel;
        MakeDirs(dst.substr(0, dst.rfind(L'\\')));
        CopyFileW((src + rel).c_str(), dst.c_str(), FALSE);
    }
}

std::uint64_t FileHash(const std::wstring& path, std::uint64_t* size = nullptr) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    std::uint64_t h = 1469598103934665603ull, total = 0;   // FNV-1a 64
    unsigned char buf[65536];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) {
        for (DWORD i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ull; }
        total += n;
    }
    CloseHandle(f);
    if (size) *size = total;
    return h;
}

// ---- NT call helpers ---------------------------------------------------------

struct Us {
    std::wstring s;
    UNICODE_STRING u;
    explicit Us(const std::wstring& t) : s(t) {
        u.Buffer = s.data();
        u.Length = static_cast<USHORT>(s.size() * 2);
        u.MaximumLength = static_cast<USHORT>(s.size() * 2 + 2);
    }
};

struct Oa {
    Us name;
    OBJECT_ATTRIBUTES a;
    Oa(const std::wstring& path, HANDLE root = nullptr, ULONG attrs = 0x40) : name(path) {
        std::memset(&a, 0, sizeof a);
        a.Length = sizeof a;
        a.RootDirectory = root;
        a.ObjectName = &name.u;
        a.Attributes = attrs;
    }
};

HANDLE Open(const std::wstring& path, ACCESS_MASK access = kRead, HANDLE root = nullptr) {
    Oa oa(path, root);
    HANDLE h = nullptr;
    NTSTATUS s = reg::Reg_NtOpenKey(&h, access, &oa.a);
    return s >= 0 ? h : nullptr;
}

NTSTATUS OpenStatus(const std::wstring& path, ACCESS_MASK access = kRead) {
    Oa oa(path);
    HANDLE h = nullptr;
    NTSTATUS s = reg::Reg_NtOpenKey(&h, access, &oa.a);
    if (s >= 0) reg::Close(h);
    return s;
}

NTSTATUS Create(const std::wstring& path, HANDLE* h, ULONG* disp = nullptr, ACCESS_MASK access = kAll) {
    Oa oa(path);
    return reg::Reg_NtCreateKey(h, access, &oa.a, 0, nullptr, 0, disp);
}

// NtCreateKey makes only the last component of a name (a missing key on the way is STATUS_OBJECT_NAME_NOT_FOUND), so
// a deep key is made one level at a time, the way RegCreateKeyEx does it.
NTSTATUS CreateDeep(const std::wstring& path, HANDLE* h, ULONG* disp = nullptr) {
    for (std::size_t p = path.find(L'\\', 1); p != std::wstring::npos; p = path.find(L'\\', p + 1)) {
        HANDLE t = nullptr;
        if (Create(path.substr(0, p), &t) >= 0) reg::Close(t);
    }
    return Create(path, h, disp);
}

template <typename T>
T Get(const std::vector<std::uint8_t>& b, std::size_t off) { T v; std::memcpy(&v, b.data() + off, sizeof v); return v; }

std::wstring WideAt(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t bytes) {
    return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + off), bytes / 2);
}

// Returns status; fills buf with the bytes written and *need with ResultLength.
NTSTATUS QueryValue(HANDLE h, const wchar_t* name, ULONG cls, ULONG len, std::vector<std::uint8_t>* buf, ULONG* need = nullptr) {
    Us n(name);
    buf->assign(len, 0xCD);
    ULONG rl = 0xDEADBEEF;
    NTSTATUS s = reg::Reg_NtQueryValueKey(h, &n.u, cls, buf->data(), len, &rl);
    if (need) *need = rl;
    return s;
}

NTSTATUS SetDword(HANDLE h, const wchar_t* name, std::uint32_t v) {
    Us n(name);
    return reg::Reg_NtSetValueKey(h, &n.u, 0, kRegDword, &v, 4);
}

NTSTATUS SetString(HANDLE h, const wchar_t* name, const std::wstring& v) {
    Us n(name);
    return reg::Reg_NtSetValueKey(h, &n.u, 0, kRegSz, const_cast<wchar_t*>(v.c_str()), static_cast<ULONG>((v.size() + 1) * 2));
}

std::vector<std::wstring> SubkeyNames(HANDLE h) {
    std::vector<std::wstring> out;
    for (ULONG i = 0;; ++i) {
        std::vector<std::uint8_t> b(512);
        ULONG rl = 0;
        NTSTATUS s = reg::Reg_NtEnumerateKey(h, i, reg::kKeyBasic, b.data(), 512, &rl);
        if (s < 0) break;
        out.push_back(WideAt(b, 16, Get<ULONG>(b, 12)));
    }
    return out;
}

std::vector<std::wstring> ValueNames(HANDLE h) {
    std::vector<std::wstring> out;
    for (ULONG i = 0;; ++i) {
        std::vector<std::uint8_t> b(1024);
        ULONG rl = 0;
        NTSTATUS s = reg::Reg_NtEnumerateValueKey(h, i, reg::kValueBasic, b.data(), 1024, &rl);
        if (s < 0) break;
        out.push_back(WideAt(b, 12, Get<ULONG>(b, 8)));
    }
    return out;
}

bool Has(const std::vector<std::wstring>& v, const wchar_t* s) {
    for (auto& x : v) if (x == s) return true;
    return false;
}

struct Session {
    explicit Session(const std::wstring& root) { init = reg::Initialize(root); }
    ~Session() { reg::Shutdown(); }
    NTSTATUS init;
};

}  // namespace

// ---------------------------------------------------------------------------

TEST(registry_convenience_reads) {
    Session s(FixtureRoot());
    CHECK_EQ(s.init, nt::kSuccess);

    std::wstring str;
    const std::wstring cv = std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion";
    CHECK_EQ(reg::QueryString(cv, L"ProductName", &str), nt::kSuccess);
    CHECK(str == L"Windows 10 Pro");
    CHECK_EQ(reg::QueryString(cv, L"CurrentBuild", &str), nt::kSuccess);
    CHECK(str == L"19045");
    // case-insensitive path and value name
    CHECK_EQ(reg::QueryString(L"\\REGISTRY\\machine\\software\\MICROSOFT\\windows nt\\currentversion", L"PRODUCTNAME", &str), nt::kSuccess);
    CHECK(str == L"Windows 10 Pro");

    std::uint32_t dw = 0;
    CHECK_EQ(reg::QueryDword(cv, L"CurrentMajorVersionNumber", &dw), nt::kSuccess);
    CHECK_EQ(dw, 10u);
    CHECK_EQ(reg::QueryDword(cv, L"UBR", &dw), nt::kSuccess);
    CHECK_EQ(dw, 3803u);
    CHECK_EQ(reg::QueryDword(cv, L"ProductName", &dw), nt::kObjectTypeMismatch);
    CHECK_EQ(reg::QueryString(cv, L"NoSuchValue", &str), nt::kObjectNameNotFound);
    CHECK_EQ(reg::QueryString(std::wstring(kSoftware) + L"\\NoSuchKey", L"x", &str), nt::kObjectNameNotFound);

    // SYSTEM hive and the CurrentControlSet -> ControlSet001 link
    CHECK_EQ(reg::QueryString(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName", L"ComputerName", &str), nt::kSuccess);
    CHECK(str == L"FIXTURE-PC");
    CHECK_EQ(reg::QueryString(L"\\Registry\\Machine\\SYSTEM\\ControlSet001\\Control\\ComputerName\\ComputerName", L"ComputerName", &str), nt::kSuccess);

    // NTUSER.DAT mounted at \Registry\User\<sid>
    CHECK_EQ(reg::QueryString(std::wstring(L"\\Registry\\User\\") + kUserSid + L"\\Environment", L"TEMP", &str), nt::kSuccess);
    CHECK(str == L"%USERPROFILE%\\AppData\\Local\\Temp");

    // key id API
    reg::KeyId k = 0;
    CHECK_EQ(reg::OpenKey(std::wstring(kSoftware) + L"\\Fixture", &k), nt::kSuccess);
    reg::Value v;
    CHECK_EQ(reg::QueryValue(k, L"String", &v), nt::kSuccess);
    CHECK_EQ(v.type, 1ul);
    CHECK_EQ(v.data.size(), 12u);
    CHECK_EQ(reg::QueryValue(k, L"", &v), nt::kSuccess);   // unnamed default value
    CHECK_EQ(reg::QueryValue(k, L"Qword", &v), nt::kSuccess);
    CHECK_EQ(v.type, 11ul);
    CHECK_EQ(v.data.size(), 8u);
    CHECK_EQ(reg::QueryValue(k, L"Big", &v), nt::kSuccess);   // db record
    CHECK_EQ(v.data.size(), 40000u);
    bool pattern_ok = true;
    for (size_t i = 0; i < v.data.size(); ++i) if (v.data[i] != static_cast<std::uint8_t>((i * 7 + 3) & 0xFF)) { pattern_ok = false; break; }
    CHECK(pattern_ok);
    CHECK_EQ(reg::QueryValue(k, L"Empty", &v), nt::kSuccess);
    CHECK_EQ(v.data.size(), 0u);
    CHECK_EQ(reg::QueryValue(k, L"Inline", &v), nt::kSuccess);
    CHECK_EQ(v.data.size(), 3u);
    std::vector<std::wstring> subs;
    CHECK_EQ(reg::EnumerateSubkeys(k, &subs), nt::kSuccess);
    CHECK_EQ(subs.size(), 7u);
    std::vector<reg::Value> vals;
    CHECK_EQ(reg::EnumerateValues(k, &vals), nt::kSuccess);
    CHECK_EQ(vals.size(), 9u);
    reg::CloseKey(k);
    CHECK_EQ(reg::QueryValue(k, L"String", &v), nt::kInvalidHandle);
}

TEST(registry_nt_query_value_buffer_semantics) {
    Session s(FixtureRoot());
    HANDLE h = Open(std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion");
    CHECK(h != nullptr);
    if (!h) return;
    const std::wstring product = L"Windows 10 Pro";
    const ULONG data_len = static_cast<ULONG>((product.size() + 1) * 2);   // 30
    std::vector<std::uint8_t> b;
    ULONG need = 0;

    // KeyValuePartialInformation: header is 12 bytes (TitleIndex, Type, DataLength, Data[]).
    NTSTATUS st = QueryValue(h, L"ProductName", reg::kValuePartial, 8, &b, &need);
    CHECK_EQ(st, nt::kBufferTooSmall);
    CHECK_EQ(need, 12 + data_len);

    st = QueryValue(h, L"ProductName", reg::kValuePartial, 0, &b, &need);
    CHECK_EQ(st, nt::kBufferTooSmall);
    CHECK_EQ(need, 12 + data_len);

    // header fits, data does not: STATUS_BUFFER_OVERFLOW with the partial data.
    st = QueryValue(h, L"ProductName", reg::kValuePartial, 12 + 10, &b, &need);
    CHECK_EQ(st, nt::kBufferOverflow);
    CHECK_EQ(need, 12 + data_len);
    CHECK_EQ(Get<ULONG>(b, 4), kRegSz);
    CHECK_EQ(Get<ULONG>(b, 8), data_len);
    CHECK(std::memcmp(b.data() + 12, product.c_str(), 10) == 0);

    // exactly sized
    st = QueryValue(h, L"ProductName", reg::kValuePartial, 12 + data_len, &b, &need);
    CHECK_EQ(st, nt::kSuccess);
    CHECK_EQ(need, 12 + data_len);
    CHECK(std::wstring(reinterpret_cast<wchar_t*>(b.data() + 12)) == product);

    // PartialAlign64: Type, DataLength, Data[] at offset 8
    st = QueryValue(h, L"ProductName", reg::kValuePartialAlign64, 8 + data_len, &b, &need);
    CHECK_EQ(st, nt::kSuccess);
    CHECK_EQ(need, 8 + data_len);
    CHECK_EQ(Get<ULONG>(b, 0), kRegSz);
    CHECK(std::wstring(reinterpret_cast<wchar_t*>(b.data() + 8)) == product);

    // Basic: TitleIndex, Type, NameLength, Name[]
    st = QueryValue(h, L"ProductName", reg::kValueBasic, 100, &b, &need);
    CHECK_EQ(st, nt::kSuccess);
    CHECK_EQ(need, 12u + 11u * 2u);
    CHECK_EQ(Get<ULONG>(b, 8), 22u);
    CHECK(WideAt(b, 12, 22) == L"ProductName");
    st = QueryValue(h, L"ProductName", reg::kValueBasic, 11, &b, &need);
    CHECK_EQ(st, nt::kBufferTooSmall);
    st = QueryValue(h, L"ProductName", reg::kValueBasic, 20, &b, &need);
    CHECK_EQ(st, nt::kBufferOverflow);

    // Full: TitleIndex, Type, DataOffset, DataLength, NameLength, Name[], then data at DataOffset
    st = QueryValue(h, L"ProductName", reg::kValueFull, 4096, &b, &need);
    CHECK_EQ(st, nt::kSuccess);
    ULONG doff = Get<ULONG>(b, 8);
    CHECK_EQ(Get<ULONG>(b, 12), data_len);
    CHECK_EQ(Get<ULONG>(b, 16), 22u);
    CHECK_EQ(doff, (20u + 22u + 3u) & ~3u);
    CHECK_EQ(need, doff + data_len);
    CHECK(std::wstring(reinterpret_cast<wchar_t*>(b.data() + doff)) == product);
    CHECK_EQ(QueryValue(h, L"ProductName", reg::kValueFull, 19, &b, &need), nt::kBufferTooSmall);
    CHECK_EQ(QueryValue(h, L"ProductName", reg::kValueFull, 30, &b, &need), nt::kBufferOverflow);

    // Errors
    CHECK_EQ(QueryValue(h, L"Nope", reg::kValuePartial, 100, &b, &need), nt::kObjectNameNotFound);
    CHECK_EQ(QueryValue(h, L"ProductName", 77, 100, &b, &need), nt::kInvalidParameter);
    CHECK_EQ(QueryValue(h, L"ProductName", reg::kValueLayer, 100, &b, &need), nt::kInvalidParameter);

    // DWORD value
    st = QueryValue(h, L"UBR", reg::kValuePartial, 16, &b, &need);
    CHECK_EQ(st, nt::kSuccess);
    CHECK_EQ(Get<ULONG>(b, 4), kRegDword);
    CHECK_EQ(Get<ULONG>(b, 12), 3803u);

    // NtQueryMultipleValueKey
    {
        Us n1(L"UBR"), n2(L"ProductName");
        reg::ValueEntry e[3] = {{&n1.u, 0, 0, 0}, {&n2.u, 0, 0, 0}, {&n1.u, 0, 0, 0}};
        std::vector<std::uint8_t> out(256);
        ULONG len = 256, req = 0;
        CHECK_EQ(reg::Reg_NtQueryMultipleValueKey(h, e, 2, out.data(), &len, &req), nt::kSuccess);
        CHECK_EQ(e[0].Type, kRegDword);
        CHECK_EQ(e[0].DataLength, 4u);
        CHECK_EQ(Get<ULONG>(out, e[0].DataOffset), 3803u);
        CHECK_EQ(e[1].DataOffset % 8, 0u);
        CHECK(std::wstring(reinterpret_cast<wchar_t*>(out.data() + e[1].DataOffset)) == product);
        len = 8;
        CHECK_EQ(reg::Reg_NtQueryMultipleValueKey(h, e, 2, out.data(), &len, &req), nt::kBufferOverflow);
        CHECK_EQ(req, e[1].DataOffset + data_len);
    }

    // access check: a handle without KEY_QUERY_VALUE cannot read values
    HANDLE h0 = Open(std::wstring(kSoftware) + L"\\Fixture", 0x0008 /*KEY_ENUMERATE_SUB_KEYS*/);
    CHECK(h0 != nullptr);
    CHECK_EQ(QueryValue(h0, L"String", reg::kValuePartial, 100, &b, &need), nt::kAccessDenied);
    reg::Close(h0);
    reg::Close(h);
}

TEST(registry_nt_enumerate) {
    Session s(FixtureRoot());
    HANDLE h = Open(std::wstring(kSoftware) + L"\\Fixture");
    CHECK(h != nullptr);
    if (!h) return;

    // subkeys come back sorted case-insensitively, like a hive's lh list
    auto subs = SubkeyNames(h);
    CHECK_EQ(subs.size(), 7u);
    if (subs.size() == 7) {
        CHECK(subs[0] == L"alpha");
        CHECK(subs[1] == L"Bravo");
        CHECK(subs[2] == L"Charlie");
        CHECK(subs[3] == L"Link");
        CHECK(subs[4] == L"Many");
        CHECK(subs[5] == L"WithClass");
        CHECK(subs[6] == L"\u00dcn\u00ef");
    }
    std::vector<std::uint8_t> b(512);
    ULONG rl = 0;
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 7, reg::kKeyBasic, b.data(), 512, &rl), nt::kNoMoreEntries);

    // buffer semantics: KEY_BASIC_INFORMATION = LastWriteTime, TitleIndex, NameLength, Name[]
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, reg::kKeyBasic, b.data(), 15, &rl), nt::kBufferTooSmall);
    CHECK_EQ(rl, 16u + 10u);
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, reg::kKeyBasic, b.data(), 16 + 4, &rl), nt::kBufferOverflow);
    CHECK_EQ(rl, 16u + 10u);
    CHECK_EQ(Get<ULONG>(b, 12), 10u);   // NameLength is always the full length
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, reg::kKeyBasic, b.data(), 26, &rl), nt::kSuccess);
    CHECK(WideAt(b, 16, 10) == L"alpha");
    CHECK(Get<std::uint64_t>(b, 0) != 0);   // LastWriteTime

    // KeyNodeInformation with a class name (WithClass)
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 5, reg::kKeyNode, b.data(), 512, &rl), nt::kSuccess);
    ULONG nlen = Get<ULONG>(b, 20), clen = Get<ULONG>(b, 16), coff = Get<ULONG>(b, 12);
    CHECK_EQ(nlen, 18u);
    CHECK_EQ(clen, 24u);
    CHECK_EQ(rl, 24u + nlen + clen);
    CHECK_EQ(coff, 24u + nlen);
    CHECK(WideAt(b, 24, nlen) == L"WithClass");
    CHECK(WideAt(b, coff, clen) == L"FixtureClass");
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, reg::kKeyNode, b.data(), 512, &rl), nt::kSuccess);
    CHECK_EQ(Get<ULONG>(b, 12), 0xFFFFFFFFu);   // no class: ClassOffset = -1
    CHECK_EQ(Get<ULONG>(b, 16), 0u);

    // KeyNameInformation on enumerate: the subkey's full name (NameLength, Name[])
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 1, reg::kKeyName, b.data(), 512, &rl), nt::kSuccess);
    CHECK(WideAt(b, 4, Get<ULONG>(b, 0)) == L"\\REGISTRY\\MACHINE\\SOFTWARE\\Fixture\\Bravo");
    CHECK_EQ(rl, 4u + Get<ULONG>(b, 0));
    // KeyFullInformation and KeyCachedInformation describe the enumerated subkey (Bravo has no subkeys, class or values)
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 1, reg::kKeyFull, b.data(), 512, &rl), nt::kSuccess);
    CHECK_EQ(rl, 44u);
    CHECK_EQ(Get<ULONG>(b, 20), 0u);   // SubKeys
    CHECK_EQ(Get<ULONG>(b, 32), 0u);   // Values
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 1, reg::kKeyCached, b.data(), 512, &rl), nt::kSuccess);
    CHECK_EQ(rl, 40u);
    CHECK_EQ(Get<ULONG>(b, 32), 10u);  // NameLength: "Bravo"
    // too small for the fixed part is TOO_SMALL with the size needed; the name does not fit is OVERFLOW
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 1, reg::kKeyBasic, b.data(), 15, &rl), nt::kBufferTooSmall);
    CHECK_EQ(rl, 16u + 10u);
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 1, reg::kKeyBasic, b.data(), 16, &rl), nt::kBufferOverflow);
    CHECK_EQ(rl, 16u + 10u);
    // classes NtEnumerateKey does not answer, and unknown ones
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, reg::kKeyFlags, b.data(), 512, &rl), nt::kInvalidInfoClass);
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0, 99, b.data(), 512, &rl), nt::kInvalidInfoClass);
    // past the end, and the index that means "this key" inside Wine's ntdll
    CHECK_EQ(reg::Reg_NtEnumerateKey(h, 0xFFFFFFFFu, reg::kKeyBasic, b.data(), 512, &rl), nt::kNoMoreEntries);

    // values, in stored order; the unnamed value has an empty name
    auto vals = ValueNames(h);
    CHECK_EQ(vals.size(), 9u);
    if (vals.size() == 9) {
        CHECK(vals[0].empty());
        CHECK(vals[1] == L"String");
        CHECK(vals[8] == L"Wide-Name-\u00e9");
    }
    b.assign(256, 0);
    CHECK_EQ(reg::Reg_NtEnumerateValueKey(h, 1, reg::kValueFull, b.data(), 256, &rl), nt::kSuccess);
    CHECK_EQ(Get<ULONG>(b, 4), kRegSz);
    CHECK_EQ(Get<ULONG>(b, 12), 12u);   // "hello\0"
    CHECK(std::wstring(reinterpret_cast<wchar_t*>(b.data() + Get<ULONG>(b, 8))) == L"hello");
    CHECK_EQ(reg::Reg_NtEnumerateValueKey(h, 9, reg::kValueBasic, b.data(), 256, &rl), nt::kNoMoreEntries);
    CHECK_EQ(reg::Reg_NtEnumerateValueKey(h, 1, reg::kValueBasic, b.data(), 5, &rl), nt::kBufferTooSmall);

    // NtQueryKey
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyFull, b.data(), 512, &rl), nt::kSuccess);
    CHECK_EQ(rl, 44u);
    CHECK_EQ(Get<ULONG>(b, 20), 7u);    // SubKeys
    CHECK_EQ(Get<ULONG>(b, 32), 9u);    // Values
    CHECK_EQ(Get<ULONG>(b, 24), 18u);   // MaxNameLen: "WithClass" = 9 chars
    CHECK_EQ(Get<ULONG>(b, 28), 24u);   // MaxClassLen
    CHECK_EQ(Get<ULONG>(b, 40), 40000u);   // MaxValueDataLen
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyFull, b.data(), 43, &rl), nt::kBufferTooSmall);
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyName, b.data(), 512, &rl), nt::kSuccess);
    CHECK(WideAt(b, 4, Get<ULONG>(b, 0)) == L"\\REGISTRY\\MACHINE\\SOFTWARE\\Fixture");
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyName, b.data(), 4 + 6, &rl), nt::kBufferOverflow);
    CHECK_EQ(rl, 4u + 2u * 34u);
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyName, b.data(), 3, &rl), nt::kBufferTooSmall);
    reg::Close(h);

    // 700 subkeys (ri of several lh lists)
    HANDLE m = Open(std::wstring(kSoftware) + L"\\Fixture\\Many");
    CHECK(m != nullptr);
    CHECK_EQ(reg::Reg_NtEnumerateKey(m, 699, reg::kKeyBasic, b.data(), 512, &rl), nt::kSuccess);
    CHECK(WideAt(b, 16, Get<ULONG>(b, 12)) == L"K0699");
    CHECK_EQ(reg::Reg_NtEnumerateKey(m, 700, reg::kKeyBasic, b.data(), 512, &rl), nt::kNoMoreEntries);
    CHECK(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Many\\K0512") == nt::kSuccess);
    reg::Close(m);

    // the root containers are enumerable too
    HANDLE mc = Open(L"\\Registry\\Machine");
    CHECK(mc != nullptr);
    auto top = SubkeyNames(mc);
    CHECK(Has(top, L"SOFTWARE"));
    CHECK(Has(top, L"SYSTEM"));
    reg::Close(mc);
}

TEST(registry_open_semantics) {
    Session s(FixtureRoot());
    CHECK_EQ(OpenStatus(L"\\Registry\\Machine\\SOFTWARE\\Nope\\Deeper"), nt::kObjectNameNotFound);
    CHECK_EQ(OpenStatus(L"Registry\\Machine"), nt::kObjectPathSyntaxBad);
    CHECK_EQ(OpenStatus(L"\\Registry\\Machine\\"), nt::kSuccess);
    CHECK_EQ(OpenStatus(L"\\Device\\Foo"), nt::kObjectNameNotFound);

    // relative open from a key handle
    HANDLE fx = Open(std::wstring(kSoftware) + L"\\Fixture");
    HANDLE rel = Open(L"WithClass", kRead, fx);
    CHECK(rel != nullptr);
    CHECK_EQ(OpenStatus(L"\\abs", kRead), nt::kObjectNameNotFound);
    {
        Oa bad(L"\\Registry", fx);
        HANDLE h = nullptr;
        CHECK_EQ(reg::Reg_NtOpenKey(&h, kRead, &bad.a), nt::kObjectPathSyntaxBad);
    }
    reg::Close(rel);
    reg::Close(fx);

    // symbolic link following; REG_OPTION_OPEN_LINK opens the link key itself
    std::wstring str;
    CHECK_EQ(reg::QueryString(std::wstring(kSoftware) + L"\\Fixture\\Link", L"ProductName", &str), nt::kSuccess);
    CHECK(str == L"Windows 10 Pro");
    {
        Oa oa(std::wstring(kSoftware) + L"\\Fixture\\Link");
        HANDLE h = nullptr;
        // REG_OPTION_OPEN_LINK does not matter to NtOpenKeyEx: the link is followed
        CHECK_EQ(reg::Reg_NtOpenKeyEx(&h, kRead, &oa.a, 8), nt::kSuccess);
        std::vector<std::uint8_t> b;
        ULONG need = 0;
        CHECK_EQ(QueryValue(h, L"SymbolicLinkValue", reg::kValuePartial, 512, &b, &need), nt::kObjectNameNotFound);
        reg::Close(h);
        // OBJ_OPENLINK opens the link key itself
        Oa link_oa(std::wstring(kSoftware) + L"\\Fixture\\Link", nullptr, 0x40 | 0x100);
        CHECK_EQ(reg::Reg_NtOpenKey(&h, kRead, &link_oa.a), nt::kSuccess);
        CHECK_EQ(QueryValue(h, L"SymbolicLinkValue", reg::kValuePartial, 512, &b, &need), nt::kSuccess);
        CHECK_EQ(Get<ULONG>(b, 4), 6u);   // REG_LINK
        reg::Close(h);
    }

    // handle encoding
    HANDLE h = Open(std::wstring(kSoftware));
    CHECK(h != nullptr);
    CHECK(reg::IsKeyHandle(h));
    reg::KeyId id = reg::FromHandle(h);
    CHECK(id != 0);
    CHECK(reg::ToHandle(id) == h);
    CHECK(!reg::IsKeyHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x44))));
    CHECK(!reg::IsKeyHandle(nullptr));
    CHECK_EQ(reg::Close(h), nt::kSuccess);
    CHECK(!reg::IsKeyHandle(h));
    CHECK_EQ(reg::Close(h), nt::kInvalidHandle);
    std::vector<std::uint8_t> b(64);
    ULONG rl = 0;
    CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyBasic, b.data(), 64, &rl), nt::kInvalidHandle);

    // generic access mapping: GENERIC_READ handle may query but not set
    HANDLE r = Open(std::wstring(kSoftware) + L"\\Fixture", 0x80000000u);
    CHECK(r != nullptr);
    CHECK_EQ(SetDword(r, L"X", 1), nt::kAccessDenied);
    reg::Close(r);
}

TEST(registry_overlay_semantics_and_hive_untouched) {
    const std::wstring root = FixtureRoot();
    const std::wstring files[3] = {root + L"\\Windows\\System32\\config\\SOFTWARE", root + L"\\Windows\\System32\\config\\SYSTEM",
                                   root + L"\\Users\\User\\NTUSER.DAT"};
    std::uint64_t before[3], sizes_before[3];
    for (int i = 0; i < 3; ++i) { before[i] = FileHash(files[i], &sizes_before[i]); CHECK(before[i] != 0); }

    {
        Session s(root);
        const std::wstring cv = std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion";
        HANDLE k = Open(cv, kAll);
        CHECK(k != nullptr);
        if (!k) return;
        const size_t nvals_before = ValueNames(k).size();

        // set a new value, read it back (three ways)
        CHECK_EQ(SetDword(k, L"OverlayValue", 0x1234), nt::kSuccess);
        std::uint32_t dw = 0;
        CHECK_EQ(reg::QueryDword(cv, L"OverlayValue", &dw), nt::kSuccess);
        CHECK_EQ(dw, 0x1234u);
        std::vector<std::uint8_t> b;
        ULONG need = 0;
        CHECK_EQ(QueryValue(k, L"overlayvalue", reg::kValuePartial, 64, &b, &need), nt::kSuccess);
        CHECK_EQ(Get<ULONG>(b, 12), 0x1234u);
        auto names = ValueNames(k);
        CHECK_EQ(names.size(), nvals_before + 1);
        CHECK(names.back() == L"OverlayValue");   // new values enumerate after the hive's

        // overwrite a hive value in place: keeps its position and stored name case
        CHECK_EQ(SetString(k, L"productname", L"Windows 10 Home"), nt::kSuccess);
        std::wstring str;
        CHECK_EQ(reg::QueryString(cv, L"ProductName", &str), nt::kSuccess);
        CHECK(str == L"Windows 10 Home");
        auto names2 = ValueNames(k);
        CHECK_EQ(names2.size(), nvals_before + 1);
        CHECK(names2[0] == L"ProductName");

        // delete a hive value and an overlay value
        {
            Us n(L"CurrentBuild");
            CHECK_EQ(reg::Reg_NtDeleteValueKey(k, &n.u), nt::kSuccess);
            CHECK_EQ(reg::Reg_NtDeleteValueKey(k, &n.u), nt::kObjectNameNotFound);
        }
        CHECK_EQ(reg::QueryString(cv, L"CurrentBuild", &str), nt::kObjectNameNotFound);
        CHECK(!Has(ValueNames(k), L"CurrentBuild"));
        CHECK_EQ(QueryValue(k, L"CurrentBuild", reg::kValueBasic, 64, &b, &need), nt::kObjectNameNotFound);
        {
            Us n(L"OverlayValue");
            CHECK_EQ(reg::Reg_NtDeleteValueKey(k, &n.u), nt::kSuccess);
        }
        CHECK_EQ(reg::QueryDword(cv, L"OverlayValue", &dw), nt::kObjectNameNotFound);
        // re-setting a deleted hive value brings it back (with the new data)
        CHECK_EQ(SetString(k, L"CurrentBuild", L"12345"), nt::kSuccess);
        CHECK_EQ(reg::QueryString(cv, L"CurrentBuild", &str), nt::kSuccess);
        CHECK(str == L"12345");
        reg::Close(k);

        // create keys, dispositions, enumeration
        HANDLE fx = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
        auto before_subs = SubkeyNames(fx);
        HANDLE c = nullptr;
        ULONG disp = 0;
        CHECK_EQ(CreateDeep(std::wstring(kSoftware) + L"\\Fixture\\Created\\Deep", &c, &disp), nt::kSuccess);
        CHECK_EQ(disp, 1ul);
        CHECK(c != nullptr);
        CHECK_EQ(SetDword(c, L"Leaf", 5), nt::kSuccess);
        HANDLE c2 = nullptr;
        CHECK_EQ(Create(std::wstring(kSoftware) + L"\\Fixture\\created\\DEEP", &c2, &disp), nt::kSuccess);
        CHECK_EQ(disp, 2ul);
        reg::Close(c2);
        reg::Close(c);
        auto after_subs = SubkeyNames(fx);
        CHECK_EQ(after_subs.size(), before_subs.size() + 1);
        if (after_subs.size() == 8) CHECK(after_subs[3] == L"Created");   // sorted between Charlie and Link
        HANDLE cr = Open(std::wstring(kSoftware) + L"\\Fixture\\Created");
        auto inner = SubkeyNames(cr);
        CHECK_EQ(inner.size(), 1u);
        if (inner.size() == 1) CHECK(inner[0] == L"Deep");
        CHECK_EQ(reg::QueryDword(std::wstring(kSoftware) + L"\\Fixture\\Created\\Deep", L"Leaf", &dw), nt::kSuccess);
        CHECK_EQ(dw, 5u);
        // volatile/class options
        {
            Oa oa(std::wstring(kSoftware) + L"\\Fixture\\Classy");
            Us cls(L"MyClass");
            HANDLE h = nullptr;
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &oa.a, 0, &cls.u, 1 /*volatile*/, &disp), nt::kSuccess);
            std::vector<std::uint8_t> kb(256);
            ULONG rl = 0;
            CHECK_EQ(reg::Reg_NtQueryKey(h, reg::kKeyFull, kb.data(), 256, &rl), nt::kSuccess);
            CHECK_EQ(Get<ULONG>(kb, 16), 14u);
            CHECK(WideAt(kb, 44, 14) == L"MyClass");
            reg::Close(h);
        }
        // the access of the parent handle does not decide a create (the parent's ACL does); containers are denied
        {
            HANDLE ro = Open(std::wstring(kSoftware) + L"\\Fixture", kRead);
            Oa oa(L"ReadOnlyParent", ro);
            HANDLE h = nullptr;
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &oa.a, 0, nullptr, 0, nullptr), nt::kSuccess);
            reg::Close(h);
            reg::Close(ro);
            CHECK_EQ(Create(L"\\Registry\\Machine\\NotAHive", &h), nt::kAccessDenied);
        }
        reg::Close(cr);

        // delete a key that lives in the hive; deleting one with children fails
        HANDLE bravo = Open(std::wstring(kSoftware) + L"\\Fixture\\Bravo", kAll);
        CHECK(bravo != nullptr);
        CHECK_EQ(reg::Reg_NtDeleteKey(bravo), nt::kSuccess);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Bravo"), nt::kObjectNameNotFound);
        CHECK(!Has(SubkeyNames(fx), L"Bravo"));
        std::vector<std::uint8_t> kb(64);
        ULONG rl = 0;
        CHECK_EQ(reg::Reg_NtQueryKey(bravo, reg::kKeyBasic, kb.data(), 64, &rl), nt::kKeyDeleted);
        CHECK_EQ(SetDword(bravo, L"X", 1), nt::kKeyDeleted);
        reg::Close(bravo);
        HANDLE many = Open(std::wstring(kSoftware) + L"\\Fixture\\Many", kAll);
        CHECK_EQ(reg::Reg_NtDeleteKey(many), nt::kCannotDelete);
        reg::Close(many);
        HANDLE hroot = Open(std::wstring(kSoftware), kAll);
        CHECK_EQ(reg::Reg_NtDeleteKey(hroot), nt::kCannotDelete);   // hive roots cannot be deleted
        reg::Close(hroot);

        // re-creating a deleted hive key yields a fresh, empty key
        HANDLE nb = nullptr;
        CHECK_EQ(Create(std::wstring(kSoftware) + L"\\Fixture\\Bravo", &nb, &disp), nt::kSuccess);
        CHECK_EQ(disp, 1ul);
        CHECK_EQ(SubkeyNames(nb).size(), 0u);
        CHECK_EQ(ValueNames(nb).size(), 0u);
        reg::Close(nb);
        CHECK(Has(SubkeyNames(fx), L"Bravo"));

        // rename a key with a subtree
        HANDLE ren = Open(std::wstring(kSoftware) + L"\\Fixture\\Created", kAll);
        Us newname(L"Renamed");
        CHECK_EQ(reg::Reg_NtRenameKey(ren, &newname.u), nt::kSuccess);
        reg::Close(ren);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Created"), nt::kObjectNameNotFound);
        CHECK_EQ(reg::QueryDword(std::wstring(kSoftware) + L"\\Fixture\\Renamed\\Deep", L"Leaf", &dw), nt::kSuccess);
        reg::Close(fx);

        // set on the root of the namespace / containers is denied
        HANDLE mach = Open(L"\\Registry\\Machine", kAll);
        CHECK_EQ(SetDword(mach, L"X", 1), nt::kAccessDenied);
        reg::Close(mach);
    }

    // The supplied hive files are byte-for-byte unchanged.
    for (int i = 0; i < 3; ++i) {
        std::uint64_t sz = 0;
        CHECK_EQ(FileHash(files[i], &sz), before[i]);
        CHECK_EQ(sz, sizes_before[i]);
    }

    // And a fresh session sees none of the overlay.
    Session s2(root);
    std::wstring str;
    CHECK_EQ(reg::QueryString(std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", &str), nt::kSuccess);
    CHECK(str == L"Windows 10 Pro");
    CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Renamed"), nt::kObjectNameNotFound);
    CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Bravo"), nt::kSuccess);
}

TEST(registry_overlay_persistence) {
    const std::wstring dir = TempDir(L"persist");
    CopyFixture(dir);
    const std::wstring cv = std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion";
    {
        Session s(dir);
        CHECK_EQ(s.init, nt::kSuccess);
        HANDLE k = Open(cv, kAll);
        CHECK_EQ(SetString(k, L"ProductName", L"Persisted"), nt::kSuccess);
        Us gone(L"CurrentBuild");
        CHECK_EQ(reg::Reg_NtDeleteValueKey(k, &gone.u), nt::kSuccess);
        reg::Close(k);
        HANDLE c = nullptr;
        CHECK_EQ(CreateDeep(std::wstring(kSoftware) + L"\\Fixture\\NewKey\\Sub", &c), nt::kSuccess);
        CHECK_EQ(SetDword(c, L"D", 77), nt::kSuccess);
        reg::Close(c);
        HANDLE b = Open(std::wstring(kSoftware) + L"\\Fixture\\Charlie", kAll);
        CHECK_EQ(reg::Reg_NtDeleteKey(b), nt::kSuccess);
        reg::Close(b);
        HANDLE wc = Open(std::wstring(kSoftware) + L"\\Fixture\\WithClass", kAll);   // delete + recreate => fresh
        CHECK_EQ(reg::Reg_NtDeleteKey(wc), nt::kSuccess);
        reg::Close(wc);
        HANDLE nwc = nullptr;
        ULONG disp = 0;
        CHECK_EQ(Create(std::wstring(kSoftware) + L"\\Fixture\\WithClass", &nwc, &disp), nt::kSuccess);
        CHECK_EQ(SetDword(nwc, L"Again", 1), nt::kSuccess);
        reg::Close(nwc);
        CHECK_EQ(reg::SaveOverlay(), nt::kSuccess);
    }
    {
        Session s(dir);
        std::wstring str;
        CHECK_EQ(reg::QueryString(cv, L"ProductName", &str), nt::kSuccess);
        CHECK(str == L"Windows 10 Pro");   // not auto-loaded
        CHECK_EQ(reg::LoadOverlay(), nt::kSuccess);
        CHECK_EQ(reg::QueryString(cv, L"ProductName", &str), nt::kSuccess);
        CHECK(str == L"Persisted");
        CHECK_EQ(reg::QueryString(cv, L"CurrentBuild", &str), nt::kObjectNameNotFound);
        std::uint32_t dw = 0;
        CHECK_EQ(reg::QueryDword(std::wstring(kSoftware) + L"\\Fixture\\NewKey\\Sub", L"D", &dw), nt::kSuccess);
        CHECK_EQ(dw, 77u);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture\\Charlie"), nt::kObjectNameNotFound);
        reg::KeyId k = 0;
        CHECK_EQ(reg::OpenKey(std::wstring(kSoftware) + L"\\Fixture\\WithClass", &k), nt::kSuccess);
        std::vector<reg::Value> vals;
        reg::EnumerateValues(k, &vals);
        CHECK_EQ(vals.size(), 1u);
        reg::CloseKey(k);
        // hive on disk untouched by SaveOverlay
        CHECK_EQ(FileHash(dir + L"\\Windows\\System32\\config\\SOFTWARE"), FileHash(FixtureRoot() + L"\\Windows\\System32\\config\\SOFTWARE"));
    }
    RemoveTree(dir);
}

TEST(registry_missing_hives_use_machine_json) {
    const std::wstring dir = TempDir(L"nohive");
    {
        std::string json = R"({"windows":{"major":10,"minor":0,"build":12345,"ubr":42,"product_name":"Windows 10 Test","display_version":"99H9","csd":""},
                               "identity":{"computer_name":"TESTPC","user_name":"Tester","user_sid":"S-1-5-21-1-2-3-1000"}})";
        HANDLE f = CreateFileW((dir + L"\\machine.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w;
        WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &w, nullptr);
        CloseHandle(f);
    }
    {
        Session s(dir);
        CHECK_EQ(s.init, nt::kSuccess);
        const std::wstring cv = std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion";
        std::wstring str;
        std::uint32_t dw = 0;
        CHECK_EQ(reg::QueryString(cv, L"ProductName", &str), nt::kSuccess);
        CHECK(str == L"Windows 10 Test");
        CHECK_EQ(reg::QueryString(cv, L"CurrentBuild", &str), nt::kSuccess);
        CHECK(str == L"12345");
        CHECK_EQ(reg::QueryString(cv, L"CurrentBuildNumber", &str), nt::kSuccess);
        CHECK(str == L"12345");
        CHECK_EQ(reg::QueryDword(cv, L"UBR", &dw), nt::kSuccess);
        CHECK_EQ(dw, 42u);
        CHECK_EQ(reg::QueryDword(cv, L"CurrentMajorVersionNumber", &dw), nt::kSuccess);
        CHECK_EQ(dw, 10u);
        CHECK_EQ(reg::QueryString(cv, L"DisplayVersion", &str), nt::kSuccess);
        CHECK(str == L"99H9");
        CHECK_EQ(reg::QueryString(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName", L"ComputerName", &str), nt::kSuccess);
        CHECK(str == L"TESTPC");
        CHECK_EQ(reg::QueryDword(L"\\Registry\\Machine\\SYSTEM\\Select", L"Current", &dw), nt::kSuccess);
        CHECK_EQ(dw, 1u);
        // the identity's SID names the user hive; empty hive is writable
        HANDLE c = nullptr;
        CHECK_EQ(CreateDeep(L"\\Registry\\User\\S-1-5-21-1-2-3-1000\\Software\\Test", &c), nt::kSuccess);
        reg::Close(c);
        CHECK_EQ(OpenStatus(L"\\Registry\\User\\.DEFAULT"), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Registry\\User\\S-1-5-21-1-2-3-1000_Classes"), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"), nt::kSuccess);
    }
    RemoveTree(dir);
}

// HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 (what fastfetch's CPU module reads): the identity's brand and
// family/model/stepping (D18), the host's own base clock, never a constant.
// The status codes ntdll's reg test pins for names, creation, links and deleted keys (Wine dlls/ntdll/tests/reg.c).
TEST(registry_nt_name_create_link_statuses) {
    const std::wstring dir = TempDir(L"statuses");
    {
        Session s(dir);
        CHECK_EQ(s.init, nt::kSuccess);
        const std::wstring hkcu = std::wstring(L"\\Registry\\User\\") + kUserSid;
        HANDLE h = nullptr;

        // names
        CHECK_EQ(OpenStatus(L""), nt::kObjectPathSyntaxBad);
        CHECK_EQ(OpenStatus(L"Machine"), nt::kObjectPathSyntaxBad);
        CHECK_EQ(OpenStatus(L"\\"), nt::kObjectTypeMismatch);
        CHECK_EQ(OpenStatus(L"\\\\\\"), nt::kObjectNameInvalid);
        CHECK_EQ(OpenStatus(L"\\Registry"), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Registry\\"), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Registry\\\\"), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Foobar"), nt::kObjectNameNotFound);
        CHECK_EQ(OpenStatus(L"\\Foobar\\Machine"), static_cast<NTSTATUS>(0xC000003A));   // OBJECT_PATH_NOT_FOUND
        CHECK_EQ(OpenStatus(L"\\Machine\\Software\\Classes"), static_cast<NTSTATUS>(0xC000003A));
        CHECK_EQ(OpenStatus(L"\\Device\\Null"), nt::kObjectTypeMismatch);   // exists in the namespace, is no key
        {
            Oa bad(hkcu);
            bad.a.Length *= 2;
            CHECK_EQ(reg::Reg_NtOpenKey(&h, kRead, &bad.a), nt::kInvalidParameter);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &bad.a, 0, nullptr, 0, nullptr), nt::kInvalidParameter);
        }

        // only the last component of a name is created; a missing key on the way is NAME_NOT_FOUND
        HANDLE user = Open(hkcu, kAll);
        CHECK(user != nullptr);
        {
            Oa deep(L"a\\b\\c", user);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &deep.a, 0, nullptr, 0, nullptr), nt::kObjectNameNotFound);
            Oa two(L"a\\b", user);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &two.a, 0, nullptr, 0, nullptr), nt::kObjectNameNotFound);
            CHECK_EQ(OpenStatus(hkcu + L"\\a"), nt::kObjectNameNotFound);
            Oa trailing(L"made\\", user);
            ULONG disp = 0;
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &trailing.a, 0, nullptr, 0, &disp), nt::kSuccess);
            CHECK_EQ(disp, 1ul);
            reg::Close(h);
            Oa nested(L"made\\\\child\\\\", user);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &nested.a, 0, nullptr, 0, &disp), nt::kSuccess);
            reg::Close(h);
            HANDLE made = Open(hkcu + L"\\made", kAll);
            CHECK_EQ(reg::Reg_NtDeleteKey(made), nt::kCannotDelete);   // it has a subkey
            reg::Close(made);
        }
        // the access of the parent handle does not gate a create
        {
            HANDLE ro = Open(hkcu, kRead);
            Oa oa(L"viaread", ro);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &oa.a, 0, nullptr, 0, nullptr), nt::kSuccess);
            reg::Close(h);
            reg::Close(ro);
        }
        // a key of a volatile tree cannot be stable
        CHECK_EQ(Create(L"\\Registry\\Machine\\HARDWARE\\stable", &h), static_cast<NTSTATUS>(0xC0000181));
        {
            Oa oa(L"\\Registry\\Machine\\HARDWARE\\volatile");
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &oa.a, 0, nullptr, 1 /*volatile*/, nullptr), nt::kSuccess);
            reg::Close(h);
        }

        // symbolic links
        HANDLE link = nullptr;
        {
            Oa oa(L"link", user);
            CHECK_EQ(reg::Reg_NtCreateKey(&link, kAll, &oa.a, 0, nullptr, 2 /*REG_OPTION_CREATE_LINK*/, nullptr), nt::kSuccess);
            Oa again(L"link", user);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &again.a, 0, nullptr, 2, nullptr), nt::kObjectNameCollision);
        }
        const std::wstring target = hkcu + L"\\target";
        const ULONG tbytes = static_cast<ULONG>(target.size() * 2);
        Us linkval(L"SymbolicLinkValue"), other(L"other");
        CHECK_EQ(reg::Reg_NtSetValueKey(link, &linkval.u, 0, kRegSz, const_cast<wchar_t*>(target.c_str()), tbytes + 2), nt::kAccessDenied);
        CHECK_EQ(reg::Reg_NtSetValueKey(link, &other.u, 0, 6, const_cast<wchar_t*>(target.c_str()), tbytes), nt::kAccessDenied);
        CHECK_EQ(reg::Reg_NtSetValueKey(link, &linkval.u, 0, 6, const_cast<wchar_t*>(target.c_str()), tbytes), nt::kSuccess);
        CHECK_EQ(OpenStatus(hkcu + L"\\link"), nt::kObjectNameNotFound);   // no target yet
        // creating through a link whose target is missing creates the target
        {
            Oa oa(L"link", user);
            ULONG disp = 0;
            HANDLE t = nullptr;
            CHECK_EQ(reg::Reg_NtCreateKey(&t, kAll, &oa.a, 0, nullptr, 0, &disp), nt::kSuccess);
            CHECK_EQ(disp, 1ul);
            CHECK_EQ(SetDword(t, L"V", 7), nt::kSuccess);
            reg::Close(t);
            std::uint32_t dw = 0;
            CHECK_EQ(reg::QueryDword(hkcu + L"\\target", L"V", &dw), nt::kSuccess);
            CHECK_EQ(OpenStatus(hkcu + L"\\link"), nt::kSuccess);
        }
        // a target with a terminator, a relative target and a loop do not resolve
        {
            std::wstring with_nul = target;
            with_nul.push_back(L'\0');
            CHECK_EQ(reg::Reg_NtSetValueKey(link, &linkval.u, 0, 6, const_cast<wchar_t*>(with_nul.c_str()), tbytes + 2), nt::kSuccess);
            CHECK_EQ(OpenStatus(hkcu + L"\\link"), nt::kObjectNameNotFound);
            std::wstring rel = L"target";
            CHECK_EQ(reg::Reg_NtSetValueKey(link, &linkval.u, 0, 6, const_cast<wchar_t*>(rel.c_str()), 12), nt::kSuccess);
            CHECK_EQ(OpenStatus(hkcu + L"\\link"), nt::kObjectNameInvalid);
            std::wstring loop = hkcu + L"\\link\\x";
            CHECK_EQ(reg::Reg_NtSetValueKey(link, &linkval.u, 0, 6, const_cast<wchar_t*>(loop.c_str()), static_cast<ULONG>(loop.size() * 2)), nt::kSuccess);
            CHECK_EQ(OpenStatus(hkcu + L"\\link\\x"), nt::kInvalidParameter);
        }
        CHECK_EQ(reg::Reg_NtDeleteKey(link), nt::kSuccess);
        reg::Close(link);

        // a deleted key: every use says KEY_DELETED, a second delete is a success, the object still answers
        HANDLE gone = nullptr;
        CHECK_EQ(Create(hkcu + L"\\gone", &gone), nt::kSuccess);
        CHECK_EQ(reg::Reg_NtDeleteKey(gone), nt::kSuccess);
        CHECK_EQ(reg::Reg_NtDeleteKey(gone), nt::kSuccess);
        std::vector<std::uint8_t> b(512);
        ULONG rl = 0;
        CHECK_EQ(reg::Reg_NtQueryKey(gone, reg::kKeyName, b.data(), 512, &rl), nt::kKeyDeleted);
        CHECK_EQ(reg::Reg_NtEnumerateKey(gone, 0, reg::kKeyFull, b.data(), 512, &rl), nt::kKeyDeleted);
        CHECK_EQ(reg::Reg_NtQueryObject(gone, 1 /*ObjectNameInformation*/, b.data(), 512, &rl), nt::kKeyDeleted);
        CHECK_EQ(reg::Reg_NtQueryObject(gone, 0 /*ObjectBasicInformation*/, b.data(), 56, &rl), nt::kSuccess);
        CHECK_EQ(rl, 56u);
        CHECK_EQ(Get<ULONG>(b, 4), static_cast<ULONG>(kAll & ~0u));   // GrantedAccess
        {
            Oa sub(L"sub", gone);
            CHECK_EQ(reg::Reg_NtOpenKey(&h, kRead, &sub.a), nt::kKeyDeleted);
            CHECK_EQ(reg::Reg_NtCreateKey(&h, kAll, &sub.a, 0, nullptr, 0, nullptr), nt::kKeyDeleted);
        }
        reg::Close(gone);

        // NtQueryObject on a live key: the name and the sizes
        CHECK_EQ(reg::Reg_NtQueryObject(user, 1, b.data(), 512, &rl), nt::kSuccess);
        const auto* us = reinterpret_cast<const UNICODE_STRING*>(b.data());
        CHECK(std::wstring(us->Buffer, us->Length / 2) == std::wstring(L"\\REGISTRY\\USER\\") + kUserSid);
        CHECK_EQ(rl, static_cast<ULONG>(sizeof(UNICODE_STRING)) + us->MaximumLength);
        CHECK_EQ(reg::Reg_NtQueryObject(user, 1, b.data(), 8, &rl), nt::kInfoLengthMismatch);
        CHECK_EQ(reg::Reg_NtQueryObject(user, 2 /*ObjectTypeInformation*/, b.data(), 512, &rl), nt::kNotImplemented);

        // rename: the name is checked before the handle, the name must be free
        Us empty(L"");
        CHECK_EQ(reg::Reg_NtRenameKey(nullptr, &empty.u), nt::kInvalidParameter);
        Us anyname(L"renamed");
        CHECK_EQ(reg::Reg_NtRenameKey(nullptr, &anyname.u), nt::kInvalidHandle);
        HANDLE r1 = nullptr, r2 = nullptr;
        CHECK_EQ(Create(hkcu + L"\\r1", &r1), nt::kSuccess);
        CHECK_EQ(Create(hkcu + L"\\r2", &r2), nt::kSuccess);
        Us r2name(L"R2"), same(L"r1");
        CHECK_EQ(reg::Reg_NtRenameKey(r1, &r2name.u), nt::kCannotDelete);   // a sibling has the name
        CHECK_EQ(reg::Reg_NtRenameKey(r1, &same.u), nt::kCannotDelete);     // so does the key itself
        reg::Close(r1);
        reg::Close(r2);
        HANDLE readonly = Open(hkcu + L"\\r1", kRead);
        CHECK_EQ(reg::Reg_NtRenameKey(readonly, &anyname.u), nt::kAccessDenied);
        reg::Close(readonly);
        reg::Close(user);

        // the value information classes keep Windows' numbering: FullAlign64 = 3, PartialAlign64 = 4
        HANDLE cv = Open(std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion");
        CHECK(cv != nullptr);
        CHECK_EQ(QueryValue(cv, L"UBR", 4, 64, &b, &rl), nt::kSuccess);
        CHECK_EQ(rl, 12u);   // Type, DataLength, Data[4]
        CHECK_EQ(Get<ULONG>(b, 0), kRegDword);
        CHECK_EQ(QueryValue(cv, L"UBR", 3, 64, &b, &rl), nt::kSuccess);
        CHECK_EQ(Get<ULONG>(b, 0), kRegDword);
        CHECK_EQ(Get<ULONG>(b, 4), 4u);       // DataLength
        CHECK_EQ(Get<ULONG>(b, 8), 6u);       // NameLength: "UBR"
        CHECK_EQ(QueryValue(cv, L"UBR", 5, 64, &b, &rl), nt::kInvalidParameter);   // KeyValueLayerInformation
        reg::Close(cv);

        // KeyCachedInformation has no name: 40 bytes, SubKeys at 12, NameLength (the leaf's) at 32
        HANDLE sw = Open(std::wstring(kSoftware) + L"\\Classes");
        CHECK(sw != nullptr);
        CHECK_EQ(reg::Reg_NtQueryKey(sw, reg::kKeyCached, b.data(), 40, &rl), nt::kSuccess);
        CHECK_EQ(rl, 40u);
        CHECK_EQ(Get<ULONG>(b, 32), 14u);   // "Classes"
        CHECK(Get<ULONG>(b, 12) >= 4u);     // CLSID, Interface, TypeLib, Wow6432Node ...

        // a SOFTWARE hive that is not supplied still has the tree every Windows has: Classes with its registration
        // keys, the Wow6432Node view and the link from Wow6432Node\Classes to Classes\Wow6432Node
        reg::Close(sw);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Classes\\Interface\\{00000000-0000-0000-C000-000000000046}"), nt::kSuccess);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Classes\\Wow6432Node\\CLSID"), nt::kSuccess);
        Oa viewlink(std::wstring(kSoftware) + L"\\Wow6432Node\\Classes", nullptr, 0x40 | 0x100);
        CHECK_EQ(reg::Reg_NtOpenKey(&h, kRead, &viewlink.a), nt::kSuccess);
        CHECK_EQ(QueryValue(h, L"SymbolicLinkValue", reg::kValuePartial, 512, &b, &rl), nt::kSuccess);
        reg::Close(h);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Wow6432Node\\Classes\\Interface"), nt::kSuccess);   // through the link
        {
            HANDLE ifc = Open(std::wstring(kSoftware) + L"\\Classes\\Interface", kAll);
            CHECK_EQ(reg::Reg_NtDeleteKey(ifc), nt::kCannotDelete);   // never an empty key
            reg::Close(ifc);
        }
    }
    RemoveTree(dir);
}

// A handle's notification keeps the filter and the tree flag of its first request; a later request only adds its
// event, and an outstanding request resets its event.
TEST(registry_notify_requests_accumulate_per_handle) {
    Session s(FixtureRoot());
    HANDLE k1 = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
    HANDLE k2 = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
    HANDLE ev[4];
    for (auto& e : ev) e = CreateEventW(nullptr, TRUE, TRUE, nullptr);   // signalled: a request must reset them
    IO_STATUS_BLOCK iosb{};
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k1, ev[0], nullptr, nullptr, &iosb, 1 /*NAME*/, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k1, ev[1], nullptr, nullptr, &iosb, 0, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k2, ev[2], nullptr, nullptr, &iosb, 0, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k2, ev[3], nullptr, nullptr, &iosb, 1, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK(WaitForMultipleObjects(4, ev, FALSE, 0) == WAIT_TIMEOUT);
    // a name change fires k1's two requests (filter NAME, from the first); k2's filter is 0 and never matches
    HANDLE c = nullptr;
    Oa oa(L"NotifyChild", k1);
    CHECK_EQ(reg::Reg_NtCreateKey(&c, kAll, &oa.a, 0, nullptr, 0, nullptr), nt::kSuccess);
    CHECK(WaitForSingleObject(ev[0], 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev[1], 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev[2], 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(ev[3], 0) == WAIT_TIMEOUT);
    // the handle keeps its filter: a new request with another filter is still a NAME request
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k1, ev[0], nullptr, nullptr, &iosb, 0, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK(WaitForSingleObject(ev[0], 0) == WAIT_TIMEOUT);
    CHECK_EQ(reg::Reg_NtDeleteKey(c), nt::kSuccess);
    CHECK(WaitForSingleObject(ev[0], 0) == WAIT_OBJECT_0);
    reg::Close(c);
    reg::Close(k1);
    reg::Close(k2);
    for (auto e : ev) CloseHandle(e);
}

TEST(registry_hardware_cpu_identity) {
    Session s(FixtureRoot());
    CHECK_EQ(s.init, nt::kSuccess);
    const std::wstring cpu0 = L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    std::wstring str;
    std::uint32_t dw = 0;
    CHECK_EQ(reg::QueryString(cpu0, L"ProcessorNameString", &str), nt::kSuccess);
    CHECK(str == win10xbox::config::Current().machine_identity.cpu_brand);
    CHECK(!str.empty());
    CHECK_EQ(reg::QueryString(cpu0, L"Identifier", &str), nt::kSuccess);
    CHECK(str == L"AMD64 Family 23 Model 136 Stepping 1");
    CHECK_EQ(reg::QueryString(cpu0, L"VendorIdentifier", &str), nt::kSuccess);
    CHECK(!str.empty());

    // ~MHz is the host's: read the same value from the host registry here and compare.
    DWORD host = 0, type = 0, size = sizeof host;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &k) ==
        ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"~MHz", nullptr, &type, reinterpret_cast<BYTE*>(&host), &size) != ERROR_SUCCESS ||
            type != REG_DWORD)
            host = 0;
        RegCloseKey(k);
    }
    if (host) {
        CHECK_EQ(reg::QueryDword(cpu0, L"~MHz", &dw), nt::kSuccess);
        CHECK_EQ(dw, static_cast<std::uint32_t>(host));
        CHECK(dw >= 100 && dw <= 10000);
    } else {
        CHECK(reg::QueryDword(cpu0, L"~MHz", &dw) != nt::kSuccess);   // no host answer: absent, not invented
    }
}

TEST(registry_save_restore_and_load_key) {
    const std::wstring dir = TempDir(L"saverestore");
    CopyFixture(dir);
    {
        Session s(dir);
        const std::wstring file = dir + L"\\saved.hiv";
        HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        HANDLE fx = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
        SetDword(fx, L"Extra", 9);
        CHECK_EQ(reg::Reg_NtSaveKey(fx, f), nt::kSuccess);
        CloseHandle(f);
        reg::Close(fx);

        // Restore into a new key and compare
        HANDLE dst = nullptr;
        CHECK_EQ(Create(std::wstring(kSoftware) + L"\\Restored", &dst), nt::kSuccess);
        SetDword(dst, L"Stale", 1);
        HANDLE rf = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK_EQ(reg::Reg_NtRestoreKey(dst, rf, 0), nt::kSuccess);
        CloseHandle(rf);
        auto vals = ValueNames(dst);
        CHECK_EQ(vals.size(), 10u);   // 9 + Extra; "Stale" replaced
        CHECK(!Has(vals, L"Stale"));
        CHECK(Has(vals, L"Extra"));
        auto subs = SubkeyNames(dst);
        CHECK_EQ(subs.size(), 7u);
        reg::Close(dst);
        reg::KeyId k = 0;
        CHECK_EQ(reg::OpenKey(std::wstring(kSoftware) + L"\\Restored\\Many\\K0699", &k), nt::kSuccess);
        reg::CloseKey(k);
        reg::Value big;
        CHECK_EQ(reg::OpenKey(std::wstring(kSoftware) + L"\\Restored", &k), nt::kSuccess);
        CHECK_EQ(reg::QueryValue(k, L"Big", &big), nt::kSuccess);
        CHECK_EQ(big.data.size(), 40000u);
        reg::CloseKey(k);

        // NtLoadKey the saved file as a new hive and unload it again
        Oa target(L"\\Registry\\Machine\\Loaded");
        Oa source(L"\\??\\C:\\saved.hiv");
        CHECK_EQ(reg::Reg_NtLoadKey(&target.a, &source.a), nt::kSuccess);
        std::uint32_t dw = 0;
        CHECK_EQ(reg::QueryDword(L"\\Registry\\Machine\\Loaded\\Many\\K0001", L"Index", &dw), nt::kSuccess);
        CHECK_EQ(dw, 1u);
        CHECK_EQ(reg::Reg_NtLoadKey(&target.a, &source.a), nt::kObjectNameCollision);
        CHECK_EQ(reg::Reg_NtUnloadKey(&target.a), nt::kSuccess);
        CHECK_EQ(OpenStatus(L"\\Registry\\Machine\\Loaded"), nt::kObjectNameNotFound);
        Oa sys(L"\\Registry\\Machine\\SYSTEM");
        CHECK_EQ(reg::Reg_NtUnloadKey(&sys.a), nt::kCannotDelete);
        Oa missing(L"\\??\\C:\\nope.hiv");
        Oa target2(L"\\Registry\\Machine\\Loaded2");
        CHECK_EQ(reg::Reg_NtLoadKey(&target2.a, &missing.a), nt::kObjectNameNotFound);
    }
    RemoveTree(dir);
}

TEST(registry_notify_change) {
    Session s(FixtureRoot());
    HANDLE k = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IO_STATUS_BLOCK iosb{};
    iosb.Status = static_cast<NTSTATUS>(0x7777);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k, ev, nullptr, nullptr, &iosb, 4 /*LAST_SET*/, FALSE, nullptr, 0, TRUE), nt::kPending);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    // a subkey change does not match LAST_SET
    HANDLE c = nullptr;
    CHECK_EQ(Create(std::wstring(kSoftware) + L"\\Fixture\\N1", &c), nt::kSuccess);
    reg::Close(c);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    CHECK_EQ(SetDword(k, L"Notify", 1), nt::kSuccess);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    CHECK_EQ(iosb.Status, nt::kSuccess);
    ResetEvent(ev);
    // watch tree: a change two levels down fires. The filter and the tree flag of a handle are those of its first
    // request, so this one is made on a handle of its own.
    HANDLE kt = Open(std::wstring(kSoftware) + L"\\Fixture", kAll);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(kt, ev, nullptr, nullptr, &iosb, 4, TRUE, nullptr, 0, TRUE), nt::kPending);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);   // outstanding requests reset their event
    HANDLE deep = Open(std::wstring(kSoftware) + L"\\Fixture\\Many\\K0001", kAll);
    CHECK_EQ(SetDword(deep, L"Index", 5), nt::kSuccess);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    reg::Close(deep);
    reg::Close(kt);
    // closing the handle cancels a pending watch
    ResetEvent(ev);
    CHECK_EQ(reg::Reg_NtNotifyChangeKey(k, ev, nullptr, nullptr, &iosb, 4, FALSE, nullptr, 0, TRUE), nt::kPending);
    reg::Close(k);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    CHECK_EQ(iosb.Status, static_cast<NTSTATUS>(0x10B));   // STATUS_NOTIFY_CLEANUP
    CloseHandle(ev);
}

// Corrupted hives must fail soft (missing keys / values), never crash or hang.
namespace {
size_t WalkTree(const std::wstring& path, int depth) {
    reg::KeyId k = 0;
    if (reg::OpenKey(path, &k) < 0) return 0;
    size_t n = 1;
    std::vector<std::wstring> subs;
    std::vector<reg::Value> vals;
    reg::EnumerateValues(k, &vals);
    if (reg::EnumerateSubkeys(k, &subs) >= 0 && depth < 12)
        for (auto& s : subs) n += WalkTree(path + L"\\" + s, depth + 1);
    reg::CloseKey(k);
    return n;
}
}  // namespace

TEST(registry_corrupt_hive_is_survivable) {
    const std::wstring dir = TempDir(L"fuzz");
    CopyFixture(dir);
    const std::wstring sw = dir + L"\\Windows\\System32\\config\\SOFTWARE";
    std::vector<std::uint8_t> orig;
    {
        HANDLE f = CreateFileW(sw.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE);
        if (f == INVALID_HANDLE_VALUE) { RemoveTree(dir); return; }
        orig.resize(GetFileSize(f, nullptr));
        DWORD n = 0;
        ReadFile(f, orig.data(), static_cast<DWORD>(orig.size()), &n, nullptr);
        CloseHandle(f);
    }
    std::uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int iter = 0; iter < 60; ++iter) {
        std::vector<std::uint8_t> mut = orig;
        for (int j = 0; j < 12; ++j) mut[0x1000 + rnd() % (mut.size() - 0x1000)] = static_cast<std::uint8_t>(rnd());
        if (iter % 10 == 9) mut.resize(0x1000 + rnd() % (mut.size() - 0x1000));   // truncation
        HANDLE f = CreateFileW(sw.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(f, mut.data(), static_cast<DWORD>(mut.size()), &w, nullptr);
        CloseHandle(f);
        Session s(dir);
        WalkTree(kSoftware, 0);   // only the absence of a crash/hang matters
    }
    // Garbage that is not a hive at all: mounted as an empty hive.
    {
        HANDLE f = CreateFileW(sw.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        const char junk[] = "this is not a registry hive";
        DWORD w = 0;
        WriteFile(f, junk, sizeof junk, &w, nullptr);
        CloseHandle(f);
        Session s(dir);
        CHECK_EQ(s.init, nt::kSuccess);
        CHECK_EQ(OpenStatus(std::wstring(kSoftware) + L"\\Fixture"), nt::kObjectNameNotFound);
        std::wstring str;
        CHECK_EQ(reg::QueryString(std::wstring(kSoftware) + L"\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", &str), nt::kSuccess);   // fidelity fallback
    }
    RemoveTree(dir);
}
