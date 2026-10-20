// probe_H_token.cpp - category H: process-token / sandbox introspection.
//
// Opens the current process token (TOKEN_QUERY) and reads the information
// classes that describe the sandbox walls: integrity level, AppContainer flag
// and package SID, granted capability SIDs, groups, privileges, user SID,
// elevation and session. Read-only; the token handle is closed at the end.
//
// No link libs: GetTokenInformation / LookupPrivilegeNameW are resolved at
// runtime (advapi32 on win32, kernelbase/advapi32 forwarders under UWP), and SIDs
// are formatted by hand from the raw SID bytes (no sddl.h / ConvertSidToStringSid).
#include "probe.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace probe {
namespace {

using GetTokenInformation_t =
    BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
using OpenProcessToken_t = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
using LookupPrivilegeNameW_t = BOOL(WINAPI*)(LPCWSTR, PLUID, LPWSTR, LPDWORD);

struct Ctx {
    HANDLE token = nullptr;
    DWORD  open_err = 0;  // GetLastError() from the open attempt (0 = opened)
    bool   api_missing = false;
    GetTokenInformation_t  get_info = nullptr;
    LookupPrivilegeNameW_t lookup_priv = nullptr;
};

template <class F>
F find_api(const char* sym) {
    static const char* const mods[] = {"advapi32.dll", "kernelbase.dll", "sechost.dll"};
    for (const char* m : mods)
        if (void* p = resolve(m, sym)) return reinterpret_cast<F>(p);
    return nullptr;
}

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

Status classify(DWORD e) {
    switch (e) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return Status::DENIED;
    case ERROR_INVALID_PARAMETER:  // info class not supported
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return Status::NOTIMPL;
    default:
        return Status::WARN;
    }
}

void fail(Record& rec, DWORD e, const char* what) {
    rec.errorcode = e;
    rec.status = classify(e);
    rec.detail = std::string(what) + " failed: " + error_text(e);
}

// --- SID helpers working on raw bytes (SID layout is a stable ABI) ----------

// S-1-<auth>-<sub>-...  (authority printed decimal if it fits 32 bits, else hex)
std::string sid_str(const void* sid) {
    if (!sid) return "<null>";
    const BYTE* b = static_cast<const BYTE*>(sid);
    const unsigned rev = b[0], cnt = b[1];
    if (rev != 1 || cnt > SID_MAX_SUB_AUTHORITIES) return "<invalid-sid>";
    std::string s = fmt("S-%u-", rev);
    if (b[2] == 0 && b[3] == 0) {
        const unsigned long long a = ((unsigned long long)b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
        s += fmt("%llu", a);
    } else {
        s += fmt("0x%02X%02X%02X%02X%02X%02X", b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    for (unsigned i = 0; i < cnt; i++) {
        DWORD v;
        memcpy(&v, b + 8 + 4 * i, 4);
        s += fmt("-%lu", (unsigned long)v);
    }
    return s;
}

DWORD sid_last_sub(const void* sid) {
    const BYTE* b = static_cast<const BYTE*>(sid);
    const unsigned cnt = b[1];
    if (!cnt || cnt > SID_MAX_SUB_AUTHORITIES) return 0;
    DWORD v;
    memcpy(&v, b + 8 + 4 * (cnt - 1), 4);
    return v;
}

// Legacy named capabilities live at S-1-15-3-<n>; name the ones we know.
const char* known_capability(const std::string& s) {
    static const std::map<std::string, const char*> k = {
        {"S-1-15-3-1", "internetClient"},
        {"S-1-15-3-2", "internetClientServer"},
        {"S-1-15-3-3", "privateNetworkClientServer"},
        {"S-1-15-3-4", "picturesLibrary"},
        {"S-1-15-3-5", "videosLibrary"},
        {"S-1-15-3-6", "musicLibrary"},
        {"S-1-15-3-7", "documentsLibrary"},
        {"S-1-15-3-8", "enterpriseAuthentication"},
        {"S-1-15-3-9", "sharedUserCertificates"},
        {"S-1-15-3-10", "removableStorage"},
        {"S-1-15-3-11", "appointments"},
        {"S-1-15-3-12", "contacts"},
    };
    auto it = k.find(s);
    return it == k.end() ? nullptr : it->second;
}

// --- token query -------------------------------------------------------------

// Fetch one info class. On failure fills 'rec' (status/errorcode/detail) and
// returns false, so callers just `return` and the record is already classified.
bool fetch(Ctx& c, Record& rec, TOKEN_INFORMATION_CLASS cls, const char* what,
           std::vector<BYTE>& buf) {
    if (c.api_missing || !c.get_info) {
        rec.status = Status::MISSING;
        rec.detail = "GetTokenInformation not resolvable (advapi32/kernelbase)";
        return false;
    }
    if (!c.token) {
        rec.errorcode = c.open_err;
        rec.status = classify(c.open_err);
        rec.detail = "no token handle: OpenProcessToken failed: " + error_text(c.open_err);
        return false;
    }
    // Size query first, then an exact-size buffer: some fixed-size classes
    // (e.g. TokenElevation) reject an oversized buffer, so never guess a size.
    DWORD need = 0;
    c.get_info(c.token, cls, nullptr, 0, &need);
    DWORD e = GetLastError();
    for (int attempt = 0; attempt < 4; attempt++) {
        if ((e != ERROR_INSUFFICIENT_BUFFER && e != ERROR_BAD_LENGTH) || need == 0) {
            fail(rec, e ? e : ERROR_INSUFFICIENT_BUFFER, what);
            return false;
        }
        buf.assign(need, 0);
        if (c.get_info(c.token, cls, buf.data(), (DWORD)buf.size(), &need)) return true;
        e = GetLastError();  // token info can grow between calls; loop re-sizes
    }
    fail(rec, e, what);
    return false;
}

const char* integrity_label(DWORD rid) {
    switch (rid) {
    case 0x0000: return "untrusted";
    case 0x1000: return "low";
    case 0x2000: return "medium";
    case 0x2100: return "medium-plus";
    case 0x3000: return "high";
    case 0x4000: return "system";
    case 0x5000: return "protected";
    default:     return "other";
    }
}

// Well-known privilege LUIDs (stable since NT), used if name lookup is unavailable.
const char* priv_fallback(DWORD low) {
    static const char* const t[] = {
        nullptr, nullptr, "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege",
        "SeLockMemoryPrivilege", "SeIncreaseQuotaPrivilege", "SeMachineAccountPrivilege",
        "SeTcbPrivilege", "SeSecurityPrivilege", "SeTakeOwnershipPrivilege",
        "SeLoadDriverPrivilege", "SeSystemProfilePrivilege", "SeSystemtimePrivilege",
        "SeProfileSingleProcessPrivilege", "SeIncreaseBasePriorityPrivilege",
        "SeCreatePagefilePrivilege", "SeCreatePermanentPrivilege", "SeBackupPrivilege",
        "SeRestorePrivilege", "SeShutdownPrivilege", "SeDebugPrivilege", "SeAuditPrivilege",
        "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege", "SeRemoteShutdownPrivilege",
        "SeUndockPrivilege", "SeSyncAgentPrivilege", "SeEnableDelegationPrivilege",
        "SeManageVolumePrivilege", "SeImpersonatePrivilege", "SeCreateGlobalPrivilege",
        "SeTrustedCredManAccessPrivilege", "SeRelabelPrivilege", "SeIncreaseWorkingSetPrivilege",
        "SeTimeZonePrivilege", "SeCreateSymbolicLinkPrivilege",
        "SeDelegateSessionUserImpersonatePrivilege"};
    return low < sizeof(t) / sizeof(t[0]) ? t[low] : nullptr;
}

std::string priv_name(Ctx& c, const LUID& l) {
    if (c.lookup_priv) {
        wchar_t name[128] = {};
        DWORD n = 128;
        LUID copy = l;
        if (c.lookup_priv(nullptr, &copy, name, &n)) return narrow(name);
    }
    if (l.HighPart == 0)
        if (const char* f = priv_fallback(l.LowPart)) return f;
    return fmt("luid=0x%lx", (unsigned long)l.LowPart);
}

}  // namespace

void register_H_token(Reporter& r) {
    Ctx c;
    c.get_info = find_api<GetTokenInformation_t>("GetTokenInformation");
    c.lookup_priv = find_api<LookupPrivilegeNameW_t>("LookupPrivilegeNameW");
    c.api_missing = (c.get_info == nullptr);

    // 0. open the token (TOKEN_QUERY) - everything below shares this handle.
    r.run("H", "token-open", [&](Record& rec) {
        auto open_fn = find_api<OpenProcessToken_t>("OpenProcessToken");
        if (!open_fn) {
            c.api_missing = true;
            rec.status = Status::MISSING;
            rec.detail = "OpenProcessToken not resolvable";
            return;
        }
        HANDLE h = nullptr;
        if (open_fn(GetCurrentProcess(), TOKEN_QUERY, &h)) {
            c.token = h;
            rec.status = Status::OK;
            rec.detail = "OpenProcessToken(TOKEN_QUERY) succeeded";
            rec.value = "access=TOKEN_QUERY";
        } else {
            c.open_err = GetLastError();
            rec.errorcode = c.open_err;
            rec.status = classify(c.open_err);
            rec.detail = "OpenProcessToken(TOKEN_QUERY) failed: " + error_text(c.open_err);
        }
    });

    // 1. mandatory integrity level
    r.run("H", "integrity-level", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenIntegrityLevel, "GetTokenInformation(TokenIntegrityLevel)", b)) return;
        const auto* tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(b.data());
        const DWORD rid = sid_last_sub(tml->Label.Sid);
        rec.status = Status::OK;
        rec.value = fmt("rid=0x%x;label=%s", (unsigned)rid, integrity_label(rid));
        rec.detail = "mandatory label SID " + sid_str(tml->Label.Sid) + " (AppContainer is typically low)";
    });

    // 2. AppContainer flag
    r.run("H", "is-appcontainer", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenIsAppContainer, "GetTokenInformation(TokenIsAppContainer)", b)) return;
        DWORD v = 0;
        memcpy(&v, b.data(), sizeof v);
        rec.status = Status::OK;
        rec.value = fmt("appcontainer=%u", v ? 1u : 0u);
        rec.detail = v ? "token is an AppContainer token (UWP-style sandbox)"
                       : "token is NOT an AppContainer token (desktop-style)";
    });

    // 3. AppContainer package SID
    r.run("H", "appcontainer-sid", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenAppContainerSid, "GetTokenInformation(TokenAppContainerSid)", b)) return;
        const auto* ac = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(b.data());
        rec.status = Status::OK;
        if (!ac->TokenAppContainer) {
            rec.value = "sid=none";
            rec.detail = "no AppContainer SID (token is not an AppContainer)";
        } else {
            rec.value = "sid=" + sid_str(ac->TokenAppContainer);
            rec.detail = "AppContainer package SID (S-1-15-2-...)";
        }
    });

    // 4. capability SIDs - the granted capabilities
    r.run("H", "capabilities", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenCapabilities, "GetTokenInformation(TokenCapabilities)", b)) return;
        const auto* g = reinterpret_cast<const TOKEN_GROUPS*>(b.data());
        std::string first, all;
        unsigned named = 0;
        for (DWORD i = 0; i < g->GroupCount; i++) {
            const std::string s = sid_str(g->Groups[i].Sid);
            if (i < 8) first += (i ? "," : "") + s;
            if (i) all += ", ";
            all += s;
            if (const char* n = known_capability(s)) {
                all += std::string("(") + n + ")";
                named++;
            }
        }
        rec.status = Status::OK;
        rec.value = fmt("count=%lu;sids=", (unsigned long)g->GroupCount) + first;
        rec.detail = g->GroupCount
            ? fmt("%lu capability SID(s), %u legacy-named. All: ", (unsigned long)g->GroupCount, named) + all
            : "no capability SIDs on this token";
    });

    // 5. groups
    r.run("H", "token-groups", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenGroups, "GetTokenInformation(TokenGroups)", b)) return;
        const auto* g = reinterpret_cast<const TOKEN_GROUPS*>(b.data());
        unsigned enabled = 0, deny_only = 0, integrity = 0;
        std::string list;
        for (DWORD i = 0; i < g->GroupCount; i++) {
            const DWORD a = g->Groups[i].Attributes;
            if (a & SE_GROUP_ENABLED) enabled++;
            if (a & SE_GROUP_USE_FOR_DENY_ONLY) deny_only++;
            if (a & SE_GROUP_INTEGRITY) integrity++;
            if (i < 12) list += fmt("%s%s[0x%lx]", i ? ", " : "", sid_str(g->Groups[i].Sid).c_str(), (unsigned long)a);
        }
        rec.status = Status::OK;
        rec.value = fmt("count=%lu", (unsigned long)g->GroupCount);
        rec.detail = fmt("enabled=%u deny-only=%u integrity=%u; first %u: ", enabled, deny_only, integrity,
                         g->GroupCount < 12 ? (unsigned)g->GroupCount : 12u) + list;
    });

    // 6. privileges
    r.run("H", "privileges", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenPrivileges, "GetTokenInformation(TokenPrivileges)", b)) return;
        const auto* p = reinterpret_cast<const TOKEN_PRIVILEGES*>(b.data());
        std::string list;
        for (DWORD i = 0; i < p->PrivilegeCount; i++) {
            const bool en = (p->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
            if (i) list += ",";
            list += priv_name(c, p->Privileges[i].Luid) + (en ? ":en" : ":dis");
        }
        rec.status = Status::OK;
        rec.value = fmt("count=%lu;privs=", (unsigned long)p->PrivilegeCount) + list;
        rec.detail = c.lookup_priv ? "names via LookupPrivilegeNameW" : "names via built-in LUID table (LookupPrivilegeNameW unavailable)";
    });

    // 7. user SID
    r.run("H", "user-sid", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenUser, "GetTokenInformation(TokenUser)", b)) return;
        const auto* u = reinterpret_cast<const TOKEN_USER*>(b.data());
        rec.status = Status::OK;
        rec.value = "sid=" + sid_str(u->User.Sid);
        rec.detail = "token user SID";
    });

    // 8. elevation (two classes; report whatever succeeds)
    r.run("H", "elevation", [&](Record& rec) {
        std::vector<BYTE> b;
        std::string el = "?", ty = "?";
        Record e1, e2;  // scratch for classification of each class
        bool ok1 = fetch(c, e1, TokenElevation, "GetTokenInformation(TokenElevation)", b);
        if (ok1) {
            DWORD v = 0;
            memcpy(&v, b.data(), sizeof v);
            el = fmt("%u", v ? 1u : 0u);
        }
        bool ok2 = fetch(c, e2, TokenElevationType, "GetTokenInformation(TokenElevationType)", b);
        if (ok2) {
            DWORD v = 0;
            memcpy(&v, b.data(), sizeof v);
            ty = fmt("%lu", (unsigned long)v);
        }
        rec.value = "elevated=" + el + ";type=" + ty;
        if (ok1 && ok2) {
            rec.status = Status::OK;
            rec.detail = "type: 1=default 2=full 3=limited";
        } else {
            const Record& bad = ok1 ? e2 : e1;
            rec.status = bad.status;
            rec.errorcode = bad.errorcode;
            rec.detail = bad.detail + (ok1 || ok2 ? " (partial)" : "");
        }
    });

    // 9. session id
    r.run("H", "session-id", [&](Record& rec) {
        std::vector<BYTE> b;
        if (!fetch(c, rec, TokenSessionId, "GetTokenInformation(TokenSessionId)", b)) return;
        DWORD v = 0;
        memcpy(&v, b.data(), sizeof v);
        rec.status = Status::OK;
        rec.value = fmt("session=%lu", (unsigned long)v);
        rec.detail = "token session id";
    });

    if (c.token) {
        CloseHandle(c.token);
        c.token = nullptr;
    }
}

}  // namespace probe
