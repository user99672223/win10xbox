// win10xbox token / identity emulation. See core/include/win10xbox/token.h.
//
// The guest sees one interactive, medium-integrity, non-elevated user token
// (session 1, not an AppContainer) built from machine.json "identity". Host
// tokens are never touched or exposed.
//
// Design
// ------
//  * A `TokenObj` is the in-memory token (SIDs, groups, privileges, ...). Many
//    guest handles can reference one object, exactly like NT: a handle table
//    maps TokenId (an opaque per-handle id) -> {shared TokenObj, granted
//    access mask}. NtOpenProcessToken therefore returns a new handle each
//    time and closing one never invalidates another, while NtAdjustPrivileges
//    through any of them mutates the same process token.
//  * Handle values are tagged ids ((0x544B << 48) | id << 2), so IsTokenHandle()
//    can recognise them without a lookup; the three NT pseudo token handles
//    (-4 process, -5 thread, -6 effective) are understood as well.
//  * All SID / ACL / SECURITY_DESCRIPTOR / TOKEN_* byte layouts are built BY HAND
//    here (no advapi32/ntdll Rtl* calls): they are small, fixed, and the output
//    must not depend on the host's token state. Where NT returns structures that
//    contain pointers (TOKEN_USER, TOKEN_GROUPS, ...), the pointers point into
//    the caller's own buffer, as the kernel does.
//  * One SRWLOCK protects every table and every token.
#include "win10xbox/token.h"
#include "win10xbox/probe.h"
#include "win10xbox/trace.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace win10xbox::token {

namespace {

// ---- status codes (local constants: avoid ntstatus.h / winnt.h macro clashes) --
constexpr NTSTATUS kSuccess                 = 0;
constexpr NTSTATUS kNotAllAssigned          = static_cast<NTSTATUS>(0x00000106);
constexpr NTSTATUS kInvalidInfoClass        = static_cast<NTSTATUS>(0xC0000003);
constexpr NTSTATUS kInfoLengthMismatch      = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS kAccessViolation         = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kInvalidHandle           = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS kInvalidParameter        = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kNotImplemented          = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kNotSupported            = static_cast<NTSTATUS>(0xC00000BB);
constexpr NTSTATUS kAccessDenied            = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kBufferTooSmall          = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS kObjectTypeMismatch      = static_cast<NTSTATUS>(0xC0000024);
constexpr NTSTATUS kPrivilegeNotHeld        = static_cast<NTSTATUS>(0xC0000061);
constexpr NTSTATUS kInvalidSid              = static_cast<NTSTATUS>(0xC0000078);
constexpr NTSTATUS kInvalidSecurityDescr    = static_cast<NTSTATUS>(0xC0000079);
constexpr NTSTATUS kCantDisableMandatory    = static_cast<NTSTATUS>(0xC000005D);
constexpr NTSTATUS kNoSuchLogonSession      = static_cast<NTSTATUS>(0xC000005F);
constexpr NTSTATUS kBadImpersonationLevel   = static_cast<NTSTATUS>(0xC00000A5);
constexpr NTSTATUS kNoToken                 = static_cast<NTSTATUS>(0xC000007C);
constexpr NTSTATUS kInvalidOwner            = static_cast<NTSTATUS>(0xC000005A);
constexpr NTSTATUS kInvalidPrimaryGroup     = static_cast<NTSTATUS>(0xC000005B);

// ---- access rights -----------------------------------------------------------
constexpr ACCESS_MASK kTokenAllAccess = 0x000F01FF;   // STANDARD_RIGHTS_REQUIRED | all TOKEN_* specific rights
constexpr ACCESS_MASK kMaximumAllowed = 0x02000000;
constexpr ACCESS_MASK kGenericRead    = 0x80000000;
constexpr ACCESS_MASK kGenericWrite   = 0x40000000;
constexpr ACCESS_MASK kGenericExecute = 0x20000000;
constexpr ACCESS_MASK kGenericAll     = 0x10000000;
constexpr ACCESS_MASK kReadControl    = 0x00020000;
constexpr ACCESS_MASK kWriteDac       = 0x00040000;
constexpr ACCESS_MASK kAccessSystemSecurity = 0x01000000;
constexpr ACCESS_MASK kTokDuplicate = 0x0002, kTokImpersonate = 0x0004,
                      kTokQuery = 0x0008, kTokQuerySource = 0x0010, kTokAdjustPrivileges = 0x0020,
                      kTokAdjustGroups = 0x0040, kTokAdjustDefault = 0x0080, kTokAdjustSessionId = 0x0100;

// SE_GROUP_* / SE_PRIVILEGE_*
constexpr std::uint32_t kGrpMandatory = 0x1, kGrpEnabledByDefault = 0x2, kGrpEnabled = 0x4, kGrpOwner = 0x8,
                        kGrpUseForDenyOnly = 0x10, kGrpIntegrity = 0x20, kGrpIntegrityEnabled = 0x40,
                        kGrpLogonId = 0xC0000000;
constexpr std::uint32_t kGrpStd = kGrpMandatory | kGrpEnabledByDefault | kGrpEnabled;   // 7
constexpr std::uint32_t kPrvEnabledByDefault = 0x1, kPrvEnabled = 0x2, kPrvRemoved = 0x4, kPrvUsedForAccess = 0x80000000;

constexpr std::uint32_t kTokenPrimary = 1, kTokenImpersonation = 2;
constexpr std::uint32_t kSecAnonymous = 0, kSecImpersonation = 2, kSecDelegation = 3;
constexpr std::uint32_t kElevDefault = 1, kElevFull = 2, kElevLimited = 3;

// TOKEN_INFORMATION_CLASS values (winnt.h lacks the newer ones on older SDKs).
enum Cls : ULONG {
    cUser = 1, cGroups, cPrivileges, cOwner, cPrimaryGroup, cDefaultDacl, cSource, cType, cImpersonationLevel,
    cStatistics, cRestrictedSids, cSessionId, cGroupsAndPrivileges, cSessionReference, cSandBoxInert, cAuditPolicy,
    cOrigin, cElevationType, cLinkedToken, cElevation, cHasRestrictions, cAccessInformation,
    cVirtualizationAllowed, cVirtualizationEnabled, cIntegrityLevel, cUIAccess, cMandatoryPolicy, cLogonSid,
    cIsAppContainer, cCapabilities, cAppContainerSid, cAppContainerNumber, cUserClaimAttributes,
    cDeviceClaimAttributes, cRestrictedUserClaimAttributes, cRestrictedDeviceClaimAttributes, cDeviceGroups,
    cRestrictedDeviceGroups, cSecurityAttributes, cIsRestricted, cProcessTrustLevel, cPrivateNameSpace,
    cSingletonAttributes, cBnoIsolation, cChildProcessFlags, cIsLessPrivilegedAppContainer, cIsSandboxed,
    cIsAppSilo, cClsMax
};

// ---- SIDs --------------------------------------------------------------------
using Sid = std::vector<std::uint8_t>;

// Layout: Revision(1)=1, SubAuthorityCount(1), IdentifierAuthority[6] big-endian, SubAuthority[n] LE DWORDs.
Sid MakeSid(std::uint64_t authority, std::initializer_list<std::uint32_t> subs) {
    Sid s(8 + 4 * subs.size());
    s[0] = 1;
    s[1] = static_cast<std::uint8_t>(subs.size());
    for (int i = 0; i < 6; ++i) s[2 + i] = static_cast<std::uint8_t>(authority >> (8 * (5 - i)));
    size_t i = 0;
    for (std::uint32_t v : subs) std::memcpy(&s[8 + 4 * i++], &v, 4);
    return s;
}

size_t SidLen(const void* p) { return 8 + 4 * static_cast<size_t>(static_cast<const std::uint8_t*>(p)[1]); }

bool SidValid(const void* p) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return p && b[0] == 1 && b[1] <= 15;
}

bool SidEq(const void* a, const void* b) {
    return SidValid(a) && SidValid(b) && SidLen(a) == SidLen(b) && std::memcmp(a, b, SidLen(a)) == 0;
}
bool SidEq(const Sid& a, const void* b) { return !a.empty() && SidEq(a.data(), b); }
bool SidEq(const Sid& a, const Sid& b) { return !a.empty() && !b.empty() && SidEq(a.data(), b.data()); }

Sid CopySid(const void* p) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return Sid(b, b + SidLen(p));
}

// ---- caller memory -----------------------------------------------------------------
// Everything a token call reads from the caller is copied once, here, through win10xbox/probe.h; the rest of the
// module works on the copies. A pointer that cannot be read is STATUS_ACCESS_VIOLATION, as in the kernel.

constexpr ULONG kMaxCapturedEntries = 0x1000;   // SE_MAXIMUM_GROUP_LIMIT: more is STATUS_INVALID_PARAMETER

// A SID, copied whole (the length comes from its sub-authority count). Only readability is checked here:
// SidValid says whether the copy is a SID.
NTSTATUS CaptureSid(const void* user, Sid* out) {
    std::uint8_t head[2];
    if (NTSTATUS st = probe::CopyFromUser(head, user, sizeof head)) return st;
    out->assign(8 + 4 * static_cast<size_t>(head[1]), 0);
    return probe::CopyFromUser(out->data(), user, out->size());
}

// TOKEN_GROUPS / TOKEN_PRIVILEGES: the count, then the entries.
template <class Entry>
NTSTATUS CaptureArray(const void* user, size_t first_entry_offset, std::vector<Entry>* out) {
    ULONG count;
    if (NTSTATUS st = probe::CopyFromUser(&count, user, sizeof count)) return st;
    if (count > kMaxCapturedEntries) return kInvalidParameter;
    out->resize(count);
    return probe::CopyFromUser(out->data(), static_cast<const std::uint8_t*>(user) + first_entry_offset,
                               count * sizeof(Entry));
}

bool ParseSid(const std::wstring& str, Sid* out) {
    // "S-R-A-S1-S2-..."
    if (str.size() < 5 || (str[0] != L'S' && str[0] != L's') || str[1] != L'-') return false;
    std::vector<std::uint64_t> parts;
    size_t pos = 2;
    while (pos <= str.size()) {
        size_t dash = str.find(L'-', pos);
        std::wstring tok = str.substr(pos, dash == std::wstring::npos ? std::wstring::npos : dash - pos);
        if (tok.empty()) return false;
        wchar_t* end = nullptr;
        const bool hex = tok.size() > 2 && tok[0] == L'0' && (tok[1] == L'x' || tok[1] == L'X');
        unsigned long long v = std::wcstoull(tok.c_str(), &end, hex ? 16 : 10);
        if (!end || *end != 0) return false;
        parts.push_back(v);
        if (dash == std::wstring::npos) break;
        pos = dash + 1;
    }
    if (parts.size() < 3 || parts[0] != 1 || parts[1] >= (1ull << 48) || parts.size() - 2 > 16) return false;
    const size_t nsub = parts.size() - 2;
    if (nsub > 15) return false;
    Sid s(8 + 4 * nsub);
    s[0] = 1;
    s[1] = static_cast<std::uint8_t>(nsub);
    for (int i = 0; i < 6; ++i) s[2 + i] = static_cast<std::uint8_t>(parts[1] >> (8 * (5 - i)));
    for (size_t i = 0; i < nsub; ++i) {
        if (parts[2 + i] > 0xFFFFFFFFull) return false;
        const std::uint32_t v = static_cast<std::uint32_t>(parts[2 + i]);
        std::memcpy(&s[8 + 4 * i], &v, 4);
    }
    *out = std::move(s);
    return true;
}

std::wstring SidToStr(const void* p) {
    if (!SidValid(p)) return L"";
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::uint64_t auth = 0;
    for (int i = 0; i < 6; ++i) auth = (auth << 8) | b[2 + i];
    std::wstring r = L"S-" + std::to_wstring(b[0]) + L"-" + std::to_wstring(auth);
    for (int i = 0; i < b[1]; ++i) {
        std::uint32_t v;
        std::memcpy(&v, b + 8 + 4 * i, 4);
        r += L"-" + std::to_wstring(v);
    }
    return r;
}

// Well-known SIDs.
Sid SidEveryone()      { return MakeSid(1, {0}); }            // S-1-1-0
Sid SidLocal()         { return MakeSid(2, {0}); }            // S-1-2-0
Sid SidConsoleLogon()  { return MakeSid(2, {1}); }            // S-1-2-1
Sid SidInteractive()   { return MakeSid(5, {4}); }            // S-1-5-4
Sid SidAnonymous()     { return MakeSid(5, {7}); }            // S-1-5-7
Sid SidAuthUsers()     { return MakeSid(5, {11}); }           // S-1-5-11
Sid SidThisOrg()       { return MakeSid(5, {15}); }           // S-1-5-15
Sid SidSystem()        { return MakeSid(5, {18}); }           // S-1-5-18
Sid SidBuiltinAdmins() { return MakeSid(5, {32, 544}); }      // S-1-5-32-544
Sid SidBuiltinUsers()  { return MakeSid(5, {32, 545}); }      // S-1-5-32-545
Sid SidIntegrity(std::uint32_t rid) { return MakeSid(16, {rid}); }   // S-1-16-rid

// ---- byte blobs with pointer fix-ups -----------------------------------------
// NT returns structures whose pointers point into the same user buffer. Build the
// image with offsets, record each pointer slot, then Emit() into the real buffer.
struct Blob {
    std::vector<std::uint8_t> d;
    std::vector<std::pair<size_t, size_t>> fix;   // (slot offset, target offset)
    size_t Reserve(size_t n) { size_t o = d.size(); d.resize(o + n, 0); return o; }
    size_t Append(const void* p, size_t n) { size_t o = Reserve(n); std::memcpy(&d[o], p, n); return o; }
    size_t Append(const Sid& s) { return Append(s.data(), s.size()); }
    void Align(size_t a) { while (d.size() % a) d.push_back(0); }
    void Put32(size_t at, std::uint32_t v) { std::memcpy(&d[at], &v, 4); }
    void Put64(size_t at, std::uint64_t v) { std::memcpy(&d[at], &v, 8); }
    void Ptr(size_t slot, size_t target) { fix.emplace_back(slot, target); }
    size_t size() const { return d.size(); }
    // Relocates the image for `dst` and copies it into the caller's buffer.
    NTSTATUS Emit(void* dst) const {
        std::vector<std::uint8_t> image(d);
        for (const auto& f : fix) {
            const std::uint64_t p = reinterpret_cast<std::uintptr_t>(dst) + f.second;
            std::memcpy(&image[f.first], &p, 8);
        }
        return probe::CopyToUser(dst, image.data(), image.size());
    }
};

// ---- token object ------------------------------------------------------------
struct Group { Sid sid; std::uint32_t attrs; };
struct Priv  { LUID luid; std::uint32_t attrs; };

struct TokenObj {
    std::uint32_t type = kTokenPrimary;
    std::uint32_t imp_level = kSecAnonymous;
    LUID          token_id{}, auth_id{}, modified_id{}, source_luid{};
    char          source_name[8] = {'U', 's', 'e', 'r', '3', '2', ' ', ' '};
    Sid           user;
    std::vector<Group> groups;       // without the integrity label
    std::vector<Group> restricted;
    std::vector<Priv>  privs;
    Sid           owner, primary_group;
    std::vector<std::uint8_t> default_dacl;   // an ACL
    std::uint32_t session = 1;
    std::uint32_t integrity_rid = 0x2000;
    std::uint32_t elevation_type = kElevLimited;
    std::uint32_t virt_allowed = 1, virt_enabled = 0, ui_access = 0, mandatory_policy = 3;
    bool          elevated = false;
};
using TokPtr = std::shared_ptr<TokenObj>;

struct HandleEnt { TokPtr tok; ACCESS_MASK access; bool inherit; bool permanent; };

// Handle values fit in 31 bits (guest code often truncates HANDLEs to 32 bits):
// 0x54000000 | (id << 2). Ids are 22 bits, far more than a process ever opens.
constexpr std::uintptr_t kHandleBase = 0x54000000u;
constexpr std::uintptr_t kHandleMask = 0xFF000000u;
HANDLE const kPseudoProcessToken   = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4));
HANDLE const kPseudoThreadToken    = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-5));
HANDLE const kPseudoEffectiveToken = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-6));

SRWLOCK g_lock = SRWLOCK_INIT;
struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

Identity g_ident;
bool g_ready = false;
TokPtr g_primary;
TokenId g_primary_id = 0;
std::unordered_map<TokenId, HandleEnt> g_handles;
std::unordered_map<DWORD, TokPtr> g_thread_tokens;
std::uint64_t g_next_handle = 0x40;       // handle ids; value = tag | id << 2
std::uint64_t g_next_luid = 0x1D8C00;

LUID NextLuid() {
    const std::uint64_t v = g_next_luid++;
    LUID l;
    l.LowPart = static_cast<DWORD>(v);
    l.HighPart = static_cast<LONG>(v >> 32);
    return l;
}

// Privilege LUIDs (SE_*_PRIVILEGE numeric ids).
enum : DWORD {
    pSecurity = 8, pTakeOwnership = 9, pLoadDriver = 10, pSystemProfile = 11, pSystemtime = 12,
    pProfSingleProcess = 13, pIncBasePriority = 14, pCreatePagefile = 15, pBackup = 17, pRestore = 18,
    pShutdown = 19, pDebug = 20, pSystemEnvironment = 22, pChangeNotify = 23, pRemoteShutdown = 24,
    pUndock = 25, pManageVolume = 28, pImpersonate = 29, pCreateGlobal = 30, pIncWorkingSet = 33,
    pTimeZone = 34, pCreateSymbolicLink = 35, pDelegateSessionUserImpersonate = 36
};
Priv MkPriv(DWORD id, std::uint32_t attrs) {
    Priv p;
    p.luid.LowPart = id;
    p.luid.HighPart = 0;
    p.attrs = attrs;
    return p;
}
bool LuidEq(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// ---- ACL / SD builders ---------------------------------------------------------
void AddAllowAce(std::vector<std::uint8_t>& acl, std::uint32_t mask, const Sid& sid) {
    const std::uint16_t size = static_cast<std::uint16_t>(8 + sid.size());
    const size_t o = acl.size();
    acl.resize(o + size);
    acl[o] = 0;     // ACCESS_ALLOWED_ACE_TYPE
    acl[o + 1] = 0; // flags
    std::memcpy(&acl[o + 2], &size, 2);
    std::memcpy(&acl[o + 4], &mask, 4);
    std::memcpy(&acl[o + 8], sid.data(), sid.size());
    // header: AclRevision(1)=2, Sbz1, AclSize(2), AceCount(2), Sbz2(2)
    std::uint16_t cnt;
    std::memcpy(&cnt, &acl[4], 2);
    ++cnt;
    std::memcpy(&acl[4], &cnt, 2);
    const std::uint16_t total = static_cast<std::uint16_t>(acl.size());
    std::memcpy(&acl[2], &total, 2);
}

std::vector<std::uint8_t> NewAcl() {
    std::vector<std::uint8_t> a(8, 0);
    a[0] = 2;   // ACL_REVISION
    a[2] = 8;
    return a;
}

// ---- token construction ------------------------------------------------------------
void RecomputeDefaults(TokenObj& t) {
    t.default_dacl = NewAcl();
    AddAllowAce(t.default_dacl, 0x10000000, SidSystem());                       // GENERIC_ALL
    AddAllowAce(t.default_dacl, 0x10000000, t.elevated ? SidBuiltinAdmins() : t.user);
}

bool BuildPrimary(const Identity& id, TokenObj* t, NTSTATUS* st) {
    Sid user;
    if (!ParseSid(id.user_sid, &user) || user.size() < 12) {
        *st = kInvalidSid;
        return false;
    }
    t->type = kTokenPrimary;
    t->imp_level = kSecAnonymous;
    t->token_id = NextLuid();
    t->modified_id = NextLuid();
    t->source_luid = NextLuid();
    t->auth_id.LowPart = 0x0001F6F2;   // interactive logon session
    t->auth_id.HighPart = 0;
    t->user = user;
    t->session = id.session_id;
    t->elevated = id.elevated;
    t->elevation_type = id.elevated ? kElevFull : kElevLimited;
    t->integrity_rid = id.integrity_rid;
    if (id.elevated && t->integrity_rid < 0x3000) t->integrity_rid = 0x3000;   // High
    t->groups.clear();
    t->groups.push_back({SidEveryone(), kGrpStd});
    if (id.elevated) t->groups.push_back({SidBuiltinAdmins(), kGrpStd | kGrpOwner});
    t->groups.push_back({SidBuiltinUsers(), kGrpStd});
    t->groups.push_back({SidInteractive(), kGrpStd});
    t->groups.push_back({SidConsoleLogon(), kGrpStd});
    t->groups.push_back({SidAuthUsers(), kGrpStd});
    t->groups.push_back({SidThisOrg(), kGrpStd});
    t->groups.push_back({SidLocal(), kGrpStd});
    t->groups.push_back({MakeSid(5, {5, 0u, t->auth_id.LowPart}), kGrpStd | kGrpLogonId});   // S-1-5-5-X-Y logon session
    // Privileges of a standard (filtered) user; an elevated token has the full admin set.
    t->privs.clear();
    if (id.elevated) {
        const DWORD all[] = {pSecurity, pTakeOwnership, pLoadDriver, pSystemProfile, pSystemtime, pProfSingleProcess,
                             pIncBasePriority, pCreatePagefile, pBackup, pRestore, pShutdown, pDebug,
                             pSystemEnvironment, pChangeNotify, pRemoteShutdown, pUndock, pManageVolume,
                             pImpersonate, pCreateGlobal, pIncWorkingSet, pTimeZone, pCreateSymbolicLink,
                             pDelegateSessionUserImpersonate};
        for (DWORD p : all) {
            const bool on = (p == pChangeNotify || p == pImpersonate || p == pCreateGlobal);
            t->privs.push_back(MkPriv(p, on ? (kPrvEnabledByDefault | kPrvEnabled) : 0));
        }
    } else {
        t->privs.push_back(MkPriv(pShutdown, 0));
        t->privs.push_back(MkPriv(pChangeNotify, kPrvEnabledByDefault | kPrvEnabled));
        t->privs.push_back(MkPriv(pUndock, 0));
        t->privs.push_back(MkPriv(pIncWorkingSet, 0));
        t->privs.push_back(MkPriv(pTimeZone, 0));
    }
    t->owner = id.elevated ? SidBuiltinAdmins() : user;
    // Primary group: <domain>-513 ("None" / Domain Users) for S-1-5-21-* accounts, else Users.
    std::uint32_t sub0 = 0;
    std::memcpy(&sub0, &user[8], 4);
    if (user[1] == 5 && user[7] == 5 && sub0 == 21) {
        t->primary_group = user;
        const std::uint32_t rid = 513;
        std::memcpy(&t->primary_group[t->primary_group.size() - 4], &rid, 4);
    } else {
        t->primary_group = SidBuiltinUsers();
    }
    t->virt_allowed = 1;
    t->virt_enabled = 0;
    t->ui_access = 0;
    t->mandatory_policy = 3;   // NO_WRITE_UP | NEW_PROCESS_MIN
    RecomputeDefaults(*t);
    return true;
}

TokPtr MakeAnonymous() {
    auto t = std::make_shared<TokenObj>();
    t->type = kTokenImpersonation;
    t->imp_level = kSecImpersonation;
    t->token_id = NextLuid();
    t->modified_id = NextLuid();
    t->source_luid = NextLuid();
    t->auth_id.LowPart = 0x3E6;   // ANONYMOUS LOGON session
    t->user = SidAnonymous();
    t->groups.push_back({SidEveryone(), kGrpStd});
    t->groups.push_back({SidAnonymous(), kGrpStd});
    t->privs.push_back(MkPriv(pChangeNotify, kPrvEnabledByDefault | kPrvEnabled));
    t->owner = t->user;
    t->primary_group = SidAnonymous();
    t->session = g_ident.session_id;
    t->integrity_rid = 0;   // Untrusted
    t->elevation_type = kElevDefault;
    t->virt_allowed = 0;
    RecomputeDefaults(*t);
    return t;
}

TokenId AddHandleLocked(const TokPtr& t, ACCESS_MASK access, bool inherit, bool permanent = false) {
    const TokenId id = g_next_handle++;
    g_handles[id] = HandleEnt{t, access, inherit, permanent};
    return id;
}

HANDLE HandleValue(TokenId id) {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(kHandleBase | ((id << 2) & ~kHandleMask)));
}

NTSTATUS InitializeLocked(const Identity& id) {
    auto t = std::make_shared<TokenObj>();
    NTSTATUS st = kSuccess;
    if (!BuildPrimary(id, t.get(), &st)) return st;
    g_ident = id;
    g_handles.clear();
    g_thread_tokens.clear();
    g_primary = t;
    g_primary_id = AddHandleLocked(t, kTokenAllAccess, false, true);
    g_ready = true;
    return kSuccess;
}

void EnsureReady() {
    Lock l;
    if (!g_ready) InitializeLocked(Identity{});
}

ACCESS_MASK MapTokenAccess(ACCESS_MASK a) {
    if (a & kMaximumAllowed) a = (a & ~kMaximumAllowed) | kTokenAllAccess;
    if (a & kGenericRead)    a = (a & ~kGenericRead)    | kReadControl | kTokQuery;
    if (a & kGenericWrite)   a = (a & ~kGenericWrite)   | kReadControl | kTokAdjustPrivileges | kTokAdjustGroups | kTokAdjustDefault;
    if (a & kGenericExecute) a = (a & ~kGenericExecute) | kReadControl;
    if (a & kGenericAll)     a = (a & ~kGenericAll)     | kTokenAllAccess;
    return a;
}

// ---- handle resolution (lock held) -----------------------------------------------
DWORD ThreadIdOf(HANDLE thread) {
    if (thread == reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-2)) || thread == nullptr) return GetCurrentThreadId();
    return GetThreadId(thread);   // host handle; 0 on failure
}

TokPtr EffectiveThreadToken() {
    auto it = g_thread_tokens.find(GetCurrentThreadId());
    return it != g_thread_tokens.end() ? it->second : g_primary;
}

NTSTATUS Resolve(HANDLE h, ACCESS_MASK need, TokPtr* out, ACCESS_MASK* granted = nullptr) {
    if (h == kPseudoProcessToken) { *out = g_primary; if (granted) *granted = kTokenAllAccess; return kSuccess; }
    if (h == kPseudoEffectiveToken) { *out = EffectiveThreadToken(); if (granted) *granted = kTokenAllAccess; return kSuccess; }
    if (h == kPseudoThreadToken) {
        auto it = g_thread_tokens.find(GetCurrentThreadId());
        if (it == g_thread_tokens.end()) return kNoToken;
        *out = it->second;
        if (granted) *granted = kTokenAllAccess;
        return kSuccess;
    }
    if (!IsTokenHandle(h)) return kInvalidHandle;
    auto it = g_handles.find(FromHandle(h));
    if (it == g_handles.end()) return kInvalidHandle;
    if ((it->second.access & need) != need) return kAccessDenied;
    *out = it->second.tok;
    if (granted) *granted = it->second.access;
    return kSuccess;
}

// ---- information builders (lock held) --------------------------------------------------
std::vector<Group> AllGroups(const TokenObj& t) {
    std::vector<Group> g = t.groups;
    g.push_back({SidIntegrity(t.integrity_rid), kGrpIntegrity | kGrpIntegrityEnabled});
    return g;
}

// TOKEN_GROUPS: { DWORD GroupCount; (pad) SID_AND_ATTRIBUTES Groups[n]; } then the SIDs.
void BuildGroups(Blob& b, const std::vector<Group>& gs) {
    const size_t base = b.Reserve(8);
    b.Put32(base, static_cast<std::uint32_t>(gs.size()));
    const size_t arr = b.Reserve(16 * gs.size());
    for (size_t i = 0; i < gs.size(); ++i) {
        const size_t so = b.Append(gs[i].sid);
        b.Ptr(arr + 16 * i, so);
        b.Put32(arr + 16 * i + 8, gs[i].attrs);
    }
}

// TOKEN_USER / TOKEN_MANDATORY_LABEL: { SID_AND_ATTRIBUTES } then the SID.
void BuildSidAndAttrs(Blob& b, const Sid& sid, std::uint32_t attrs) {
    const size_t base = b.Reserve(16);
    const size_t so = b.Append(sid);
    b.Ptr(base, so);
    b.Put32(base + 8, attrs);
}

// TOKEN_OWNER / TOKEN_PRIMARY_GROUP: { PSID } then the SID.
void BuildSidPtr(Blob& b, const Sid& sid) {
    const size_t base = b.Reserve(8);
    const size_t so = b.Append(sid);
    b.Ptr(base, so);
}

void BuildPrivileges(Blob& b, const std::vector<Priv>& ps) {
    const size_t base = b.Reserve(4 + 12 * ps.size());
    b.Put32(base, static_cast<std::uint32_t>(ps.size()));
    for (size_t i = 0; i < ps.size(); ++i) {
        b.Put32(base + 4 + 12 * i, ps[i].luid.LowPart);
        b.Put32(base + 8 + 12 * i, static_cast<std::uint32_t>(ps[i].luid.HighPart));
        b.Put32(base + 12 + 12 * i, ps[i].attrs);
    }
}

void BuildU32(Blob& b, std::uint32_t v) { b.Put32(b.Reserve(4), v); }

// CLAIM_SECURITY_ATTRIBUTES_INFORMATION with no attributes.
void BuildEmptyClaims(Blob& b) {
    const size_t o = b.Reserve(16);
    const std::uint16_t ver = 1;
    std::memcpy(&b.d[o], &ver, 2);
}

NTSTATUS BuildInfo(const TokenObj& t, ULONG cls, Blob& b) {
    switch (cls) {
    case cUser:
        BuildSidAndAttrs(b, t.user, 0);
        return kSuccess;
    case cGroups:
        BuildGroups(b, AllGroups(t));
        return kSuccess;
    case cPrivileges:
        BuildPrivileges(b, t.privs);
        return kSuccess;
    case cOwner:
        BuildSidPtr(b, t.owner);
        return kSuccess;
    case cPrimaryGroup:
        BuildSidPtr(b, t.primary_group);
        return kSuccess;
    case cDefaultDacl: {
        const size_t base = b.Reserve(8);
        if (t.default_dacl.empty()) return kSuccess;   // NULL ACL pointer
        const size_t ao = b.Append(t.default_dacl.data(), t.default_dacl.size());
        b.Ptr(base, ao);
        return kSuccess;
    }
    case cSource: {
        TOKEN_SOURCE s{};
        std::memcpy(s.SourceName, t.source_name, 8);
        s.SourceIdentifier = t.source_luid;
        b.Append(&s, sizeof s);
        return kSuccess;
    }
    case cType:
        BuildU32(b, t.type);
        return kSuccess;
    case cImpersonationLevel:
        if (t.type != kTokenImpersonation) return kInvalidInfoClass;   // NT: only for impersonation tokens
        BuildU32(b, t.imp_level);
        return kSuccess;
    case cStatistics: {
        TOKEN_STATISTICS s{};
        static_assert(sizeof(TOKEN_STATISTICS) == 56, "TOKEN_STATISTICS layout");
        s.TokenId = t.token_id;
        s.AuthenticationId = t.auth_id;
        s.ExpirationTime.QuadPart = 0x7FFFFFFFFFFFFFFFll;   // never expires
        s.TokenType = static_cast<TOKEN_TYPE>(t.type);
        s.ImpersonationLevel = static_cast<SECURITY_IMPERSONATION_LEVEL>(t.type == kTokenImpersonation ? t.imp_level : 0);
        s.DynamicCharged = 0x1F4;
        s.DynamicAvailable = 0x1F4 - static_cast<DWORD>(t.default_dacl.size());
        s.GroupCount = static_cast<DWORD>(t.groups.size() + 1);   // + integrity label
        s.PrivilegeCount = static_cast<DWORD>(t.privs.size());
        s.ModifiedId = t.modified_id;
        b.Append(&s, sizeof s);
        return kSuccess;
    }
    case cRestrictedSids:
        BuildGroups(b, t.restricted);
        return kSuccess;
    case cSessionId:
        BuildU32(b, t.session);
        return kSuccess;
    case cGroupsAndPrivileges: {
        // TOKEN_GROUPS_AND_PRIVILEGES (56 bytes), then Sids (user + groups), restricted, privileges.
        const size_t base = b.Reserve(56);
        std::vector<Group> sids;
        sids.push_back({t.user, 0});
        for (auto& g : AllGroups(t)) sids.push_back(g);
        const size_t sids_off = b.size();
        const size_t arr = b.Reserve(16 * sids.size());
        for (size_t i = 0; i < sids.size(); ++i) {
            const size_t so = b.Append(sids[i].sid);
            b.Ptr(arr + 16 * i, so);
            b.Put32(arr + 16 * i + 8, sids[i].attrs);
        }
        const size_t sids_len = b.size() - sids_off;
        b.Put32(base + 0, static_cast<std::uint32_t>(sids.size()));
        b.Put32(base + 4, static_cast<std::uint32_t>(sids_len));
        b.Ptr(base + 8, arr);
        b.Put32(base + 16, static_cast<std::uint32_t>(t.restricted.size()));
        b.Put32(base + 20, 0);
        // restricted SIDs pointer stays NULL when there are none
        const size_t priv_off = b.size();
        const size_t parr = b.Reserve(12 * t.privs.size());
        for (size_t i = 0; i < t.privs.size(); ++i) {
            b.Put32(parr + 12 * i, t.privs[i].luid.LowPart);
            b.Put32(parr + 12 * i + 4, static_cast<std::uint32_t>(t.privs[i].luid.HighPart));
            b.Put32(parr + 12 * i + 8, t.privs[i].attrs);
        }
        b.Put32(base + 32, static_cast<std::uint32_t>(t.privs.size()));
        b.Put32(base + 36, static_cast<std::uint32_t>(b.size() - priv_off));
        if (!t.privs.empty()) b.Ptr(base + 40, parr);
        b.Put32(base + 48, t.auth_id.LowPart);
        b.Put32(base + 52, static_cast<std::uint32_t>(t.auth_id.HighPart));
        return kSuccess;
    }
    case cSandBoxInert:
    case cHasRestrictions:
    case cIsRestricted:
        BuildU32(b, cls == cSandBoxInert ? 0u : (t.restricted.empty() ? 0u : 1u));
        return kSuccess;
    case cOrigin: {
        const size_t o = b.Reserve(8);
        b.Put32(o, t.auth_id.LowPart);
        b.Put32(o + 4, static_cast<std::uint32_t>(t.auth_id.HighPart));
        return kSuccess;
    }
    case cElevationType:
        BuildU32(b, t.elevation_type);
        return kSuccess;
    case cLinkedToken:
        return kNoSuchLogonSession;   // no split token pair is ever offered: nothing to elevate to
    case cElevation:
        BuildU32(b, t.elevated ? 1u : 0u);
        return kSuccess;
    case cVirtualizationAllowed:
        BuildU32(b, t.virt_allowed);
        return kSuccess;
    case cVirtualizationEnabled:
        BuildU32(b, t.virt_enabled);
        return kSuccess;
    case cIntegrityLevel:
        BuildSidAndAttrs(b, SidIntegrity(t.integrity_rid), kGrpIntegrity | kGrpIntegrityEnabled);
        return kSuccess;
    case cUIAccess:
        BuildU32(b, t.ui_access);
        return kSuccess;
    case cMandatoryPolicy:
        BuildU32(b, t.mandatory_policy);
        return kSuccess;
    case cLogonSid: {
        std::vector<Group> logon;
        for (const auto& g : t.groups)
            if ((g.attrs & kGrpLogonId) == kGrpLogonId) logon.push_back(g);
        BuildGroups(b, logon);
        return kSuccess;
    }
    case cIsAppContainer:
    case cAppContainerNumber:
    case cPrivateNameSpace:
    case cChildProcessFlags:
    case cIsLessPrivilegedAppContainer:
    case cIsSandboxed:
    case cIsAppSilo:
        BuildU32(b, 0);
        return kSuccess;
    case cCapabilities:
    case cDeviceGroups:
    case cRestrictedDeviceGroups:
        BuildGroups(b, {});
        return kSuccess;
    case cAppContainerSid:
    case cProcessTrustLevel:
        b.Reserve(8);   // a NULL PSID
        return kSuccess;
    case cUserClaimAttributes:
    case cDeviceClaimAttributes:
    case cRestrictedUserClaimAttributes:
    case cRestrictedDeviceClaimAttributes:
    case cSecurityAttributes:
    case cSingletonAttributes:
        BuildEmptyClaims(b);
        return kSuccess;
    case cBnoIsolation:
        b.Reserve(16);   // { PWSTR IsolationPrefix = NULL; BOOLEAN IsolationEnabled = FALSE }
        return kSuccess;
    case cSessionReference:
    case cAuditPolicy:
        return kPrivilegeNotHeld;   // NT demands SeTcbPrivilege for these
    case cAccessInformation:
        FIXME(token, "TokenAccessInformation not implemented");
        return kNotImplemented;
    default:
        return cls >= cClsMax || cls == 0 ? kInvalidInfoClass : kNotImplemented;
    }
}

ACCESS_MASK QueryNeed(ULONG cls) { return cls == cSource ? kTokQuerySource : kTokQuery; }

// ---- security descriptors ------------------------------------------------------------
struct SdView {
    const std::uint8_t* owner = nullptr;
    const std::uint8_t* group = nullptr;
    const std::uint8_t* dacl = nullptr;   // ACL, or null
    bool dacl_present = false;
};

bool ParseSd(const void* sd, SdView* v) {
    if (!sd) return false;
    const auto* p = static_cast<const std::uint8_t*>(sd);
    if (p[0] != 1) return false;
    WORD control;
    std::memcpy(&control, p + 2, 2);
    v->dacl_present = (control & 0x0004) != 0;
    if (control & 0x8000) {   // SE_SELF_RELATIVE
        std::uint32_t o, g, d;
        std::memcpy(&o, p + 4, 4);
        std::memcpy(&g, p + 8, 4);
        std::memcpy(&d, p + 16, 4);
        v->owner = o ? p + o : nullptr;
        v->group = g ? p + g : nullptr;
        v->dacl = (v->dacl_present && d) ? p + d : nullptr;
    } else {
        const auto* a = static_cast<const SECURITY_DESCRIPTOR*>(sd);
        v->owner = static_cast<const std::uint8_t*>(a->Owner);
        v->group = static_cast<const std::uint8_t*>(a->Group);
        v->dacl = v->dacl_present ? reinterpret_cast<const std::uint8_t*>(a->Dacl) : nullptr;
    }
    return true;
}

// A caller's security descriptor, copied into a self-relative image of its own (owner and DACL; the group and the
// SACL play no part in an access check). An absolute descriptor is flattened, so ParseSd only ever reads the copy.
// A revision other than 1 yields a header-only image that ParseSd refuses.
NTSTATUS CaptureSd(const void* user, std::vector<std::uint8_t>* flat) {
    constexpr size_t kSelfRelativeHeader = 20;
    std::uint8_t h[sizeof(SECURITY_DESCRIPTOR)] = {};
    if (!user) return kInvalidSecurityDescr;
    if (NTSTATUS st = probe::CopyFromUser(h, user, kSelfRelativeHeader)) return st;
    flat->assign(kSelfRelativeHeader, 0);
    if (h[0] != 1) { (*flat)[0] = h[0]; return kSuccess; }
    WORD control;
    std::memcpy(&control, h + 2, 2);
    const auto* base = static_cast<const std::uint8_t*>(user);
    const std::uint8_t* owner = nullptr;
    const std::uint8_t* dacl = nullptr;
    const bool dacl_present = (control & 0x0004) != 0;
    if (control & 0x8000) {   // SE_SELF_RELATIVE
        std::uint32_t o, d;
        std::memcpy(&o, h + 4, 4);
        std::memcpy(&d, h + 16, 4);
        owner = o ? base + o : nullptr;
        dacl = (dacl_present && d) ? base + d : nullptr;
    } else {
        if (NTSTATUS st = probe::CopyFromUser(h + kSelfRelativeHeader, base + kSelfRelativeHeader,
                                              sizeof(SECURITY_DESCRIPTOR) - kSelfRelativeHeader))
            return st;
        const auto* a = reinterpret_cast<const SECURITY_DESCRIPTOR*>(h);
        owner = static_cast<const std::uint8_t*>(a->Owner);
        dacl = dacl_present ? reinterpret_cast<const std::uint8_t*>(a->Dacl) : nullptr;
    }
    (*flat)[0] = 1;
    const WORD out_control = static_cast<WORD>(0x8000 | (dacl_present ? 0x0004 : 0));
    std::memcpy(flat->data() + 2, &out_control, 2);
    if (owner) {
        Sid sid;
        if (NTSTATUS st = CaptureSid(owner, &sid)) return st;
        const std::uint32_t off = static_cast<std::uint32_t>(flat->size());
        flat->insert(flat->end(), sid.begin(), sid.end());
        std::memcpy(flat->data() + 4, &off, 4);
    }
    if (dacl) {
        WORD size;
        if (NTSTATUS st = probe::CopyFromUser(&size, dacl + 2, 2)) return st;
        if (size < 8) size = 8;   // the header alone: the evaluator rejects what it finds in it
        std::vector<std::uint8_t> acl(size);
        if (NTSTATUS st = probe::CopyFromUser(acl.data(), dacl, size)) return st;
        const std::uint32_t off = static_cast<std::uint32_t>(flat->size());
        flat->insert(flat->end(), acl.begin(), acl.end());
        std::memcpy(flat->data() + 16, &off, 4);
    }
    return kSuccess;
}

ACCESS_MASK MapGeneric(ACCESS_MASK m, const GENERIC_MAPPING* gm) {
    if (m & kGenericRead)    m |= gm->GenericRead;
    if (m & kGenericWrite)   m |= gm->GenericWrite;
    if (m & kGenericExecute) m |= gm->GenericExecute;
    if (m & kGenericAll)     m |= gm->GenericAll;
    return m & ~(kGenericRead | kGenericWrite | kGenericExecute | kGenericAll);
}

bool GroupEnabled(const Group& g) { return (g.attrs & kGrpEnabled) && !(g.attrs & kGrpUseForDenyOnly); }

// Does the SID in an ACE apply for an ALLOW (enabled only) / DENY (enabled or deny-only) decision?
bool SidAppliesAllow(const TokenObj& t, const void* sid) {
    if (SidEq(t.user, sid)) return true;
    for (const auto& g : t.groups) if (GroupEnabled(g) && SidEq(g.sid, sid)) return true;
    return false;
}
bool SidAppliesDeny(const TokenObj& t, const void* sid) {
    if (SidEq(t.user, sid)) return true;
    for (const auto& g : t.groups)
        if ((g.attrs & (kGrpEnabled | kGrpUseForDenyOnly)) && SidEq(g.sid, sid)) return true;
    return false;
}

// Self-relative SD for a token object (Sacl, Dacl, Owner, Group).
std::vector<std::uint8_t> BuildTokenSd(const TokenObj& t, SECURITY_INFORMATION si) {
    std::vector<std::uint8_t> sacl, dacl;
    const bool want_label = (si & 0x10) != 0;       // LABEL_SECURITY_INFORMATION
    if (want_label) {
        sacl = NewAcl();
        const Sid label = SidIntegrity(t.integrity_rid);
        const std::uint16_t size = static_cast<std::uint16_t>(8 + label.size());
        const size_t o = sacl.size();
        sacl.resize(o + size);
        sacl[o] = 0x11;   // SYSTEM_MANDATORY_LABEL_ACE_TYPE
        std::memcpy(&sacl[o + 2], &size, 2);
        const std::uint32_t mask = 1;   // SYSTEM_MANDATORY_LABEL_NO_WRITE_UP
        std::memcpy(&sacl[o + 4], &mask, 4);
        std::memcpy(&sacl[o + 8], label.data(), label.size());
        const std::uint16_t cnt = 1, total = static_cast<std::uint16_t>(sacl.size());
        std::memcpy(&sacl[4], &cnt, 2);
        std::memcpy(&sacl[2], &total, 2);
    }
    const bool want_dacl = (si & 0x4) != 0;
    if (want_dacl) {
        dacl = NewAcl();
        AddAllowAce(dacl, kTokenAllAccess, t.elevated ? SidBuiltinAdmins() : t.user);
        AddAllowAce(dacl, kTokenAllAccess, SidSystem());
    }
    const bool want_owner = (si & 0x1) != 0, want_group = (si & 0x2) != 0;

    std::vector<std::uint8_t> sd(20, 0);
    sd[0] = 1;
    WORD control = 0x8000;
    if (want_dacl) control |= 0x0004;
    if (want_label) control |= 0x0010;
    std::memcpy(&sd[2], &control, 2);
    auto put = [&](size_t hdr_off, const void* p, size_t n) {
        const std::uint32_t off = static_cast<std::uint32_t>(sd.size());
        sd.insert(sd.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
        std::memcpy(&sd[hdr_off], &off, 4);
    };
    if (want_label) put(12, sacl.data(), sacl.size());
    if (want_dacl) put(16, dacl.data(), dacl.size());
    if (want_owner) put(4, t.owner.data(), t.owner.size());
    if (want_group) put(8, t.primary_group.data(), t.primary_group.size());
    return sd;
}

// ---- helpers for Adjust* ---------------------------------------------------------------
NTSTATUS OpenCommon(ACCESS_MASK want, ULONG attrs, PHANDLE out, const TokPtr& tok) {
    if (NTSTATUS st = probe::ProbeWrite(out, sizeof(HANDLE))) return st;   // NULL included
    const ACCESS_MASK mapped = MapTokenAccess(want);
    if (mapped & ~kTokenAllAccess) return kAccessDenied;   // SYNCHRONIZE, ACCESS_SYSTEM_SECURITY, ...
    const TokenId id = AddHandleLocked(tok, mapped, (attrs & 0x2) != 0);   // OBJ_INHERIT
    if (NTSTATUS st = probe::Store<HANDLE>(out, HandleValue(id))) {
        g_handles.erase(id);   // the caller's slot vanished: do not leak the handle
        return st;
    }
    return kSuccess;
}

}  // namespace

// =============================================================================
// Public helpers
// =============================================================================

NTSTATUS Initialize(const Identity& id) {
    Lock l;
    const NTSTATUS st = InitializeLocked(id);
    if (st == kSuccess)
        TRACE(token, "identity %ls\\%ls sid=%ls session=%u elevated=%d il=0x%x", id.domain.c_str(), id.user_name.c_str(),
              id.user_sid.c_str(), id.session_id, (int)id.elevated, id.integrity_rid);
    else
        ERR(token, "invalid identity sid '%ls' (0x%08lx)", id.user_sid.c_str(), (unsigned long)st);
    return st;
}

const Identity& Current() {
    EnsureReady();
    return g_ident;
}

bool IsTokenHandle(HANDLE h) {
    if (h == kPseudoProcessToken || h == kPseudoThreadToken || h == kPseudoEffectiveToken) return true;
    return (reinterpret_cast<std::uintptr_t>(h) & ~static_cast<std::uintptr_t>(0xFFFFFFFFu)) == 0 &&
           (reinterpret_cast<std::uintptr_t>(h) & kHandleMask) == kHandleBase;
}

HANDLE ToHandle(TokenId id) { return HandleValue(id); }

TokenId FromHandle(HANDLE h) {
    if (!IsTokenHandle(h) || h == kPseudoProcessToken || h == kPseudoThreadToken || h == kPseudoEffectiveToken) return 0;
    return (reinterpret_cast<std::uintptr_t>(h) & ~kHandleMask) >> 2;
}

TokenId PrimaryToken() {
    EnsureReady();
    Lock l;
    return g_primary_id;
}

std::wstring UserSidString() { EnsureReady(); Lock l; return g_ident.user_sid; }
std::wstring UserName() { EnsureReady(); Lock l; return g_ident.user_name; }
std::wstring ComputerName() { EnsureReady(); Lock l; return g_ident.computer_name; }

bool SidFromString(const std::wstring& s, std::vector<std::uint8_t>* out) { return ParseSid(s, out); }
std::wstring SidToString(const void* sid) { return SidToStr(sid); }

// =============================================================================
// NT entry points
// =============================================================================

NTSTATUS Tok_NtOpenProcessToken(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, PHANDLE TokenHandle) {
    return Tok_NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, 0, TokenHandle);
}

NTSTATUS Tok_NtOpenProcessTokenEx(HANDLE /*ProcessHandle*/, ACCESS_MASK DesiredAccess, ULONG HandleAttributes, PHANDLE TokenHandle) {
    EnsureReady();
    Lock l;
    const NTSTATUS st = OpenCommon(DesiredAccess, HandleAttributes, TokenHandle, g_primary);
    TRACE(token, "access=%08lx (0x%08lx)", (unsigned long)DesiredAccess, (unsigned long)st);
    return st;
}

NTSTATUS Tok_NtOpenThreadToken(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN OpenAsSelf, PHANDLE TokenHandle) {
    return Tok_NtOpenThreadTokenEx(ThreadHandle, DesiredAccess, OpenAsSelf, 0, TokenHandle);
}

NTSTATUS Tok_NtOpenThreadTokenEx(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN /*OpenAsSelf*/, ULONG HandleAttributes, PHANDLE TokenHandle) {
    // A thread that is not impersonating gets a handle on the emulated process
    // token (instead of STATUS_NO_TOKEN): callers that fall back to the process
    // token (CheckTokenMembership & co) work either way, and callers that only
    // want "some token of this thread" succeed. An impersonating thread
    // (SetThreadImpersonationToken / NtImpersonateAnonymousToken) gets its impersonation token.
    EnsureReady();
    Lock l;
    const DWORD tid = ThreadIdOf(ThreadHandle);
    if (!tid) return kInvalidHandle;
    auto it = g_thread_tokens.find(tid);
    const TokPtr& tok = it != g_thread_tokens.end() ? it->second : g_primary;
    return OpenCommon(DesiredAccess, HandleAttributes, TokenHandle, tok);
}

NTSTATUS Tok_NtQueryInformationToken(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
                                     PVOID TokenInformation, ULONG TokenInformationLength, PULONG ReturnLength) {
    EnsureReady();
    const ULONG cls = static_cast<ULONG>(TokenInformationClass);
    if (NTSTATUS st = probe::ProbeWrite(TokenInformation, TokenInformationLength)) return st;
    if (ReturnLength)
        if (NTSTATUS st = probe::ProbeWrite(ReturnLength, sizeof(ULONG), sizeof(ULONG))) return st;
    Blob b;
    {
        Lock l;
        TokPtr t;
        NTSTATUS st = Resolve(TokenHandle, QueryNeed(cls), &t);
        if (st != kSuccess) return st;
        st = BuildInfo(*t, cls, b);
        if (st != kSuccess) {
            TRACE(token, "class %lu -> 0x%08lx", (unsigned long)cls, (unsigned long)st);
            return st;
        }
    }
    if (ReturnLength)
        if (NTSTATUS st = probe::Store<ULONG>(ReturnLength, static_cast<ULONG>(b.size()))) return st;
    if (TokenInformationLength < b.size()) return kBufferTooSmall;
    return b.Emit(TokenInformation);
}

NTSTATUS Tok_NtSetInformationToken(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, PVOID TokenInformation, ULONG Length) {
    EnsureReady();
    const ULONG cls = static_cast<ULONG>(TokenInformationClass);
    Lock l;
    TokPtr t;
    ACCESS_MASK need = kTokAdjustDefault;
    if (cls == cSessionId) need = kTokAdjustSessionId;
    NTSTATUS st = Resolve(TokenHandle, need, &t);
    if (st != kSuccess) return st;
    // The caller's buffer is copied once; SIDs and ACLs it points at are copied where they are used.
    if (NTSTATUS ps = probe::ProbeRead(TokenInformation, Length)) return ps;
    if (!Length) return kAccessViolation;
    std::vector<std::uint8_t> info_copy(Length);
    if (NTSTATUS ps = probe::CopyFromUser(info_copy.data(), TokenInformation, Length)) return ps;
    TokenInformation = info_copy.data();
    auto touch = [&] { t->modified_id = NextLuid(); };

    switch (cls) {
    case cOwner: {
        if (Length < 8) return kInfoLengthMismatch;
        const void* user_sid = *static_cast<void* const*>(TokenInformation);
        Sid sid_copy;
        if (!user_sid) return kInvalidSid;
        if (NTSTATUS ps = CaptureSid(user_sid, &sid_copy)) return ps;
        const void* sid = sid_copy.data();
        if (!SidValid(sid)) return kInvalidSid;
        bool ok = SidEq(t->user, sid);
        for (const auto& g : t->groups) if (g.attrs & kGrpOwner && SidEq(g.sid, sid)) ok = true;
        if (!ok) return kInvalidOwner;
        t->owner = CopySid(sid);
        touch();
        return kSuccess;
    }
    case cPrimaryGroup: {
        if (Length < 8) return kInfoLengthMismatch;
        const void* user_sid = *static_cast<void* const*>(TokenInformation);
        Sid sid_copy;
        if (!user_sid) return kInvalidSid;
        if (NTSTATUS ps = CaptureSid(user_sid, &sid_copy)) return ps;
        const void* sid = sid_copy.data();
        if (!SidValid(sid)) return kInvalidSid;
        bool ok = SidEq(t->primary_group, sid);
        for (const auto& g : t->groups) if (SidEq(g.sid, sid)) ok = true;
        if (!ok) return kInvalidPrimaryGroup;
        t->primary_group = CopySid(sid);
        touch();
        return kSuccess;
    }
    case cDefaultDacl: {
        if (Length < 8) return kInfoLengthMismatch;
        const auto* acl = *static_cast<const std::uint8_t* const*>(TokenInformation);
        if (!acl) { t->default_dacl.clear(); touch(); return kSuccess; }
        WORD size;
        if (NTSTATUS ps = probe::CopyFromUser(&size, acl + 2, 2)) return ps;
        if (size < 8) return kInvalidParameter;
        std::vector<std::uint8_t> acl_copy(size);
        if (NTSTATUS ps = probe::CopyFromUser(acl_copy.data(), acl, size)) return ps;
        t->default_dacl = std::move(acl_copy);
        touch();
        return kSuccess;
    }
    case cSessionId:
        if (Length < 4) return kInfoLengthMismatch;
        return kPrivilegeNotHeld;   // needs SeTcbPrivilege, which the user token never holds
    case cVirtualizationEnabled:
    case cVirtualizationAllowed:
        if (Length < 4) return kInfoLengthMismatch;
        if (cls == cVirtualizationEnabled) {
            if (*static_cast<ULONG*>(TokenInformation) && !t->virt_allowed) return kAccessDenied;
            t->virt_enabled = *static_cast<ULONG*>(TokenInformation) ? 1 : 0;
        } else {
            return kPrivilegeNotHeld;
        }
        touch();
        return kSuccess;
    case cUIAccess:
        if (Length < 4) return kInfoLengthMismatch;
        if (*static_cast<ULONG*>(TokenInformation)) return kPrivilegeNotHeld;   // granting UIAccess needs TCB
        t->ui_access = 0;
        return kSuccess;
    case cIntegrityLevel: {
        if (Length < 16) return kInfoLengthMismatch;
        const void* user_sid = *static_cast<void* const*>(TokenInformation);
        Sid sid_copy;
        if (!user_sid) return kInvalidSid;
        if (NTSTATUS ps = CaptureSid(user_sid, &sid_copy)) return ps;
        const void* sid = sid_copy.data();
        if (!SidValid(sid) || static_cast<const std::uint8_t*>(sid)[1] != 1 || SidLen(sid) != 12) return kInvalidSid;
        const auto* b = static_cast<const std::uint8_t*>(sid);
        std::uint64_t auth = 0;
        for (int i = 0; i < 6; ++i) auth = (auth << 8) | b[2 + i];
        if (auth != 16) return kInvalidSid;
        std::uint32_t rid;
        std::memcpy(&rid, b + 8, 4);
        if (rid > t->integrity_rid) return kPrivilegeNotHeld;   // raising integrity needs TCB
        t->integrity_rid = rid;
        touch();
        return kSuccess;
    }
    case cMandatoryPolicy:
        return kPrivilegeNotHeld;
    default:
        return kInvalidInfoClass;
    }
}

NTSTATUS Tok_NtAdjustPrivilegesToken(HANDLE TokenHandle, BOOLEAN DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
                                     ULONG BufferLength, PTOKEN_PRIVILEGES PreviousState, PULONG ReturnLength) {
    EnsureReady();
    // Captured before the lock: the new state, and the probes of the two outputs.
    std::vector<LUID_AND_ATTRIBUTES> requested;
    if (!DisableAllPrivileges) {
        if (!NewState) return kInvalidParameter;
        if (NTSTATUS ps = CaptureArray(NewState, offsetof(TOKEN_PRIVILEGES, Privileges), &requested)) return ps;
    }
    if (PreviousState) {
        if (NTSTATUS ps = probe::ProbeWrite(PreviousState, BufferLength)) return ps;
        if (ReturnLength)
            if (NTSTATUS ps = probe::ProbeWrite(ReturnLength, sizeof(ULONG), sizeof(ULONG))) return ps;
    } else if (ReturnLength) {
        if (NTSTATUS ps = probe::ProbeWrite(ReturnLength, sizeof(ULONG), sizeof(ULONG))) return ps;
    }
    Lock l;
    TokPtr t;
    NTSTATUS st = Resolve(TokenHandle, kTokAdjustPrivileges | (PreviousState ? kTokQuery : 0), &t);
    if (st != kSuccess) return st;

    std::vector<Priv> next = t->privs;
    std::vector<Priv> prev;           // old attributes of every privilege that changes
    std::vector<LUID> removals;
    bool missing = false;

    if (DisableAllPrivileges) {
        for (auto& p : next) {
            if (p.attrs & kPrvEnabled) {
                prev.push_back(p);
                p.attrs &= ~kPrvEnabled;
            }
        }
    } else {
        for (size_t i = 0; i < requested.size(); ++i) {
            const LUID_AND_ATTRIBUTES& e = requested[i];
            auto it = std::find_if(next.begin(), next.end(), [&](const Priv& p) { return LuidEq(p.luid, e.Luid); });
            if (it == next.end()) { missing = true; continue; }
            const std::uint32_t old = it->attrs;
            if (e.Attributes & kPrvRemoved) {
                prev.push_back(*it);
                removals.push_back(e.Luid);
                continue;
            }
            if (e.Attributes & kPrvEnabled) it->attrs |= kPrvEnabled; else it->attrs &= ~kPrvEnabled;
            if (it->attrs != old) prev.push_back(Priv{it->luid, old});
        }
        for (const LUID& r : removals)
            next.erase(std::remove_if(next.begin(), next.end(), [&](const Priv& p) { return LuidEq(p.luid, r); }), next.end());
    }

    const ULONG needed = static_cast<ULONG>(4 + 12 * prev.size());
    if (PreviousState) {
        if (ReturnLength)
            if (NTSTATUS ws = probe::Store<ULONG>(ReturnLength, needed)) return ws;
        if (BufferLength < needed) return kBufferTooSmall;   // nothing is changed
        Blob b;
        BuildPrivileges(b, prev);
        if (NTSTATUS ws = b.Emit(PreviousState)) return ws;
    } else if (ReturnLength) {
        if (NTSTATUS ws = probe::Store<ULONG>(ReturnLength, 0)) return ws;
    }
    if (!prev.empty()) {
        t->privs = std::move(next);
        t->modified_id = NextLuid();
    }
    TRACE(token, "disableAll=%d changed=%zu missing=%d", (int)DisableAllPrivileges, prev.size(), (int)missing);
    return missing ? kNotAllAssigned : kSuccess;
}

NTSTATUS Tok_NtAdjustGroupsToken(HANDLE TokenHandle, BOOLEAN ResetToDefault, PTOKEN_GROUPS NewState, ULONG BufferLength,
                                 PTOKEN_GROUPS PreviousState, PULONG ReturnLength) {
    EnsureReady();
    // Captured before the lock: the new groups (every SID in them) and the probes of the two outputs.
    std::vector<Group> requested;
    if (!ResetToDefault) {
        if (!NewState) return kInvalidParameter;
        std::vector<SID_AND_ATTRIBUTES> entries;
        if (NTSTATUS ps = CaptureArray(NewState, offsetof(TOKEN_GROUPS, Groups), &entries)) return ps;
        for (const SID_AND_ATTRIBUTES& e : entries) {
            Group g;
            g.attrs = e.Attributes;
            if (NTSTATUS ps = CaptureSid(e.Sid, &g.sid)) return ps;
            requested.push_back(std::move(g));
        }
    }
    if (PreviousState) {
        if (NTSTATUS ps = probe::ProbeWrite(PreviousState, BufferLength)) return ps;
    }
    if (ReturnLength)
        if (NTSTATUS ps = probe::ProbeWrite(ReturnLength, sizeof(ULONG), sizeof(ULONG))) return ps;
    Lock l;
    TokPtr t;
    NTSTATUS st = Resolve(TokenHandle, kTokAdjustGroups | (PreviousState ? kTokQuery : 0), &t);
    if (st != kSuccess) return st;

    std::vector<Group> next = t->groups;
    std::vector<Group> prev;
    bool missing = false;

    if (ResetToDefault) {
        for (auto& g : next) {
            const std::uint32_t want = (g.attrs & kGrpEnabledByDefault) ? (g.attrs | kGrpEnabled) : (g.attrs & ~kGrpEnabled);
            if (want != g.attrs) { prev.push_back(g); g.attrs = want; }
        }
    } else {
        for (size_t i = 0; i < requested.size(); ++i) {
            const Group& e = requested[i];
            auto it = std::find_if(next.begin(), next.end(), [&](const Group& g) { return SidEq(g.sid, e.sid); });
            if (it == next.end()) { missing = true; continue; }
            const bool want_on = (e.attrs & kGrpEnabled) != 0;
            if (!want_on && (it->attrs & kGrpMandatory)) return kCantDisableMandatory;
            if (want_on && (it->attrs & kGrpUseForDenyOnly)) return kInvalidParameter;
            const std::uint32_t old = it->attrs;
            if (want_on) it->attrs |= kGrpEnabled; else it->attrs &= ~kGrpEnabled;
            if (it->attrs != old) prev.push_back(Group{it->sid, old});
        }
    }
    Blob b;
    BuildGroups(b, prev);
    if (PreviousState) {
        if (ReturnLength)
            if (NTSTATUS ws = probe::Store<ULONG>(ReturnLength, static_cast<ULONG>(b.size()))) return ws;
        if (BufferLength < b.size()) return kBufferTooSmall;
        if (NTSTATUS ws = b.Emit(PreviousState)) return ws;
    } else if (ReturnLength) {
        if (NTSTATUS ws = probe::Store<ULONG>(ReturnLength, 0)) return ws;
    }
    if (!prev.empty()) {
        t->groups = std::move(next);
        t->modified_id = NextLuid();
    }
    return missing ? kNotAllAssigned : kSuccess;
}

NTSTATUS Tok_NtDuplicateToken(HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                              BOOLEAN EffectiveOnly, TOKEN_TYPE TokenType, PHANDLE NewTokenHandle) {
    EnsureReady();
    if (NTSTATUS ps = probe::ProbeWrite(NewTokenHandle, sizeof(HANDLE))) return ps;
    // The attributes are optional; their quality-of-service block is followed, so it is read here, once.
    probe::ObjectAttributes cap;
    SECURITY_QUALITY_OF_SERVICE qos{};
    bool have_qos = false;
    if (ObjectAttributes) {
        if (NTSTATUS ps = probe::CaptureObjectAttributes(ObjectAttributes, &cap)) return ps;
        if (cap.oa.SecurityQualityOfService) {
            if (NTSTATUS ps = probe::Load<SECURITY_QUALITY_OF_SERVICE>(
                    static_cast<const SECURITY_QUALITY_OF_SERVICE*>(cap.oa.SecurityQualityOfService), &qos))
                return ps;
            have_qos = true;
        }
    }
    const std::uint32_t type = static_cast<std::uint32_t>(TokenType);
    if (type != kTokenPrimary && type != kTokenImpersonation) return kInvalidParameter;
    Lock l;
    TokPtr src;
    NTSTATUS st = Resolve(ExistingTokenHandle, kTokDuplicate, &src);
    if (st != kSuccess) return st;

    std::uint32_t level = src->type == kTokenImpersonation ? src->imp_level : kSecImpersonation;
    bool effective_only = EffectiveOnly != 0;
    ULONG attrs = 0;
    if (ObjectAttributes) {
        attrs = cap.oa.Attributes;
        if (have_qos && qos.Length == sizeof(SECURITY_QUALITY_OF_SERVICE)) {
            if (qos.ImpersonationLevel > kSecDelegation) return kInvalidParameter;
            level = static_cast<std::uint32_t>(qos.ImpersonationLevel);
            if (qos.EffectiveOnly) effective_only = true;
        }
    }
    if (type == kTokenPrimary && src->type == kTokenImpersonation && src->imp_level < kSecImpersonation)
        return kBadImpersonationLevel;
    if (type == kTokenImpersonation && src->type == kTokenImpersonation && level > src->imp_level)
        return kBadImpersonationLevel;

    auto copy = std::make_shared<TokenObj>(*src);
    copy->type = type;
    copy->imp_level = type == kTokenImpersonation ? level : kSecAnonymous;
    copy->token_id = NextLuid();
    copy->modified_id = NextLuid();
    if (effective_only) {
        copy->privs.erase(std::remove_if(copy->privs.begin(), copy->privs.end(),
                          [](const Priv& p) { return !(p.attrs & kPrvEnabled); }), copy->privs.end());
        copy->groups.erase(std::remove_if(copy->groups.begin(), copy->groups.end(),
                           [](const Group& g) { return !(g.attrs & kGrpEnabled); }), copy->groups.end());
    }
    const ACCESS_MASK want = DesiredAccess == 0 ? kTokenAllAccess : DesiredAccess;
    return OpenCommon(want, attrs, NewTokenHandle, copy);
}

NTSTATUS Tok_NtFilterToken(HANDLE ExistingTokenHandle, ULONG Flags, PTOKEN_GROUPS SidsToDisable,
                           PTOKEN_PRIVILEGES PrivilegesToDelete, PTOKEN_GROUPS RestrictedSids, PHANDLE NewTokenHandle) {
    EnsureReady();
    if (NTSTATUS ps = probe::ProbeWrite(NewTokenHandle, sizeof(HANDLE))) return ps;
    // The three optional lists are copied once, every SID in them included.
    auto capture_groups = [](const TOKEN_GROUPS* user, std::vector<Group>* out) -> NTSTATUS {
        std::vector<SID_AND_ATTRIBUTES> entries;
        if (NTSTATUS ps = CaptureArray(user, offsetof(TOKEN_GROUPS, Groups), &entries)) return ps;
        for (const SID_AND_ATTRIBUTES& e : entries) {
            Group g;
            g.attrs = e.Attributes;
            if (NTSTATUS ps = CaptureSid(e.Sid, &g.sid)) return ps;
            out->push_back(std::move(g));
        }
        return kSuccess;
    };
    std::vector<Group> to_disable, restricted_in;
    std::vector<LUID_AND_ATTRIBUTES> to_delete;
    if (SidsToDisable)
        if (NTSTATUS ps = capture_groups(SidsToDisable, &to_disable)) return ps;
    if (PrivilegesToDelete)
        if (NTSTATUS ps = CaptureArray(PrivilegesToDelete, offsetof(TOKEN_PRIVILEGES, Privileges), &to_delete)) return ps;
    if (RestrictedSids)
        if (NTSTATUS ps = capture_groups(RestrictedSids, &restricted_in)) return ps;
    Lock l;
    TokPtr src;
    NTSTATUS st = Resolve(ExistingTokenHandle, kTokDuplicate, &src);
    if (st != kSuccess) return st;
    constexpr ULONG kDisableMaxPrivilege = 0x1, kSandboxInert = 0x2, kLuaToken = 0x4, kWriteRestricted = 0x8;
    if (Flags & ~(kDisableMaxPrivilege | kSandboxInert | kLuaToken | kWriteRestricted)) return kInvalidParameter;

    auto t = std::make_shared<TokenObj>(*src);
    t->token_id = NextLuid();
    t->modified_id = NextLuid();
    if (SidsToDisable) {
        for (const Group& d : to_disable) {
            const void* sid = d.sid.data();
            for (auto& g : t->groups)
                if (SidEq(g.sid, sid)) { g.attrs = (g.attrs & ~(kGrpEnabled | kGrpEnabledByDefault)) | kGrpUseForDenyOnly; }
            if (SidEq(t->user, sid)) { /* user SID cannot be disabled via groups; real NT turns it deny-only */ }
        }
    }
    if (Flags & kLuaToken) {
        // LUA_TOKEN: drop admin-ish groups to deny-only.
        for (auto& g : t->groups)
            if (SidEq(g.sid, SidBuiltinAdmins())) g.attrs = (g.attrs & ~(kGrpEnabled | kGrpEnabledByDefault)) | kGrpUseForDenyOnly;
    }
    if (Flags & kDisableMaxPrivilege) {
        t->privs.erase(std::remove_if(t->privs.begin(), t->privs.end(),
                       [](const Priv& p) { return p.luid.LowPart != pChangeNotify; }), t->privs.end());
    }
    if (PrivilegesToDelete) {
        for (const LUID_AND_ATTRIBUTES& d : to_delete) {
            const LUID l2 = d.Luid;
            t->privs.erase(std::remove_if(t->privs.begin(), t->privs.end(),
                           [&](const Priv& p) { return LuidEq(p.luid, l2); }), t->privs.end());
        }
    }
    if (RestrictedSids) {
        for (const Group& r : restricted_in) {
            if (!SidValid(r.sid.data())) return kInvalidSid;
            t->restricted.push_back({r.sid, kGrpStd});
        }
    }
    return OpenCommon(kTokenAllAccess, 0, NewTokenHandle, t);
}

NTSTATUS Tok_NtImpersonateAnonymousToken(HANDLE ThreadHandle) {
    EnsureReady();
    Lock l;
    const DWORD tid = ThreadIdOf(ThreadHandle);
    if (!tid) return kInvalidHandle;
    g_thread_tokens[tid] = MakeAnonymous();
    return kSuccess;
}

NTSTATUS SetThreadImpersonationToken(HANDLE ThreadHandle, HANDLE TokenHandle) {
    EnsureReady();
    Lock l;
    const DWORD tid = ThreadIdOf(ThreadHandle);
    if (!tid) return kInvalidHandle;
    if (!TokenHandle) {
        g_thread_tokens.erase(tid);
        return kSuccess;
    }
    TokPtr t;
    NTSTATUS st = Resolve(TokenHandle, kTokImpersonate, &t);
    if (st != kSuccess) return st;
    if (t->type == kTokenPrimary) {
        // NT converts a primary token to an impersonation token at SecurityImpersonation.
        auto c = std::make_shared<TokenObj>(*t);
        c->type = kTokenImpersonation;
        c->imp_level = kSecImpersonation;
        c->token_id = NextLuid();
        t = c;
    } else if (t->imp_level < kSecImpersonation) {
        // Identification/anonymous levels are legal for impersonation; kept as is.
    }
    g_thread_tokens[tid] = t;
    return kSuccess;
}

NTSTATUS Tok_NtPrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges, PBOOLEAN Result) {
    // NT insists on an impersonation token here; the process token is accepted
    // too (callers commonly pass whatever OpenProcessToken gave them).
    EnsureReady();
    if (NTSTATUS ps = probe::ProbeWrite(Result, sizeof(BOOLEAN))) return ps;
    // PRIVILEGE_SET { ULONG PrivilegeCount; ULONG Control; LUID_AND_ATTRIBUTES Privilege[] }: copied, then the
    // attributes (PRIVILEGE_USED_FOR_ACCESS) are written back.
    constexpr size_t kPrivilegeOffset = offsetof(PRIVILEGE_SET, Privilege);
    ULONG control;
    std::vector<LUID_AND_ATTRIBUTES> required;
    if (!RequiredPrivileges) return kAccessViolation;
    if (NTSTATUS ps = probe::Load<ULONG>(&RequiredPrivileges->Control, &control)) return ps;
    if (NTSTATUS ps = CaptureArray(RequiredPrivileges, kPrivilegeOffset, &required)) return ps;
    if (NTSTATUS ps = probe::ProbeWrite(RequiredPrivileges->Privilege, required.size() * sizeof(LUID_AND_ATTRIBUTES)))
        return ps;
    Lock l;
    TokPtr t;
    NTSTATUS st = Resolve(ClientToken, kTokQuery, &t);
    if (st != kSuccess) return st;
    const bool all_necessary = (control & 1) != 0;   // PRIVILEGE_SET_ALL_NECESSARY
    DWORD held = 0;
    for (size_t i = 0; i < required.size(); ++i) {
        LUID_AND_ATTRIBUTES& e = required[i];
        e.Attributes &= ~kPrvUsedForAccess;
        for (const auto& p : t->privs) {
            if (LuidEq(p.luid, e.Luid) && (p.attrs & kPrvEnabled)) {
                e.Attributes |= kPrvUsedForAccess;
                ++held;
                break;
            }
        }
    }
    const DWORD n = static_cast<DWORD>(required.size());
    if (!required.empty())
        if (NTSTATUS ws = probe::CopyToUser(RequiredPrivileges->Privilege, required.data(),
                                            required.size() * sizeof(LUID_AND_ATTRIBUTES)))
            return ws;
    return probe::Store<BOOLEAN>(Result, all_necessary ? (held == n) : (n == 0 || held > 0));
}

NTSTATUS Tok_NtAccessCheck(PSECURITY_DESCRIPTOR SecurityDescriptor, HANDLE ClientToken, ACCESS_MASK DesiredAccess,
                           PGENERIC_MAPPING GenericMapping, PPRIVILEGE_SET PrivilegeSet, PULONG PrivilegeSetLength,
                           PACCESS_MASK GrantedAccess, NTSTATUS* AccessStatus) {  // PNTSTATUS undeclared on MSVC
    // A real DACL evaluator (allow/deny ACEs matched against the user and enabled
    // groups, implicit READ_CONTROL|WRITE_DAC for the owner, NULL DACL = full
    // access, empty DACL = none). Not evaluated: mandatory-label ACEs, callback /
    // object ACEs, restricted-SID second pass. NT requires an impersonation
    // token; the process token is accepted too.
    EnsureReady();
    // Everything the caller supplied is copied first, the outputs are probed, and only the copies are evaluated.
    if (NTSTATUS ps = probe::ProbeWrite(GrantedAccess, sizeof(ACCESS_MASK), sizeof(ULONG))) return ps;
    if (NTSTATUS ps = probe::ProbeWrite(AccessStatus, sizeof(NTSTATUS), sizeof(ULONG))) return ps;
    if (!GenericMapping) return kInvalidParameter;
    GENERIC_MAPPING mapping;
    if (NTSTATUS ps = probe::Load<GENERIC_MAPPING>(GenericMapping, &mapping)) return ps;
    GenericMapping = &mapping;
    std::vector<std::uint8_t> sd_image;
    if (NTSTATUS ps = CaptureSd(SecurityDescriptor, &sd_image)) return ps;
    SdView sd;
    if (!ParseSd(sd_image.data(), &sd)) return kInvalidSecurityDescr;
    if (!sd.owner || !SidValid(sd.owner)) return kInvalidSecurityDescr;

    if (PrivilegeSet && PrivilegeSetLength) {
        ULONG set_length;
        if (NTSTATUS ps = probe::Load<ULONG>(PrivilegeSetLength, &set_length)) return ps;
        if (set_length < sizeof(PRIVILEGE_SET)) {
            if (NTSTATUS ws = probe::Store<ULONG>(PrivilegeSetLength, sizeof(PRIVILEGE_SET))) return ws;
            return kBufferTooSmall;
        }
        const PRIVILEGE_SET none{};
        if (NTSTATUS ws = probe::Store<PRIVILEGE_SET>(PrivilegeSet, none)) return ws;
    }
    auto finish = [&](ACCESS_MASK granted_mask, NTSTATUS access_status) -> NTSTATUS {
        if (NTSTATUS ws = probe::Store<ACCESS_MASK>(GrantedAccess, granted_mask)) return ws;
        if (NTSTATUS ws = probe::Store<NTSTATUS>(AccessStatus, access_status)) return ws;
        return kSuccess;
    };

    Lock l;
    TokPtr t;
    NTSTATUS st = Resolve(ClientToken, kTokQuery, &t);
    if (st != kSuccess) return st;

    const bool maximum = (DesiredAccess & kMaximumAllowed) != 0;
    const ACCESS_MASK desired = MapGeneric(DesiredAccess & ~kMaximumAllowed, GenericMapping);

    auto deny = [&](NTSTATUS why) { return finish(0, why); };

    if (desired & kAccessSystemSecurity) {
        bool held = false;
        for (const auto& p : t->privs) if (p.luid.LowPart == pSecurity && (p.attrs & kPrvEnabled)) held = true;
        if (!held) return deny(kPrivilegeNotHeld);
    }

    ACCESS_MASK granted = 0, denied = 0;
    if (!sd.dacl_present || !sd.dacl) {
        // NULL DACL: everything is granted (ACCESS_SYSTEM_SECURITY was checked above).
        granted = maximum ? (MapGeneric(kGenericAll, GenericMapping) | 0x001F0000) : desired;
        return finish(granted, kSuccess);
    }
    if (SidAppliesAllow(*t, sd.owner)) granted |= (kReadControl | kWriteDac);

    const std::uint8_t* acl = sd.dacl;
    WORD aclSize, aceCount;
    std::memcpy(&aclSize, acl + 2, 2);
    std::memcpy(&aceCount, acl + 4, 2);
    const std::uint8_t* ace = acl + 8;
    ACCESS_MASK remaining = desired & ~granted;
    for (WORD i = 0; i < aceCount; ++i) {
        if (ace + 8 > acl + aclSize) return kInvalidSecurityDescr;
        const std::uint8_t type = ace[0], flags = ace[1];
        WORD size;
        std::memcpy(&size, ace + 2, 2);
        if (size < 8 || ace + size > acl + aclSize) return kInvalidSecurityDescr;
        if (!(flags & 0x08) && (type == 0 || type == 1)) {   // not INHERIT_ONLY; ACCESS_ALLOWED / ACCESS_DENIED
            ACCESS_MASK mask;
            std::memcpy(&mask, ace + 4, 4);
            mask = MapGeneric(mask, GenericMapping);
            const void* sid = ace + 8;
            if (type == 0 && SidAppliesAllow(*t, sid)) {
                granted |= mask & ~denied;
                remaining &= ~mask;
            } else if (type == 1 && SidAppliesDeny(*t, sid)) {
                if (!maximum && (mask & remaining & ~granted)) return deny(kAccessDenied);
                denied |= mask & ~granted;
            }
        }
        ace += size;
        if (!maximum && (desired & ~granted) == 0) break;
    }
    granted &= ~kAccessSystemSecurity | (desired & kAccessSystemSecurity);
    if (maximum) {
        if ((desired & ~granted) != 0) return deny(kAccessDenied);
        return finish(granted, granted ? kSuccess : kAccessDenied);
    }
    if ((desired & ~granted) != 0) return deny(kAccessDenied);
    return finish(desired, kSuccess);
}

NTSTATUS Tok_NtCompareTokens(HANDLE FirstTokenHandle, HANDLE SecondTokenHandle, PBOOLEAN Equal) {
    EnsureReady();
    if (NTSTATUS ps = probe::ProbeWrite(Equal, sizeof(BOOLEAN))) return ps;
    Lock l;
    TokPtr a, b;
    NTSTATUS st = Resolve(FirstTokenHandle, kTokQuery, &a);
    if (st != kSuccess) return st;
    st = Resolve(SecondTokenHandle, kTokQuery, &b);
    if (st != kSuccess) return st;
    bool eq = a == b;
    if (!eq) {
        auto same_groups = [](const std::vector<Group>& x, const std::vector<Group>& y) {
            if (x.size() != y.size()) return false;
            for (const auto& g : x) {
                auto it = std::find_if(y.begin(), y.end(), [&](const Group& h) { return h.sid == g.sid; });
                if (it == y.end() || ((it->attrs ^ g.attrs) & (kGrpEnabled | kGrpUseForDenyOnly))) return false;
            }
            return true;
        };
        auto same_privs = [](const std::vector<Priv>& x, const std::vector<Priv>& y) {
            if (x.size() != y.size()) return false;
            for (const auto& p : x) {
                auto it = std::find_if(y.begin(), y.end(), [&](const Priv& q) { return LuidEq(q.luid, p.luid); });
                if (it == y.end() || ((it->attrs ^ p.attrs) & kPrvEnabled)) return false;
            }
            return true;
        };
        eq = a->user == b->user && a->integrity_rid == b->integrity_rid && a->session == b->session &&
             same_groups(a->groups, b->groups) && same_groups(a->restricted, b->restricted) &&
             same_privs(a->privs, b->privs);
    }
    return probe::Store<BOOLEAN>(Equal, eq ? TRUE : FALSE);
}

NTSTATUS Tok_NtCreateLowBoxToken(PHANDLE NewTokenHandle, HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, PSID PackageSid, ULONG CapabilityCount,
                                 PSID_AND_ATTRIBUTES Capabilities, ULONG HandleCount, HANDLE* Handles) {
    (void)DesiredAccess; (void)ObjectAttributes; (void)Handles;
    EnsureReady();
    if (NTSTATUS ps = probe::Store<HANDLE>(NewTokenHandle, nullptr)) return ps;   // probes, and clears
    if (!PackageSid || (CapabilityCount && !Capabilities) || CapabilityCount > 0x1000 || HandleCount > 0x1000)
        return kInvalidParameter;
    {
        Lock l;
        TokPtr src;
        const NTSTATUS st = Resolve(ExistingTokenHandle, kTokDuplicate, &src);
        if (st != kSuccess) return st;
    }
    // A LowBox token that did not report TokenIsAppContainer would be a lie, and one that did would contradict the
    // identity every other token query gives: refuse, so a sandboxing program sees the failure it can handle.
    FIXME(token, "NtCreateLowBoxToken: AppContainer tokens are not emulated (%lu capabilities, %lu handles)",
          static_cast<unsigned long>(CapabilityCount), static_cast<unsigned long>(HandleCount));
    return kNotSupported;
}

NTSTATUS Tok_NtQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                   ULONG Length, PULONG LengthNeeded) {
    EnsureReady();
    Lock l;
    TokPtr t;
    ACCESS_MASK granted = 0;
    NTSTATUS st = Resolve(Handle, 0, &t, &granted);
    if (st != kSuccess) return st;
    // OWNER=1 GROUP=2 DACL=4 SACL=8 LABEL=0x10 ATTRIBUTE=0x20 SCOPE=0x40 PROCESS_TRUST=0x80 ACCESS_FILTER=0x100
    const SECURITY_INFORMATION known = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100;
    if (SecurityInformation & ~known) return kInvalidParameter;
    if ((SecurityInformation & 0x8) && !(granted & kAccessSystemSecurity)) return kAccessDenied;
    if ((SecurityInformation & (0x1 | 0x2 | 0x4 | 0x10)) && !(granted & kReadControl)) return kAccessDenied;
    const auto sd = BuildTokenSd(*t, SecurityInformation & ~0x8u);
    if (NTSTATUS ps = probe::ProbeWrite(SecurityDescriptor, Length)) return ps;
    if (LengthNeeded)
        if (NTSTATUS ws = probe::Store<ULONG>(LengthNeeded, static_cast<ULONG>(sd.size()))) return ws;
    if (Length < sd.size()) return kBufferTooSmall;
    return probe::CopyToUser(SecurityDescriptor, sd.data(), sd.size());
}

NTSTATUS Close(HANDLE TokenHandle) {
    EnsureReady();
    if (TokenHandle == kPseudoProcessToken || TokenHandle == kPseudoThreadToken || TokenHandle == kPseudoEffectiveToken)
        return kSuccess;   // closing a pseudo handle is a no-op
    if (!IsTokenHandle(TokenHandle)) return kObjectTypeMismatch;
    Lock l;
    auto it = g_handles.find(FromHandle(TokenHandle));
    if (it == g_handles.end()) return kInvalidHandle;
    if (it->second.permanent) return kSuccess;   // the module's own process-token handle is never torn down
    g_handles.erase(it);
    return kSuccess;
}

}  // namespace win10xbox::token
