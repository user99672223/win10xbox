#include "check.h"
#include "win10xbox/token.h"
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tok = win10xbox::token;

namespace {
constexpr NTSTATUS kBufTooSmall = (NTSTATUS)0xC0000023;
constexpr NTSTATUS kNotAllAssigned = (NTSTATUS)0x00000106;
constexpr NTSTATUS kInfoClass = (NTSTATUS)0xC0000003;
constexpr NTSTATUS kInvalidHandle = (NTSTATUS)0xC0000008;
constexpr NTSTATUS kAccessDenied = (NTSTATUS)0xC0000022;

enum { TUser = 1, TGroups = 2, TPrivileges = 3, TOwner = 4, TPrimaryGroup = 5, TDefaultDacl = 6, TSource = 7, TType = 8,
       TImpLevel = 9, TStatistics = 10, TSessionId = 12, TElevationType = 18, TElevation = 20, TIntegrity = 25,
       TIsAppContainer = 29, TAppContainerSid = 31, TVirtAllowed = 23, TVirtEnabled = 24, TLogonSid = 28, TLinked = 19,
       TGroupsAndPrivs = 13, TOrigin = 17, TMandatoryPolicy = 27, TUIAccess = 26 };

HANDLE OpenPrimary(ACCESS_MASK a = TOKEN_ALL_ACCESS) {
    HANDLE h = nullptr;
    NTSTATUS st = tok::Tok_NtOpenProcessToken((HANDLE)(intptr_t)-1, a, &h);
    CHECK_EQ(st, 0);
    return h;
}

NTSTATUS Query(HANDLE h, int cls, std::vector<std::uint8_t>& buf, ULONG* need = nullptr) {
    ULONG n = 0;
    NTSTATUS st = tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)cls, nullptr, 0, &n);
    if (st != kBufTooSmall) { if (need) *need = n; return st; }
    buf.assign(n + 8, 0xAA);   // canary tail
    ULONG n2 = 0;
    st = tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)cls, buf.data(), n, &n2);
    CHECK_EQ(n2, n);
    CHECK_EQ(buf[n], 0xAA);    // did not write past the reported length
    buf.resize(n);
    if (need) *need = n;
    return st;
}

template <class T> T U(const std::vector<std::uint8_t>& b, size_t off = 0) { T v; std::memcpy(&v, b.data() + off, sizeof v); return v; }
bool InBuf(const std::vector<std::uint8_t>& b, const void* p, size_t n) {
    auto* q = (const std::uint8_t*)p;
    return q >= b.data() && q + n <= b.data() + b.size();
}
std::uint32_t LastSub(const void* sid) {
    auto* b = (const std::uint8_t*)sid;
    std::uint32_t v;
    std::memcpy(&v, b + 8 + 4 * (b[1] - 1), 4);
    return v;
}
std::wstring Str(const void* sid) { return tok::SidToString(sid); }
void ResetIdentity() { tok::Initialize(tok::Identity{}); }

// Does the TOKEN_GROUPS blob contain this SID? returns attributes or -1.
long long FindGroup(const std::vector<std::uint8_t>& b, const wchar_t* sid) {
    std::uint32_t n = U<std::uint32_t>(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t p = U<std::uint64_t>(b, 8 + 16 * i);
        CHECK(InBuf(b, (const void*)(std::uintptr_t)p, 8));
        if (Str((const void*)(std::uintptr_t)p) == sid) return U<std::uint32_t>(b, 8 + 16 * i + 8);
    }
    return -1;
}
}  // namespace

TEST(token_user_sid_matches_identity) {
    tok::Identity id;
    id.user_sid = L"S-1-5-21-1111-2222-3333-1234";
    id.user_name = L"Varsha";
    CHECK_EQ(tok::Initialize(id), 0);
    CHECK(tok::UserSidString() == L"S-1-5-21-1111-2222-3333-1234");
    CHECK(tok::UserName() == L"Varsha");
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(h, TUser, b), 0);
    CHECK_EQ(b.size(), (size_t)(16 + 8 + 4 * 5));
    auto* sid = (const void*)(std::uintptr_t)U<std::uint64_t>(b);
    CHECK(InBuf(b, sid, 28));
    CHECK(Str(sid) == L"S-1-5-21-1111-2222-3333-1234");
    CHECK_EQ(U<std::uint32_t>(b, 8), 0u);   // attributes
    CHECK_EQ(LastSub(sid), 1234u);
    // Binary layout by hand: revision 1, 5 subauthorities, authority 5 (big endian)
    auto* raw = (const std::uint8_t*)sid;
    CHECK_EQ(raw[0], 1); CHECK_EQ(raw[1], 5);
    CHECK_EQ(raw[7], 5); CHECK_EQ(raw[2], 0);
    tok::Close(h);
    ResetIdentity();
}

TEST(token_invalid_identity_rejected) {
    tok::Identity id;
    id.user_sid = L"not-a-sid";
    CHECK(tok::Initialize(id) != 0);
    CHECK(tok::UserSidString() == tok::Identity{}.user_sid);   // previous identity kept
}

TEST(token_not_elevated_medium_not_appcontainer) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(h, TElevation, b), 0);
    CHECK_EQ(b.size(), 4u);
    CHECK_EQ(U<std::uint32_t>(b), 0u);
    CHECK_EQ(Query(h, TElevationType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 3u);   // TokenElevationTypeLimited
    CHECK_EQ(Query(h, TIsAppContainer, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 0u);
    CHECK_EQ(Query(h, TAppContainerSid, b), 0);
    CHECK_EQ(U<std::uint64_t>(b), 0ull);
    CHECK_EQ(Query(h, TSessionId, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);
    CHECK_EQ(Query(h, TType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);   // TokenPrimary
    CHECK_EQ(Query(h, TVirtAllowed, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);
    CHECK_EQ(Query(h, TVirtEnabled, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 0u);
    CHECK_EQ(Query(h, TUIAccess, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 0u);
    CHECK_EQ(Query(h, TMandatoryPolicy, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 3u);
    CHECK(Query(h, TLinked, b) != 0);
    // medium integrity
    CHECK_EQ(Query(h, TIntegrity, b), 0);
    auto* sid = (const void*)(std::uintptr_t)U<std::uint64_t>(b);
    CHECK(InBuf(b, sid, 12));
    CHECK(Str(sid) == L"S-1-16-8192");
    CHECK_EQ(LastSub(sid), (std::uint32_t)SECURITY_MANDATORY_MEDIUM_RID);
    CHECK_EQ(U<std::uint32_t>(b, 8), 0x60u);
    tok::Close(h);
}

TEST(token_elevated_identity) {
    tok::Identity id;
    id.elevated = true;
    CHECK_EQ(tok::Initialize(id), 0);
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(h, TElevation, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);
    CHECK_EQ(Query(h, TElevationType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 2u);
    CHECK_EQ(Query(h, TIntegrity, b), 0);
    CHECK_EQ(LastSub((const void*)(std::uintptr_t)U<std::uint64_t>(b)), 0x3000u);
    CHECK_EQ(Query(h, TGroups, b), 0);
    CHECK(FindGroup(b, L"S-1-5-32-544") > 0);
    tok::Close(h);
    ResetIdentity();
}

TEST(token_groups_are_the_interactive_set) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(h, TGroups, b), 0);
    CHECK(FindGroup(b, L"S-1-1-0") == 7);        // Everyone
    CHECK(FindGroup(b, L"S-1-5-32-545") == 7);   // Users
    CHECK(FindGroup(b, L"S-1-5-4") == 7);        // INTERACTIVE
    CHECK(FindGroup(b, L"S-1-5-11") == 7);       // Authenticated Users
    CHECK(FindGroup(b, L"S-1-2-0") == 7);        // LOCAL
    CHECK(FindGroup(b, L"S-1-2-1") == 7);        // CONSOLE LOGON
    CHECK(FindGroup(b, L"S-1-16-8192") == 0x60); // Medium mandatory label
    CHECK(FindGroup(b, L"S-1-5-32-544") == -1);  // not an admin
    // logon-session SID
    std::vector<std::uint8_t> l;
    CHECK_EQ(Query(h, TLogonSid, l), 0);
    CHECK_EQ(U<std::uint32_t>(l), 1u);
    CHECK_EQ(U<std::uint32_t>(l, 8 + 8), 0xC0000007u);
    std::uint32_t n = U<std::uint32_t>(b);
    CHECK_EQ(n, 9u);
    // owner / primary group / default DACL
    std::vector<std::uint8_t> o, pg, dacl;
    CHECK_EQ(Query(h, TOwner, o), 0);
    CHECK(Str((const void*)(std::uintptr_t)U<std::uint64_t>(o)) == tok::UserSidString());
    CHECK_EQ(Query(h, TPrimaryGroup, pg), 0);
    CHECK_EQ(LastSub((const void*)(std::uintptr_t)U<std::uint64_t>(pg)), 513u);
    CHECK_EQ(Query(h, TDefaultDacl, dacl), 0);
    auto* acl = (const std::uint8_t*)(std::uintptr_t)U<std::uint64_t>(dacl);
    CHECK(InBuf(dacl, acl, 8));
    CHECK_EQ(acl[0], 2);   // ACL_REVISION
    CHECK_EQ(*(const std::uint16_t*)(acl + 4), 2);   // two ACEs
    CHECK_EQ(*(const std::uint16_t*)(acl + 2), dacl.size() - 8);
    tok::Close(h);
}

TEST(token_statistics_sane) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(h, TStatistics, b), 0);
    CHECK_EQ(b.size(), sizeof(TOKEN_STATISTICS));
    TOKEN_STATISTICS s;
    std::memcpy(&s, b.data(), sizeof s);
    CHECK(s.TokenId.LowPart != 0 || s.TokenId.HighPart != 0);
    CHECK(s.AuthenticationId.LowPart != 0);
    CHECK_EQ(s.ExpirationTime.QuadPart, 0x7FFFFFFFFFFFFFFFll);
    CHECK_EQ((int)s.TokenType, 1);
    CHECK_EQ(s.PrivilegeCount, 5u);
    CHECK_EQ(s.GroupCount, 9u);
    CHECK(s.DynamicCharged >= s.DynamicAvailable);
    // Source
    CHECK_EQ(Query(h, TSource, b), 0);
    CHECK_EQ(b.size(), sizeof(TOKEN_SOURCE));
    CHECK_EQ(std::memcmp(b.data(), "User32", 6), 0);
    // GroupsAndPrivileges sanity
    CHECK_EQ(Query(h, TGroupsAndPrivs, b), 0);
    CHECK_EQ(U<std::uint32_t>(b, 0), 10u);   // user + 9 groups
    CHECK_EQ(U<std::uint32_t>(b, 32), 5u);
    CHECK(InBuf(b, (const void*)(std::uintptr_t)U<std::uint64_t>(b, 8), 16));
    // Impersonation level is an error on a primary token
    CHECK_EQ(Query(h, TImpLevel, b), kInfoClass);
    tok::Close(h);
}

TEST(token_buffer_too_small_reports_required_length) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    ULONG need = 0;
    std::uint8_t tiny[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    NTSTATUS st = tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)TUser, tiny, sizeof tiny, &need);
    CHECK_EQ(st, kBufTooSmall);
    CHECK_EQ(need, (ULONG)(16 + 8 + 4 * 5));
    CHECK_EQ(tiny[0], 0xEE);   // untouched
    // One byte short is still too small; exact is fine.
    std::vector<std::uint8_t> buf(need);
    ULONG n2 = 0;
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)TUser, buf.data(), need - 1, &n2), kBufTooSmall);
    CHECK_EQ(n2, need);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)TUser, buf.data(), need, &n2), 0);
    // statistics: fixed size
    st = tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)TStatistics, tiny, 4, &need);
    CHECK_EQ(st, kBufTooSmall);
    CHECK_EQ(need, 56u);
    // bad handle / bad class / missing access
    CHECK_EQ(tok::Tok_NtQueryInformationToken((HANDLE)(intptr_t)0x1234, (TOKEN_INFORMATION_CLASS)TUser, buf.data(), need, &n2), kInvalidHandle);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)99, buf.data(), need, &n2), kInfoClass);
    tok::Close(h);
    HANDLE noq = OpenPrimary(TOKEN_ADJUST_PRIVILEGES);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(noq, (TOKEN_INFORMATION_CLASS)TUser, buf.data(), need, &n2), kAccessDenied);
    tok::Close(noq);
    // closed handle is invalid
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, (TOKEN_INFORMATION_CLASS)TUser, buf.data(), need, &n2), kInvalidHandle);
}

TEST(token_adjust_privileges_enable_and_report) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    std::vector<std::uint8_t> b;
    auto find = [&](DWORD luid) -> long long {
        CHECK_EQ(Query(h, TPrivileges, b), 0);
        std::uint32_t n = U<std::uint32_t>(b);
        for (std::uint32_t i = 0; i < n; ++i)
            if (U<std::uint32_t>(b, 4 + 12 * i) == luid) return U<std::uint32_t>(b, 4 + 12 * i + 8);
        return -1;
    };
    CHECK_EQ(find(19), 0);          // SeShutdownPrivilege present, disabled
    CHECK_EQ(find(23), 3);          // SeChangeNotifyPrivilege enabled by default + enabled
    CHECK_EQ(find(20), -1);         // SeDebugPrivilege not held

    TOKEN_PRIVILEGES np{};
    np.PrivilegeCount = 1;
    np.Privileges[0].Luid.LowPart = 19;
    np.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    std::uint8_t prevbuf[64];
    ULONG ret = 0;
    NTSTATUS st = tok::Tok_NtAdjustPrivilegesToken(h, FALSE, &np, sizeof prevbuf, (PTOKEN_PRIVILEGES)prevbuf, &ret);
    CHECK_EQ(st, 0);
    CHECK_EQ(ret, 16u);
    auto* prev = (TOKEN_PRIVILEGES*)prevbuf;
    CHECK_EQ(prev->PrivilegeCount, 1u);
    CHECK_EQ(prev->Privileges[0].Luid.LowPart, 19u);
    CHECK_EQ(prev->Privileges[0].Attributes, 0u);        // previous state: disabled
    CHECK_EQ(find(19), 2);                               // now enabled

    // A second token handle sees the same underlying token.
    HANDLE h2 = OpenPrimary(TOKEN_QUERY);
    CHECK_EQ(Query(h2, TPrivileges, b), 0);
    bool seen = false;
    for (std::uint32_t i = 0; i < U<std::uint32_t>(b); ++i)
        if (U<std::uint32_t>(b, 4 + 12 * i) == 19) seen = U<std::uint32_t>(b, 4 + 12 * i + 8) == 2;
    CHECK(seen);
    tok::Close(h2);

    // Re-enabling changes nothing: previous state is empty.
    st = tok::Tok_NtAdjustPrivilegesToken(h, FALSE, &np, sizeof prevbuf, (PTOKEN_PRIVILEGES)prevbuf, &ret);
    CHECK_EQ(st, 0);
    CHECK_EQ(prev->PrivilegeCount, 0u);

    // Not held -> STATUS_NOT_ALL_ASSIGNED, the held one is still applied.
    // TOKEN_PRIVILEGES has ANYSIZE_ARRAY == 1, so use a bigger local layout-compatible struct
    struct { DWORD n; LUID_AND_ATTRIBUTES a[2]; } big{};
    big.n = 2;
    big.a[0].Luid.LowPart = 20;   // SeDebug: not held
    big.a[0].Attributes = SE_PRIVILEGE_ENABLED;
    big.a[1].Luid.LowPart = 25;   // SeUndock
    big.a[1].Attributes = SE_PRIVILEGE_ENABLED;
    st = tok::Tok_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&big, 0, nullptr, nullptr);
    CHECK_EQ(st, kNotAllAssigned);
    CHECK_EQ(find(25), 2);

    // Previous-state buffer too small: nothing changes, required size reported.
    struct { DWORD n; LUID_AND_ATTRIBUTES a[1]; } one{};
    one.n = 1; one.a[0].Luid.LowPart = 33; one.a[0].Attributes = SE_PRIVILEGE_ENABLED;
    st = tok::Tok_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&one, 4, (PTOKEN_PRIVILEGES)prevbuf, &ret);
    CHECK_EQ(st, kBufTooSmall);
    CHECK_EQ(ret, 16u);
    CHECK_EQ(find(33), 0);

    // Disable all; previous state lists what was enabled (19, 23, 25).
    st = tok::Tok_NtAdjustPrivilegesToken(h, TRUE, nullptr, sizeof prevbuf, (PTOKEN_PRIVILEGES)prevbuf, &ret);
    CHECK_EQ(st, 0);
    CHECK_EQ(prev->PrivilegeCount, 3u);
    CHECK_EQ(find(23), 1);
    CHECK_EQ(find(19), 0);

    // Remove
    big.n = 1; big.a[0].Luid.LowPart = 34; big.a[0].Attributes = SE_PRIVILEGE_REMOVED;
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&big, 0, nullptr, nullptr), 0);
    CHECK_EQ(find(34), -1);

    // access checks
    HANDLE ro = OpenPrimary(TOKEN_QUERY);
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(ro, TRUE, nullptr, 0, nullptr, nullptr), kAccessDenied);
    tok::Close(ro);
    tok::Close(h);
    ResetIdentity();   // restore privilege state for later tests
}

TEST(token_adjust_groups) {
    ResetIdentity();
    HANDLE h = OpenPrimary();
    struct { DWORD n; SID_AND_ATTRIBUTES a[1]; } g{};
    std::vector<std::uint8_t> users;
    CHECK(tok::SidFromString(L"S-1-5-32-545", &users));
    g.n = 1; g.a[0].Sid = users.data(); g.a[0].Attributes = 0;
    CHECK_EQ(tok::Tok_NtAdjustGroupsToken(h, FALSE, (PTOKEN_GROUPS)&g, 0, nullptr, nullptr), (NTSTATUS)0xC000005D);   // mandatory
    std::vector<std::uint8_t> bogus;
    CHECK(tok::SidFromString(L"S-1-5-99", &bogus));
    g.a[0].Sid = bogus.data(); g.a[0].Attributes = SE_GROUP_ENABLED;
    CHECK_EQ(tok::Tok_NtAdjustGroupsToken(h, FALSE, (PTOKEN_GROUPS)&g, 0, nullptr, nullptr), kNotAllAssigned);
    CHECK_EQ(tok::Tok_NtAdjustGroupsToken(h, TRUE, nullptr, 0, nullptr, nullptr), 0);
    tok::Close(h);
}

TEST(token_open_variants_and_pseudo_handles) {
    ResetIdentity();
    CHECK(!tok::IsTokenHandle((HANDLE)0x1c4));
    HANDLE a = nullptr, b2 = nullptr, c = nullptr;
    CHECK_EQ(tok::Tok_NtOpenProcessTokenEx((HANDLE)(intptr_t)-1, TOKEN_QUERY, 0, &a), 0);
    CHECK_EQ(tok::Tok_NtOpenThreadToken((HANDLE)(intptr_t)-2, TOKEN_QUERY, TRUE, &b2), 0);
    CHECK_EQ(tok::Tok_NtOpenThreadTokenEx((HANDLE)(intptr_t)-2, MAXIMUM_ALLOWED, FALSE, 0, &c), 0);
    CHECK(tok::IsTokenHandle(a) && tok::IsTokenHandle(b2) && tok::IsTokenHandle(c));
    CHECK(a != b2 && b2 != c);
    CHECK(tok::ToHandle(tok::FromHandle(a)) == a);
    CHECK((std::uintptr_t)a < 0x80000000u);
    CHECK(tok::FromHandle((HANDLE)0x1c4) == 0);
    CHECK(tok::IsTokenHandle(tok::ToHandle(tok::PrimaryToken())));
    // Closing one handle leaves the others valid.
    CHECK_EQ(tok::Close(a), 0);
    CHECK_EQ(tok::Close(a), kInvalidHandle);
    ULONG n = 0;
    CHECK_EQ(tok::Tok_NtQueryInformationToken(b2, (TOKEN_INFORMATION_CLASS)TSessionId, &n, 4, &n), 0);
    // the module's own handle survives Close
    CHECK_EQ(tok::Close(tok::ToHandle(tok::PrimaryToken())), 0);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(tok::ToHandle(tok::PrimaryToken()), (TOKEN_INFORMATION_CLASS)TSessionId, &n, 4, &n), 0);
    // pseudo handles
    ULONG sid = 99;
    CHECK_EQ(tok::Tok_NtQueryInformationToken((HANDLE)(intptr_t)-4, (TOKEN_INFORMATION_CLASS)TSessionId, &sid, 4, &n), 0);
    CHECK_EQ(sid, 1u);
    CHECK_EQ(tok::Tok_NtQueryInformationToken((HANDLE)(intptr_t)-6, (TOKEN_INFORMATION_CLASS)TSessionId, &sid, 4, &n), 0);
    CHECK_EQ(tok::Tok_NtQueryInformationToken((HANDLE)(intptr_t)-5, (TOKEN_INFORMATION_CLASS)TSessionId, &sid, 4, &n), (NTSTATUS)0xC000007C);  // STATUS_NO_TOKEN
    // access mask mapping / rejection
    HANDLE d = nullptr;
    CHECK_EQ(tok::Tok_NtOpenProcessToken((HANDLE)(intptr_t)-1, SYNCHRONIZE, &d), kAccessDenied);
    tok::Close(b2); tok::Close(c);
}

TEST(token_duplicate_impersonation_and_thread_token) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    HANDLE imp = nullptr;
    SECURITY_QUALITY_OF_SERVICE q{};
    q.Length = sizeof q;
    q.ImpersonationLevel = SecurityIdentification;
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.SecurityQualityOfService = &q;
    CHECK_EQ(tok::Tok_NtDuplicateToken(p, TOKEN_ALL_ACCESS, &oa, FALSE, TokenImpersonation, &imp), 0);
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(imp, TType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 2u);
    CHECK_EQ(Query(imp, TImpLevel, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), (std::uint32_t)SecurityIdentification);
    // an identification-level impersonation token cannot become primary
    HANDLE prim2 = nullptr;
    CHECK_EQ(tok::Tok_NtDuplicateToken(imp, TOKEN_ALL_ACCESS, nullptr, FALSE, TokenPrimary, &prim2), (NTSTATUS)0xC00000A5);
    // default: no attributes -> impersonation level SecurityImpersonation
    HANDLE imp2 = nullptr;
    CHECK_EQ(tok::Tok_NtDuplicateToken(p, TOKEN_QUERY, nullptr, FALSE, TokenImpersonation, &imp2), 0);
    CHECK_EQ(Query(imp2, TImpLevel, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), (std::uint32_t)SecurityImpersonation);
    // distinct token id, same user
    std::vector<std::uint8_t> s1, s2;
    CHECK_EQ(Query(p, TStatistics, s1), 0);
    CHECK_EQ(Query(imp2, TStatistics, s2), 0);
    CHECK(std::memcmp(s1.data(), s2.data(), 8) != 0);
    BOOLEAN eq = FALSE;
    CHECK_EQ(tok::Tok_NtCompareTokens(p, imp2, &eq), 0);
    CHECK(eq);
    // duplicate needs TOKEN_DUPLICATE
    HANDLE q2 = OpenPrimary(TOKEN_QUERY);
    HANDLE x = nullptr;
    CHECK_EQ(tok::Tok_NtDuplicateToken(q2, TOKEN_QUERY, nullptr, FALSE, TokenImpersonation, &x), kAccessDenied);
    tok::Close(q2);
    // modifying the duplicate does not touch the original
    struct { DWORD n; LUID_AND_ATTRIBUTES a[1]; } one{};
    one.n = 1; one.a[0].Luid.LowPart = 19; one.a[0].Attributes = SE_PRIVILEGE_ENABLED;
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(imp2, FALSE, (PTOKEN_PRIVILEGES)&one, 0, nullptr, nullptr), (NTSTATUS)kAccessDenied);  // opened with TOKEN_QUERY only
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(imp, FALSE, (PTOKEN_PRIVILEGES)&one, 0, nullptr, nullptr), 0);
    CHECK_EQ(tok::Tok_NtCompareTokens(p, imp, &eq), 0);
    CHECK(!eq);
    // thread impersonation round trip
    CHECK_EQ(tok::SetThreadImpersonationToken((HANDLE)(intptr_t)-2, imp2), kAccessDenied);   // imp2 lacks TOKEN_IMPERSONATE
    CHECK_EQ(tok::SetThreadImpersonationToken((HANDLE)(intptr_t)-2, imp), 0);
    HANDLE th = nullptr;
    CHECK_EQ(tok::Tok_NtOpenThreadToken((HANDLE)(intptr_t)-2, TOKEN_QUERY, TRUE, &th), 0);
    CHECK_EQ(Query(th, TType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 2u);
    tok::Close(th);
    CHECK_EQ(tok::Tok_NtImpersonateAnonymousToken((HANDLE)(intptr_t)-2), 0);
    CHECK_EQ(tok::Tok_NtOpenThreadToken((HANDLE)(intptr_t)-2, TOKEN_QUERY, TRUE, &th), 0);
    CHECK_EQ(Query(th, TUser, b), 0);
    CHECK(Str((const void*)(std::uintptr_t)U<std::uint64_t>(b)) == L"S-1-5-7");
    tok::Close(th);
    CHECK_EQ(tok::SetThreadImpersonationToken((HANDLE)(intptr_t)-2, nullptr), 0);
    CHECK_EQ(tok::Tok_NtOpenThreadToken((HANDLE)(intptr_t)-2, TOKEN_QUERY, TRUE, &th), 0);
    CHECK_EQ(Query(th, TType, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);   // back to the process token
    tok::Close(th);
    tok::Close(p); tok::Close(imp); tok::Close(imp2);
    ResetIdentity();
}

TEST(token_filter_token) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    std::vector<std::uint8_t> users;
    CHECK(tok::SidFromString(L"S-1-5-32-545", &users));
    struct { DWORD n; SID_AND_ATTRIBUTES a[1]; } dis{};
    dis.n = 1; dis.a[0].Sid = users.data();
    HANDLE f = nullptr;
    CHECK_EQ(tok::Tok_NtFilterToken(p, 1 /*DISABLE_MAX_PRIVILEGE*/, (PTOKEN_GROUPS)&dis, nullptr, nullptr, &f), 0);
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(f, TGroups, b), 0);
    CHECK_EQ(FindGroup(b, L"S-1-5-32-545"), (long long)(SE_GROUP_USE_FOR_DENY_ONLY | SE_GROUP_MANDATORY));
    CHECK_EQ(Query(f, TPrivileges, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);   // only SeChangeNotify survives
    BOOLEAN eq = TRUE;
    CHECK_EQ(tok::Tok_NtCompareTokens(p, f, &eq), 0);
    CHECK(!eq);
    tok::Close(f); tok::Close(p);
}

TEST(token_privilege_check) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    struct { DWORD n; DWORD control; LUID_AND_ATTRIBUTES a[2]; } ps{};
    ps.n = 1; ps.control = 1;
    ps.a[0].Luid.LowPart = 23;   // SeChangeNotify: enabled
    BOOLEAN r = FALSE;
    CHECK_EQ(tok::Tok_NtPrivilegeCheck(p, (PPRIVILEGE_SET)&ps, &r), 0);
    CHECK(r);
    CHECK(ps.a[0].Attributes & SE_PRIVILEGE_USED_FOR_ACCESS);
    ps.a[0].Luid.LowPart = 19;   // SeShutdown: held but disabled
    CHECK_EQ(tok::Tok_NtPrivilegeCheck(p, (PPRIVILEGE_SET)&ps, &r), 0);
    CHECK(!r);
    ps.n = 2; ps.a[0].Luid.LowPart = 23; ps.a[1].Luid.LowPart = 19;
    CHECK_EQ(tok::Tok_NtPrivilegeCheck(p, (PPRIVILEGE_SET)&ps, &r), 0);
    CHECK(!r);                 // ALL_NECESSARY
    ps.control = 0;
    CHECK_EQ(tok::Tok_NtPrivilegeCheck(p, (PPRIVILEGE_SET)&ps, &r), 0);
    CHECK(r);                  // any
    tok::Close(p);
}

namespace {
std::vector<std::uint8_t> MakeAcl(std::initializer_list<std::pair<std::uint8_t /*type*/, std::pair<std::uint32_t /*mask*/, std::wstring>>> aces) {
    std::vector<std::uint8_t> a(8, 0);
    a[0] = 2;
    std::uint16_t n = 0;
    for (auto& e : aces) {
        std::vector<std::uint8_t> sid;
        tok::SidFromString(e.second.second, &sid);
        std::uint16_t size = (std::uint16_t)(8 + sid.size());
        size_t o = a.size();
        a.resize(o + size);
        a[o] = e.first;
        std::memcpy(&a[o + 2], &size, 2);
        std::memcpy(&a[o + 4], &e.second.first, 4);
        std::memcpy(&a[o + 8], sid.data(), sid.size());
        ++n;
    }
    std::uint16_t total = (std::uint16_t)a.size();
    std::memcpy(&a[2], &total, 2);
    std::memcpy(&a[4], &n, 2);
    return a;
}
}  // namespace

TEST(token_access_check_dacl_evaluation) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    HANDLE imp = nullptr;
    CHECK_EQ(tok::Tok_NtDuplicateToken(p, TOKEN_ALL_ACCESS, nullptr, FALSE, TokenImpersonation, &imp), 0);
    GENERIC_MAPPING gm = {0x1, 0x2, 0x4, 0x7};   // read=1 write=2 exec=4 all=7
    std::vector<std::uint8_t> user, owner;
    CHECK(tok::SidFromString(tok::UserSidString(), &user));

    auto check = [&](const std::vector<std::uint8_t>* acl, ACCESS_MASK want, ACCESS_MASK* granted, NTSTATUS* as) {
        SECURITY_DESCRIPTOR sd{};
        sd.Revision = 1;
        sd.Owner = (PSID)user.data();
        sd.Group = (PSID)user.data();
        if (acl) { sd.Control |= SE_DACL_PRESENT; sd.Dacl = (PACL)acl->data(); }
        else sd.Control |= SE_DACL_PRESENT;   // NULL DACL
        NTSTATUS st = tok::Tok_NtAccessCheck(&sd, imp, want, &gm, nullptr, nullptr, granted, as);
        CHECK_EQ(st, 0);
    };
    ACCESS_MASK g = 0xFFFF;
    NTSTATUS as = -1;

    check(nullptr, 7, &g, &as);                       // NULL DACL grants everything
    CHECK_EQ(as, 0); CHECK_EQ(g, 7u);

    auto allow_user = MakeAcl({{0, {0x3, tok::UserSidString()}}});
    check(&allow_user, 0x1, &g, &as);
    CHECK_EQ(as, 0); CHECK_EQ(g, 0x1u);
    check(&allow_user, 0x7, &g, &as);                 // 0x4 not granted
    CHECK_EQ(as, kAccessDenied); CHECK_EQ(g, 0u);
    check(&allow_user, MAXIMUM_ALLOWED, &g, &as);
    CHECK_EQ(as, 0); CHECK_EQ(g, 0x3u | READ_CONTROL | WRITE_DAC);   // owner rights are implicit

    auto admins_only = MakeAcl({{0, {0x7, L"S-1-5-32-544"}}});
    check(&admins_only, 0x1, &g, &as);
    CHECK_EQ(as, kAccessDenied);

    auto users_group = MakeAcl({{0, {0x7, L"S-1-5-32-545"}}});
    check(&users_group, 0x4, &g, &as);
    CHECK_EQ(as, 0);

    auto deny_first = MakeAcl({{1, {0x2, L"S-1-1-0"}}, {0, {0x7, L"S-1-1-0"}}});
    check(&deny_first, 0x1, &g, &as);
    CHECK_EQ(as, 0);
    check(&deny_first, 0x2, &g, &as);
    CHECK_EQ(as, kAccessDenied);
    check(&deny_first, MAXIMUM_ALLOWED, &g, &as);
    CHECK_EQ(g & 0x7, 0x5u);

    auto generic = MakeAcl({{0, {GENERIC_READ, L"S-1-5-11"}}});
    check(&generic, 0x1, &g, &as);                    // generic bit in the ACE is mapped
    CHECK_EQ(as, 0);

    // The token's own SD (via NtQuerySecurityObject) grants its owner full access.
    ULONG need = 0;
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(p, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, 0, &need), kBufTooSmall);
    std::vector<std::uint8_t> sdb(need);
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(p, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sdb.data(), need, &need), 0);
    GENERIC_MAPPING tm = {TOKEN_READ, TOKEN_WRITE, TOKEN_EXECUTE, TOKEN_ALL_ACCESS};
    CHECK_EQ(tok::Tok_NtAccessCheck(sdb.data(), imp, TOKEN_QUERY | TOKEN_DUPLICATE, &tm, nullptr, nullptr, &g, &as), 0);
    CHECK_EQ(as, 0);
    CHECK_EQ(g, (ACCESS_MASK)(TOKEN_QUERY | TOKEN_DUPLICATE));

    tok::Close(imp); tok::Close(p);
}

TEST(token_query_security_object_layout) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    ULONG need = 0;
    SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(p, si, nullptr, 0, &need), kBufTooSmall);
    std::vector<std::uint8_t> sd(need);
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(p, si, sd.data(), need, &need), 0);
    CHECK_EQ(sd[0], 1);
    WORD ctl = *(WORD*)&sd[2];
    CHECK(ctl & SE_SELF_RELATIVE);
    CHECK(ctl & SE_DACL_PRESENT);
    CHECK(ctl & SE_SACL_PRESENT);
    DWORD oo = *(DWORD*)&sd[4], go = *(DWORD*)&sd[8], so = *(DWORD*)&sd[12], d = *(DWORD*)&sd[16];
    CHECK(oo >= 20 && go >= 20 && so >= 20 && d >= 20 && oo < need && go < need);
    CHECK(Str(&sd[oo]) == tok::UserSidString());
    CHECK_EQ(sd[d], 2);                           // ACL_REVISION
    CHECK_EQ(*(WORD*)&sd[d + 4], 2);              // user + SYSTEM
    CHECK_EQ(sd[so + 8], 0x11);                   // SYSTEM_MANDATORY_LABEL_ACE
    CHECK(Str(&sd[so + 16]) == L"S-1-16-8192");
    // The sections do not overlap and end inside the buffer.
    CHECK(so + *(WORD*)&sd[so + 2] <= need && d + *(WORD*)&sd[d + 2] <= need);
    // SACL needs ACCESS_SYSTEM_SECURITY; READ_CONTROL is required for the rest.
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(p, SACL_SECURITY_INFORMATION, sd.data(), need, &need), kAccessDenied);
    tok::Close(p);
    HANDLE noc = OpenPrimary(TOKEN_QUERY);
    CHECK_EQ(tok::Tok_NtQuerySecurityObject(noc, DACL_SECURITY_INFORMATION, sd.data(), (ULONG)sd.size(), &need), kAccessDenied);
    tok::Close(noc);
}

TEST(token_set_information) {
    ResetIdentity();
    HANDLE p = OpenPrimary();
    std::vector<std::uint8_t> sidv;
    CHECK(tok::SidFromString(L"S-1-5-32-544", &sidv));
    struct { PSID sid; } owner{(PSID)sidv.data()};
    CHECK(tok::Tok_NtSetInformationToken(p, (TOKEN_INFORMATION_CLASS)TOwner, &owner, sizeof owner) != 0);   // not in token
    std::vector<std::uint8_t> me;
    CHECK(tok::SidFromString(tok::UserSidString(), &me));
    owner.sid = (PSID)me.data();
    CHECK_EQ(tok::Tok_NtSetInformationToken(p, (TOKEN_INFORMATION_CLASS)TOwner, &owner, sizeof owner), 0);
    ULONG sess = 5;
    CHECK(tok::Tok_NtSetInformationToken(p, (TOKEN_INFORMATION_CLASS)12, &sess, 4) != 0);   // needs TCB
    ULONG virt = 1;
    CHECK_EQ(tok::Tok_NtSetInformationToken(p, (TOKEN_INFORMATION_CLASS)TVirtEnabled, &virt, 4), 0);
    std::vector<std::uint8_t> b;
    CHECK_EQ(Query(p, TVirtEnabled, b), 0);
    CHECK_EQ(U<std::uint32_t>(b), 1u);
    // raising integrity is refused
    std::vector<std::uint8_t> high;
    CHECK(tok::SidFromString(L"S-1-16-12288", &high));
    struct { PSID sid; DWORD attr; } il{(PSID)high.data(), 0x60};
    CHECK(tok::Tok_NtSetInformationToken(p, (TOKEN_INFORMATION_CLASS)TIntegrity, &il, sizeof il) != 0);
    tok::Close(p);
    ResetIdentity();
}

TEST(token_sid_string_roundtrip) {
    std::vector<std::uint8_t> s;
    CHECK(tok::SidFromString(L"S-1-5-21-1-2-3-500", &s));
    CHECK(tok::SidToString(s.data()) == L"S-1-5-21-1-2-3-500");
    CHECK_EQ(s.size(), (size_t)(8 + 4 * 5));
    CHECK(!tok::SidFromString(L"S-2-5-1", &s));
    CHECK(!tok::SidFromString(L"S-1-5-x", &s));
    CHECK(!tok::SidFromString(L"", &s));
    CHECK(tok::SidFromString(L"S-1-5-18", &s));
    CHECK_EQ(s.size(), 12u);
    CHECK_EQ(s[7], 5);
}
