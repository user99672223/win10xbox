// Syscalls that route to modules which already exist: transacted key opens
// (the transaction is ignored), NtLoadKey2, and the default locale state kept
// by core/dispatch/nt_nls.cpp. Each is checked against its non-transacted or
// set/query sibling.
#include "check.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/registry.h"

#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reg = win10xbox::registry;
namespace nt = win10xbox::registry::nt;
namespace dsp = win10xbox::dispatch;

namespace {

constexpr ULONG kRead = 0x20019;        // KEY_READ
constexpr NTSTATUS kOk = 0;
constexpr BOOLEAN kUser = TRUE;
constexpr BOOLEAN kSystem = FALSE;
constexpr NTSTATUS kInvalid = static_cast<NTSTATUS>(0xC000000D);   // STATUS_INVALID_PARAMETER
constexpr NTSTATUS kNotFound = static_cast<NTSTATUS>(0xC0000034);  // STATUS_OBJECT_NAME_NOT_FOUND
constexpr const wchar_t* kSoftware = L"\\Registry\\Machine\\SOFTWARE";
constexpr const wchar_t* kCurrentVersion = L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

// A handle value that no open produced: the transaction the open must ignore.
HANDLE FakeTransaction() { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234)); }

// ---- path helpers (as in test_registry.cpp) -----------------------------------

std::wstring HostPath(const char* p) {
    std::wstring w;
    for (; *p; ++p) w += (*p == '/') ? L'\\' : static_cast<wchar_t>(static_cast<unsigned char>(*p));
    if (!w.empty() && w[0] == L'\\') w = L"Z:" + w;    // Wine: unix absolute paths live on Z:
    return w;
}

std::wstring FixtureRoot() { return HostPath(WIN10XBOX_FIXTURES_DIR) + L"\\registry\\machine"; }

// ---- NT call helpers -----------------------------------------------------------

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
    explicit Oa(const std::wstring& path) : name(path) {
        std::memset(&a, 0, sizeof a);
        a.Length = sizeof a;
        a.ObjectName = &name.u;
        a.Attributes = 0x40;   // OBJ_CASE_INSENSITIVE
    }
};

struct Session {
    explicit Session(const std::wstring& root) { init = reg::Initialize(root); }
    ~Session() { reg::Shutdown(); }
    NTSTATUS init;
};

// Reads one value through a key handle (KeyValuePartialInformation, class 2)
// and returns the whole output buffer, so two handles can be compared byte for byte.
std::vector<std::uint8_t> ValueBytes(HANDLE h, const wchar_t* name, NTSTATUS* status, ULONG* result_len) {
    Us n(name);
    std::vector<std::uint8_t> b(512, 0xCD);
    *result_len = 0;
    *status = reg::Reg_NtQueryValueKey(h, &n.u, 2, b.data(), 512, result_len);
    return b;
}

// The Nt entry points as the dispatcher's Emulate table holds them.
const dsp::Entry* Emulated(const char* name) {
    const dsp::Entry* e = dsp::LookupByName(name);
    return (e && e->policy == dsp::Policy::Emulate && e->impl) ? e : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Transacted opens: same result as the plain open, transaction ignored.

TEST(syscalls_open_transacted_equals_plain) {
    Session s(FixtureRoot());
    CHECK_EQ(s.init, nt::kSuccess);
    Oa cv(kCurrentVersion);

    HANDLE plain = nullptr;
    const NTSTATUS sp = reg::Reg_NtOpenKey(&plain, kRead, &cv.a);
    CHECK_EQ(sp, kOk);
    for (HANDLE tx : {static_cast<HANDLE>(nullptr), FakeTransaction()}) {
        HANDLE h = nullptr;
        const NTSTATUS st = reg::Reg_NtOpenKeyTransacted(&h, kRead, &cv.a, tx);
        CHECK_EQ(st, sp);
        if (st < 0 || sp < 0) continue;
        NTSTATUS sa = 0, sb = 0;
        ULONG la = 0, lb = 0;
        CHECK(ValueBytes(h, L"ProductName", &sa, &la) == ValueBytes(plain, L"ProductName", &sb, &lb));
        CHECK_EQ(sa, sb);
        CHECK_EQ(la, lb);
        CHECK_EQ(sa, kOk);
        reg::Close(h);
    }
    if (sp >= 0) reg::Close(plain);

    // Missing key: both fail the same way.
    Oa missing(std::wstring(kSoftware) + L"\\NoSuchKey");
    HANDLE hp = nullptr, ht = nullptr;
    CHECK_EQ(reg::Reg_NtOpenKey(&hp, kRead, &missing.a), kNotFound);
    CHECK_EQ(reg::Reg_NtOpenKeyTransacted(&ht, kRead, &missing.a, FakeTransaction()), kNotFound);

    // Invalid arguments: identical to the plain open's answer.
    CHECK_EQ(reg::Reg_NtOpenKeyTransacted(nullptr, kRead, &cv.a, FakeTransaction()),
             reg::Reg_NtOpenKey(nullptr, kRead, &cv.a));
}

TEST(syscalls_open_transacted_ex_equals_plain_ex) {
    Session s(FixtureRoot());
    CHECK_EQ(s.init, nt::kSuccess);
    Oa cv(kCurrentVersion);

    for (ULONG options : {0UL, 0x00000008UL /* REG_OPTION_OPEN_LINK */}) {
        HANDLE plain = nullptr, ht = nullptr;
        const NTSTATUS sp = reg::Reg_NtOpenKeyEx(&plain, kRead, &cv.a, options);
        const NTSTATUS st = reg::Reg_NtOpenKeyTransactedEx(&ht, kRead, &cv.a, options, FakeTransaction());
        CHECK_EQ(st, sp);
        if (sp >= 0 && st >= 0) {
            NTSTATUS sa = 0, sb = 0;
            ULONG la = 0, lb = 0;
            CHECK(ValueBytes(ht, L"ProductName", &sa, &la) == ValueBytes(plain, L"ProductName", &sb, &lb));
            reg::Close(ht);
        }
        if (plain && sp >= 0) reg::Close(plain);
    }
}

TEST(syscalls_transacted_entries_are_emulated_registry) {
    dsp::Initialize();
    const dsp::Entry* t = Emulated("NtOpenKeyTransacted");
    const dsp::Entry* tx = Emulated("NtOpenKeyTransactedEx");
    CHECK(t != nullptr && t->impl == reinterpret_cast<void*>(&reg::Reg_NtOpenKeyTransacted));
    CHECK(tx != nullptr && tx->impl == reinterpret_cast<void*>(&reg::Reg_NtOpenKeyTransactedEx));
    const dsp::Entry* lk2 = Emulated("NtLoadKey2");
    CHECK(lk2 != nullptr && lk2->impl == reinterpret_cast<void*>(&reg::Reg_NtLoadKey2));
    CHECK(t && std::strcmp(t->note, "registry (transaction ignored)") == 0);
}

// ---------------------------------------------------------------------------
// NtLoadKey2: NtLoadKeyEx with the flags. A missing source fails the same way.

TEST(syscalls_load_key2_equals_load_key_ex) {
    Session s(FixtureRoot());
    CHECK_EQ(s.init, nt::kSuccess);
    Oa target(std::wstring(kSoftware) + L"\\Loaded");
    Oa missing_file(L"\\??\\C:\\no\\such\\hive.hiv");
    for (ULONG flags : {0UL, 0x00000004UL /* REG_NO_LAZY_FLUSH */}) {
        const NTSTATUS a = reg::Reg_NtLoadKey2(&target.a, &missing_file.a, flags);
        const NTSTATUS b = reg::Reg_NtLoadKeyEx(&target.a, &missing_file.a, flags, nullptr, nullptr, 0, nullptr, nullptr);
        CHECK_EQ(a, b);
        // The fixture hives are read-only here, so the load is refused; the point is that
        // the refusal is the same one NtLoadKeyEx gives, with nothing mounted.
        CHECK(a < 0);
    }
    CHECK_EQ(reg::Reg_NtLoadKey2(&target.a, nullptr, 0), kInvalid);
    CHECK_EQ(reg::Reg_NtLoadKey2(&target.a, nullptr, 0),
             reg::Reg_NtLoadKeyEx(&target.a, nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr));
}

// ---------------------------------------------------------------------------
// Default locale state: set then query, separate user and system slots, and
// invalid arguments rejected without changing anything.

TEST(syscalls_default_locale_set_then_query) {
    dsp::Initialize();
    const dsp::Entry* qloc = Emulated("NtQueryDefaultLocale");
    const dsp::Entry* sloc = Emulated("NtSetDefaultLocale");
    const dsp::Entry* qui = Emulated("NtQueryDefaultUILanguage");
    const dsp::Entry* sui = Emulated("NtSetDefaultUILanguage");
    CHECK(qloc && sloc && qui && sui);
    if (!(qloc && sloc && qui && sui)) return;

    using QueryLocale = NTSTATUS(NTAPI*)(BOOLEAN, PLCID);
    using SetLocale = NTSTATUS(NTAPI*)(BOOLEAN, LCID);
    using QueryUI = NTSTATUS(NTAPI*)(LANGID*);
    using SetUI = NTSTATUS(NTAPI*)(LANGID);
    auto query = reinterpret_cast<QueryLocale>(qloc->impl);
    auto set = reinterpret_cast<SetLocale>(sloc->impl);
    auto query_ui = reinterpret_cast<QueryUI>(qui->impl);
    auto set_ui = reinterpret_cast<SetUI>(sui->impl);

    // Remember the starting state and put it back at the end.
    LCID user0 = 0, sys0 = 0;
    LANGID ui0 = 0;
    CHECK_EQ(query(kUser, &user0), kOk);
    CHECK_EQ(query(kSystem, &sys0), kOk);
    CHECK_EQ(query_ui(&ui0), kOk);
    CHECK(user0 != 0 && sys0 != 0 && ui0 != 0);

    LCID got = 0;
    CHECK_EQ(set(kUser, 0x0407), kOk);                    // de-DE for the user
    CHECK_EQ(query(kUser, &got), kOk);
    CHECK_EQ(got, static_cast<LCID>(0x0407));
    CHECK_EQ(query(kSystem, &got), kOk);
    CHECK_EQ(got, sys0);                                 // the system slot did not move

    CHECK_EQ(set(kSystem, 0x0C09), kOk);                   // en-AU for the system
    CHECK_EQ(query(kSystem, &got), kOk);
    CHECK_EQ(got, static_cast<LCID>(0x0C09));
    CHECK_EQ(query(kUser, &got), kOk);
    CHECK_EQ(got, static_cast<LCID>(0x0407));

    LANGID ui = 0;
    CHECK_EQ(set_ui(0x0407), kOk);
    CHECK_EQ(query_ui(&ui), kOk);
    CHECK_EQ(ui, static_cast<LANGID>(0x0407));

    CHECK_EQ(set(kUser, user0), kOk);
    CHECK_EQ(set(kSystem, sys0), kOk);
    CHECK_EQ(set_ui(ui0), kOk);
}

TEST(syscalls_default_locale_invalid_arguments) {
    dsp::Initialize();
    const dsp::Entry* qloc = Emulated("NtQueryDefaultLocale");
    const dsp::Entry* sloc = Emulated("NtSetDefaultLocale");
    const dsp::Entry* qui = Emulated("NtQueryDefaultUILanguage");
    const dsp::Entry* sui = Emulated("NtSetDefaultUILanguage");
    CHECK(qloc && sloc && qui && sui);
    if (!(qloc && sloc && qui && sui)) return;

    using QueryLocale = NTSTATUS(NTAPI*)(BOOLEAN, PLCID);
    using SetLocale = NTSTATUS(NTAPI*)(BOOLEAN, LCID);
    using QueryUI = NTSTATUS(NTAPI*)(LANGID*);
    using SetUI = NTSTATUS(NTAPI*)(LANGID);
    auto query = reinterpret_cast<QueryLocale>(qloc->impl);
    auto set = reinterpret_cast<SetLocale>(sloc->impl);
    auto query_ui = reinterpret_cast<QueryUI>(qui->impl);
    auto set_ui = reinterpret_cast<SetUI>(sui->impl);

    LCID before_user = 0, before_sys = 0;
    LANGID before_ui = 0;
    CHECK_EQ(query(kUser, &before_user), kOk);
    CHECK_EQ(query(kSystem, &before_sys), kOk);
    CHECK_EQ(query_ui(&before_ui), kOk);

    CHECK_EQ(set(kUser, 0), kInvalid);
    CHECK_EQ(set(kSystem, 0), kInvalid);
    CHECK_EQ(set_ui(0), kInvalid);
    CHECK_EQ(query(kUser, nullptr), kInvalid);
    CHECK_EQ(query(kSystem, nullptr), kInvalid);
    CHECK_EQ(query_ui(nullptr), kInvalid);

    LCID after_user = 0, after_sys = 0;
    LANGID after_ui = 0;
    CHECK_EQ(query(kUser, &after_user), kOk);
    CHECK_EQ(query(kSystem, &after_sys), kOk);
    CHECK_EQ(query_ui(&after_ui), kOk);
    CHECK_EQ(after_user, before_user);
    CHECK_EQ(after_sys, before_sys);
    CHECK_EQ(after_ui, before_ui);
}
