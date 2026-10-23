// The object manager namespace (docs/DISPATCHER.md "Object manager namespace").
//
//   directory objects and symbolic links   NtCreate/Open DirectoryObject, NtQueryDirectoryObject,
//                                          NtCreate/Open/Query SymbolicLinkObject: the tree in ob_namespace.cpp. Their
//                                          handles are emulated (ObjectKind::Directory / SymbolicLink).
//   named synchronisation objects          events, mutexes, semaphores, timers, sections, keyed events, I/O
//                                          completions, jobs, reserve objects: the HOST creates them (so waits, access
//                                          and security stay real), under one name in the host's own named-object
//                                          directory that encodes the guest path. A child process is another host
//                                          process, so it finds the object by the same guest name.
//   NtQueryObject                          names of those objects are decoded back to the guest path, directory and
//                                          link handles are answered here.
//
// Names that are not in the tree (\Device\..., \Registry\..., \KnownDlls...) stay with the host/VFS exactly as before.
#include "internal.h"
#include "ob_namespace.h"
#include "win10xbox/config.h"
#include "win10xbox/probe.h"
#if WIN10XBOX_HAVE_VFS
#include "win10xbox/vfs.h"
#endif
#if WIN10XBOX_HAVE_REGISTRY
#include "win10xbox/registry.h"
#endif
#if WIN10XBOX_HAVE_TOKEN
#include "win10xbox/token.h"
#endif
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <unordered_map>

namespace win10xbox::dispatch::detail {
namespace {

using ob::Kind;
using ob::NodeRef;

namespace nts {
constexpr NTSTATUS DatatypeMisalignment = static_cast<NTSTATUS>(0x80000002);
constexpr NTSTATUS MoreEntries = 0x00000105;
constexpr NTSTATUS NameExists = 0x40000000;
constexpr NTSTATUS NameCollision = static_cast<NTSTATUS>(0xC0000035);
constexpr NTSTATUS PathSyntaxBad = static_cast<NTSTATUS>(0xC000003B);
constexpr NTSTATUS NameInvalid = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS NotImplemented = static_cast<NTSTATUS>(0xC0000002);
}  // namespace nts

constexpr ULONG kObjInherit = 0x2, kObjPermanent = 0x10, kObjCaseInsensitive = 0x40, kObjOpenIf = 0x80;
constexpr ULONG kObjectBasicInformation = 0, kObjectNameInformation = 1, kObjectTypeInformation = 2,
                kObjectTypesInformation = 3;

// ---- state -------------------------------------------------------------------------------------------------------------

ob::Tree g_tree;

// How the host names things for this layer (a tree of processes started from one another shares it).
struct HostNs {
    bool ready = false;
    std::wstring layer;     // 16 hex digits, one per layer
    std::wstring prefix;    // L"W10X" + layer
    std::wstring base;      // the host's named-object directory for this process ("\Sessions\1\BaseNamedObjects")
    bool ac = false;
    std::wstring ac_sid;
};
HostNs g_host;

struct SyncRec {
    NodeRef dir;            // pinned for as long as the handle is open
    std::wstring leaf;
    std::wstring type;
};
SRWLOCK g_sync_lock = SRWLOCK_INIT;
std::unordered_map<std::uintptr_t, SyncRec> g_sync;
std::unordered_map<std::wstring, std::wstring> g_long_names;   // hashed host name digits -> guest path

struct HandleRec {
    NodeRef node;
    ACCESS_MASK access = 0;
    ULONG attributes = 0;
};
SRWLOCK g_rec_lock = SRWLOCK_INIT;
volatile LONG g_handle_count[2];   // directory, symbolic link handles open

template <class Fn> Fn HostProc(const char* name) { return reinterpret_cast<Fn>(HostExport(name)); }

// ---- guest memory --------------------------------------------------------------------------------------------------------

// Caller memory goes through win10xbox/probe.h: a bad pointer is STATUS_ACCESS_VIOLATION, never a fault in the layer.
bool GuestReadable(const void* p, std::size_t n) { return probe::ProbeRead(p, n) == 0; }
bool GuestWritable(void* p, std::size_t n) { return probe::ProbeWrite(p, n) == 0; }
void PutHandle(PHANDLE h, HANDLE v) { probe::Store<HANDLE>(h, v); }
void PutUlong(PULONG p, ULONG v) { if (p) probe::Store<ULONG>(p, v); }

// ---- handles ------------------------------------------------------------------------------------------------------------

ACCESS_MASK MapAccess(ACCESS_MASK a, Kind kind) {
    const bool dir = kind == Kind::Directory;
    const ACCESS_MASK all = dir ? ob::kDirectoryAllAccess : ob::kSymlinkAllAccess;
    const ACCESS_MASK valid = dir ? 0x1F000F : 0x1F0001;
    if (a & ob::kMaximumAllowed) return all;
    if (a & GENERIC_READ) a |= dir ? 0x20003 : 0x20001;
    if (a & GENERIC_WRITE) a |= dir ? 0x2000C : 0x20000;
    if (a & GENERIC_EXECUTE) a |= dir ? 0x20003 : 0x20001;
    if (a & GENERIC_ALL) a |= all;
    return a & valid;
}

HANDLE NewHandle(const NodeRef& node, ACCESS_MASK access, ULONG attributes) {
    auto* rec = new HandleRec{node, MapAccess(access, node->kind), attributes};
    const ObjectKind k = node->kind == Kind::Directory ? ObjectKind::Directory : ObjectKind::SymbolicLink;
    HANDLE h = RegisterEmulatedObject(k, node->serial, rec);
    if (!h) {
        delete rec;
        return nullptr;
    }
    g_tree.Pin(node);
    InterlockedIncrement(&g_handle_count[node->kind == Kind::Directory ? 0 : 1]);
    return h;
}

bool OurHandle(HANDLE h, HandleRec* out, ObjectKind* kind = nullptr) {
    EmulatedObject o;
    if (!h || !LookupEmulatedObject(h, &o)) return false;
    if ((o.kind != ObjectKind::Directory && o.kind != ObjectKind::SymbolicLink) || !o.extra) return false;
    AcquireSRWLockShared(&g_rec_lock);
    if (out) *out = *static_cast<HandleRec*>(o.extra);
    ReleaseSRWLockShared(&g_rec_lock);
    if (kind) *kind = o.kind;
    return true;
}

// ---- parsing OBJECT_ATTRIBUTES --------------------------------------------------------------------------------------------

struct ParsedOA {
    NTSTATUS status = 0;        // non-zero: the call returns it
    bool null_oa = false, null_name = false, empty_name = false;
    std::wstring name;          // captured copy of the caller's name
    HANDLE root_handle = nullptr;
    bool root_ours = false;
    HandleRec root;
    ULONG attributes = 0;
    bool ci() const { return (attributes & kObjCaseInsensitive) != 0; }
};

// What the kernel checks about an OBJECT_ATTRIBUTES before it looks at the name, in the order it does (om.c).
ParsedOA ParseOA(const OBJECT_ATTRIBUTES* oa) {
    ParsedOA p;
    if (!oa) { p.null_oa = true; return p; }
    // The attributes and the name are read once; everything below works on the copy.
    probe::ObjectAttributes cap;
    if (NTSTATUS cs = probe::CaptureObjectAttributes(oa, &cap)) { p.status = cs; return p; }
    p.attributes = cap.oa.Attributes;
    p.root_handle = cap.oa.RootDirectory;
    if (p.root_handle) p.root_ours = OurHandle(p.root_handle, &p.root);
    if (!cap.oa.ObjectName) { p.null_name = true; return p; }
    const USHORT length = cap.name.us.Length;
    if ((length & 1) || length > 65532) { p.status = nts::NameInvalid; return p; }
    p.name = cap.name.text;
    p.empty_name = length == 0;
    return p;
}

// ---- the host side of a named object -------------------------------------------------------------------------------------

std::wstring AnonymousKey(const NodeRef& dir) {
    return L"\\?" + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(dir->serial);
}

struct SyncPlan {
    enum class Route { Forward, Done, Redirect } route = Route::Forward;
    NTSTATUS status = 0;
    OBJECT_ATTRIBUTES oa{};
    UNICODE_STRING us{};
    std::wstring host_name;
    NodeRef dir;
    std::wstring leaf;
    bool hashed = false;
    bool anonymous = false;
    SyncPlan() = default;
    SyncPlan(const SyncPlan&) = delete;
    SyncPlan& operator=(const SyncPlan&) = delete;
};

void SetHostOA(SyncPlan* plan, const OBJECT_ATTRIBUTES* in, bool anonymous) {
    plan->oa = *in;
    plan->oa.RootDirectory = nullptr;
    if (anonymous) {
        plan->oa.ObjectName = nullptr;
        return;
    }
    plan->us.Buffer = plan->host_name.data();
    plan->us.Length = static_cast<USHORT>(plan->host_name.size() * sizeof(wchar_t));
    plan->us.MaximumLength = static_cast<USHORT>(plan->us.Length + sizeof(wchar_t));
    plan->oa.ObjectName = &plan->us;
}

void PlanSync(const OBJECT_ATTRIBUTES* oa, bool create, SyncPlan* plan) {
    ParsedOA p = ParseOA(oa);
    if (p.status) { plan->route = SyncPlan::Route::Done; plan->status = p.status; return; }
    if (p.null_oa) {
        if (create) return;   // an unnamed object: the host's business
        plan->route = SyncPlan::Route::Done;
        plan->status = st::InvalidParameter;
        return;
    }
    if (p.null_name) {
        if (p.root_handle) { plan->route = SyncPlan::Route::Done; plan->status = nts::NameInvalid; return; }
        if (create) return;
        plan->route = SyncPlan::Route::Done;
        plan->status = nts::PathSyntaxBad;
        return;
    }
    if (p.empty_name) {
        if (p.root_ours) {
            if (create) {   // an unnamed object, whatever the root says
                SetHostOA(plan, oa, true);
                plan->route = SyncPlan::Route::Redirect;
                plan->anonymous = true;
                return;
            }
            plan->route = SyncPlan::Route::Done;
            plan->status = st::ObjectTypeMismatch;
            return;
        }
        if (!create && !p.root_handle) {
            plan->route = SyncPlan::Route::Done;
            plan->status = nts::PathSyntaxBad;
        }
        return;   // the host judges a root of its own
    }
    if (p.root_handle && !p.root_ours) return;   // relative to a host object
    if (!g_host.ready) return;
    ob::Resolution r = g_tree.Resolve(p.root_ours ? p.root.node : NodeRef(), p.name, p.ci(), true);
    if (r.status) { plan->route = SyncPlan::Route::Done; plan->status = r.status; return; }
    if (r.foreign) return;
    if (r.Found()) {
        plan->route = SyncPlan::Route::Done;
        plan->status = create && !(p.attributes & kObjOpenIf) ? nts::NameCollision : st::ObjectTypeMismatch;
        return;
    }
    // A free name in one of our directories: the host object that carries it.
    std::wstring dir_path = r.dir_path.empty() ? AnonymousKey(r.node) : r.dir_path;
    std::wstring guest_path = dir_path + L"\\" + r.leaf;
    bool hashed = false;
    plan->host_name = g_host.base + L"\\" + ob::MangleName(g_host.prefix, guest_path, &hashed);
    plan->hashed = hashed;
    plan->dir = r.node;
    plan->leaf = r.leaf;
    if (hashed) {
        std::wstring digits = plan->host_name.substr(plan->host_name.size() - 16);
        AcquireSRWLockExclusive(&g_sync_lock);
        g_long_names[digits] = guest_path;
        ReleaseSRWLockExclusive(&g_sync_lock);
    }
    SetHostOA(plan, oa, false);
    plan->route = SyncPlan::Route::Redirect;
}

void RecordSync(HANDLE h, const SyncPlan& plan, const wchar_t* type) {
    if (!h || !plan.dir) return;
    g_tree.Pin(plan.dir);
    AcquireSRWLockExclusive(&g_sync_lock);
    auto [it, inserted] = g_sync.try_emplace(reinterpret_cast<std::uintptr_t>(h));
    NodeRef old = inserted ? NodeRef() : it->second.dir;
    it->second = SyncRec{plan.dir, plan.leaf, type};
    ReleaseSRWLockExclusive(&g_sync_lock);
    if (old) g_tree.Unpin(old);   // a handle value reused without a close we saw
}

// Runs `call(oa, &out)` for a create/open of a named kernel object with the name translated. `call` is the host's
// syscall with every other argument already bound.
template <class F>
NTSTATUS SyncCall(const wchar_t* type, bool create, PHANDLE handle, POBJECT_ATTRIBUTES oa, F&& call) {
    if (!handle || !GuestWritable(handle, sizeof(HANDLE))) return st::AccessViolation;
    SyncPlan plan;
    PlanSync(oa, create, &plan);
    switch (plan.route) {
    case SyncPlan::Route::Forward:
        return call(oa, handle);
    case SyncPlan::Route::Done: {
        if (create) {
            // The other arguments are checked before the name (om.c: an invalid event type is
            // STATUS_INVALID_PARAMETER whatever the name): let the host judge them on an unnamed object.
            HANDLE scratch = nullptr;
            NTSTATUS ps = call(nullptr, &scratch);
            if (NtSuccess(ps)) {
                Host().NtClose(scratch);
            } else {
                PutHandle(handle, nullptr);
                return ps;
            }
        }
        PutHandle(handle, nullptr);
        return plan.status;
    }
    case SyncPlan::Route::Redirect: {
        NTSTATUS s = call(&plan.oa, handle);
        if (NtSuccess(s)) {
            HANDLE made = nullptr;
            if (!plan.anonymous && probe::Load<HANDLE>(handle, &made) == 0) RecordSync(made, plan, type);
        }
        return s;
    }
    }
    return st::Unsuccessful;
}

#define SYNC_HOST_FN(Sig, name)                                    \
    auto host = reinterpret_cast<Sig>(HostExport(name));           \
    if (!host) return nts::NotImplemented

NTSTATUS NTAPI Xl_NtCreateEvent(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG type, BOOLEAN state) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN), "NtCreateEvent");
    return SyncCall(L"Event", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, type, state); });
}
NTSTATUS NTAPI Xl_NtOpenEvent(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenEvent");
    return SyncCall(L"Event", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateMutant(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, BOOLEAN owned) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN), "NtCreateMutant");
    return SyncCall(L"Mutant", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, owned); });
}
NTSTATUS NTAPI Xl_NtOpenMutant(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenMutant");
    return SyncCall(L"Mutant", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateSemaphore(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, LONG initial, LONG maximum) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG, LONG), "NtCreateSemaphore");
    return SyncCall(L"Semaphore", true, h, oa,
                    [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, initial, maximum); });
}
NTSTATUS NTAPI Xl_NtOpenSemaphore(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenSemaphore");
    return SyncCall(L"Semaphore", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateTimer(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG type) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG), "NtCreateTimer");
    return SyncCall(L"Timer", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, type); });
}
NTSTATUS NTAPI Xl_NtOpenTimer(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenTimer");
    return SyncCall(L"Timer", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtOpenSection(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenSection");
    return SyncCall(L"Section", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateKeyedEvent(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG flags) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG), "NtCreateKeyedEvent");
    return SyncCall(L"KeyedEvent", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, flags); });
}
NTSTATUS NTAPI Xl_NtOpenKeyedEvent(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenKeyedEvent");
    return SyncCall(L"KeyedEvent", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateIoCompletion(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG count) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG), "NtCreateIoCompletion");
    return SyncCall(L"IoCompletion", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o, count); });
}
NTSTATUS NTAPI Xl_NtOpenIoCompletion(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenIoCompletion");
    return SyncCall(L"IoCompletion", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtCreateJobObject(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtCreateJobObject");
    return SyncCall(L"Job", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtOpenJobObject(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES), "NtOpenJobObject");
    return SyncCall(L"Job", false, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, access, o); });
}
NTSTATUS NTAPI Xl_NtAllocateReserveObject(PHANDLE h, POBJECT_ATTRIBUTES oa, ULONG type) {
    SYNC_HOST_FN(NTSTATUS(NTAPI*)(PHANDLE, POBJECT_ATTRIBUTES, ULONG), "NtAllocateReserveObject");
    return SyncCall(L"IoCompletionReserve", true, h, oa, [&](POBJECT_ATTRIBUTES o, PHANDLE out) { return host(out, o, type); });
}

// ---- host directory listings ---------------------------------------------------------------------------------------------

// The entries of a host object directory (name, type name), empty when the host does not let this process look.
std::vector<ob::DirEntry> HostList(const std::wstring& host_path) {
    std::vector<ob::DirEntry> out;
    auto open = Host().NtOpenDirectoryObject;
    auto query = HostProc<NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG)>("NtQueryDirectoryObject");
    if (!open || !query) return out;
    UNICODE_STRING us;
    us.Buffer = const_cast<PWSTR>(host_path.c_str());
    us.Length = static_cast<USHORT>(host_path.size() * sizeof(wchar_t));
    us.MaximumLength = static_cast<USHORT>(us.Length + sizeof(wchar_t));
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.ObjectName = &us;
    oa.Attributes = kObjCaseInsensitive;
    HANDLE dir = nullptr;
    if (!NtSuccess(open(&dir, ob::kDirectoryQuery, &oa))) return out;
    std::vector<unsigned char> buf(64 * 1024);
    ULONG context = 0, ret = 0;
    BOOLEAN restart = TRUE;
    for (;;) {
        NTSTATUS s = query(dir, buf.data(), static_cast<ULONG>(buf.size()), FALSE, restart, &context, &ret);
        restart = FALSE;
        if (s == st::NoMoreEntries || (!NtSuccess(s) && s != nts::MoreEntries)) break;
        auto* rows = reinterpret_cast<UNICODE_STRING*>(buf.data());
        bool rows_seen = false;
        for (; rows[0].Buffer; rows += 2, rows_seen = true)
            out.push_back({std::wstring(rows[0].Buffer, rows[0].Length / sizeof(wchar_t)),
                           std::wstring(rows[1].Buffer, rows[1].Length / sizeof(wchar_t))});
        if (s != nts::MoreEntries || !rows_seen) break;
    }
    Host().NtClose(dir);
    return out;
}

bool Contains(const std::vector<ob::DirEntry>& v, const std::wstring& name) {
    for (const auto& e : v)
        if (ob::NameEquals(e.name, name, true)) return true;
    return false;
}

std::vector<ob::DirEntry> ListDirectory(const NodeRef& dir) {
    std::vector<ob::DirEntry> out = g_tree.List(dir);
    const std::wstring path = g_tree.PathOf(dir);
    if (dir == g_tree.Root()) {
        // The host's own top-level directories (\Device, \Driver...) that the layer does not replace.
        for (auto& e : HostList(L"\\"))
            if (!Contains(out, e.name)) out.push_back(std::move(e));
    }
    if (g_host.ready && !path.empty()) {
        for (const auto& e : HostList(g_host.base)) {
            std::wstring gp;
            bool hashed = false;
            if (!ob::DemangleName(g_host.prefix, e.name, &gp, &hashed)) continue;
            if (hashed) {
                AcquireSRWLockShared(&g_sync_lock);
                auto it = g_long_names.find(gp);
                const bool known = it != g_long_names.end();
                if (known) gp = it->second;
                ReleaseSRWLockShared(&g_sync_lock);
                if (!known) continue;
            }
            if (ob::NameEquals(ob::ParentPath(gp), path, true) && !Contains(out, ob::LeafOf(gp)))
                out.push_back({ob::LeafOf(gp), e.type});
        }
    }
    // What this process holds open (the host may not let a sandboxed process look).
    AcquireSRWLockShared(&g_sync_lock);
    for (const auto& kv : g_sync)
        if (kv.second.dir == dir && !Contains(out, kv.second.leaf)) out.push_back({kv.second.leaf, kv.second.type});
    ReleaseSRWLockShared(&g_sync_lock);
    std::sort(out.begin(), out.end(), [](const ob::DirEntry& a, const ob::DirEntry& b) {
        const int c = CompareStringOrdinal(a.name.c_str(), static_cast<int>(a.name.size()), b.name.c_str(),
                                           static_cast<int>(b.name.size()), TRUE);
        return c == CSTR_LESS_THAN;
    });
    return out;
}


// ---- directory objects and symbolic links -------------------------------------------------------------------------------

bool IsKnownDlls(std::wstring_view name) {
    constexpr std::wstring_view k = L"\\KnownDlls";
    if (name.size() < k.size()) return false;
    for (std::size_t i = 0; i < k.size(); ++i)
        if (std::towupper(name[i]) != std::towupper(k[i])) return false;
    return name.size() == k.size() || name[k.size()] == L'\\';
}

// \GLOBAL??: the drive letters of the machine the guest sees (core/vfs) and the devices every Windows has there.
// Rebuilt when the set of drives changes.
SRWLOCK g_drive_lock = SRWLOCK_INIT;
std::wstring g_drive_signature = L"?";

void RefreshDriveLinks() {
#if WIN10XBOX_HAVE_VFS
    std::wstring sig;
    std::vector<std::pair<std::wstring, std::wstring>> links;
    for (const vfs::DriveMapping& d : vfs::Drives()) {
        sig += d.letter;
        links.emplace_back(std::wstring(1, d.letter) + L":", GuestWin32ToDevicePath(std::wstring(1, d.letter) + L":"));
    }
    AcquireSRWLockExclusive(&g_drive_lock);
    if (sig != g_drive_signature) {
        g_drive_signature = sig;
        links.emplace_back(L"PIPE", L"\\Device\\NamedPipe");
        links.emplace_back(L"MAILSLOT", L"\\Device\\Mailslot");
        links.emplace_back(L"UNC", L"\\Device\\Mup");
        links.emplace_back(L"NUL", L"\\Device\\Null");
        links.emplace_back(L"GLOBALROOT", L"");
        links.emplace_back(L"Global", L"\\GLOBAL??");
        g_tree.ReplaceKernelLinks(L"\\GLOBAL??", links, L"layer");
    }
    ReleaseSRWLockExclusive(&g_drive_lock);
#endif
}

struct DirPlan {
    enum class Route { Done, Forward, Anonymous, Found, Leaf } route = Route::Done;
    NTSTATUS status = 0;
    NodeRef node;          // Found: the object; Leaf: the directory it would live in
    std::wstring leaf;
    ParsedOA p;
};

DirPlan PlanDir(const OBJECT_ATTRIBUTES* oa, bool create, bool follow_final) {
    DirPlan d;
    d.p = ParseOA(oa);
    ParsedOA& p = d.p;
    auto done = [&](NTSTATUS s) {
        d.route = DirPlan::Route::Done;
        d.status = s;
        return d;
    };
    if (p.status) return done(p.status);
    if (p.null_oa) {
        if (!create) return done(st::InvalidParameter);
        d.route = DirPlan::Route::Anonymous;
        return d;
    }
    if (p.null_name) {
        if (p.root_handle) return done(nts::NameInvalid);
        if (!create) return done(nts::PathSyntaxBad);
        d.route = DirPlan::Route::Anonymous;
        return d;
    }
    if (p.empty_name && create) {
        d.route = DirPlan::Route::Anonymous;
        return d;
    }
    if (p.root_handle && !p.root_ours) {
        d.route = DirPlan::Route::Forward;
        return d;
    }
    RefreshDriveLinks();
    ob::Resolution r = g_tree.Resolve(p.root_ours ? p.root.node : NodeRef(), p.name, p.ci(), follow_final);
    if (r.status) return done(r.status);
    if (r.foreign) { d.route = DirPlan::Route::Forward; return d; }
    d.node = r.node;
    d.leaf = r.leaf;
    d.route = r.Found() ? DirPlan::Route::Found : DirPlan::Route::Leaf;
    return d;
}

using PFN_OpenOrCreateDir = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);

NTSTATUS NTAPI Xl_NtCreateDirectoryObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    if (!handle || !GuestWritable(handle, sizeof(HANDLE))) return st::AccessViolation;
    DirPlan d = PlanDir(oa, true, true);
    auto fail = [&](NTSTATUS s) { PutHandle(handle, nullptr); return s; };
    auto handle_for = [&](const NodeRef& node, NTSTATUS ok) -> NTSTATUS {
        HANDLE h = NewHandle(node, access, d.p.attributes);
        if (!h) return fail(st::NoMemory);
        PutHandle(handle, h);
        return ok;
    };
    switch (d.route) {
    case DirPlan::Route::Done:
        return fail(d.status);
    case DirPlan::Route::Forward: {
        auto fn = HostProc<PFN_OpenOrCreateDir>("NtCreateDirectoryObject");
        return fn ? fn(handle, access, oa) : fail(nts::NotImplemented);
    }
    case DirPlan::Route::Anonymous: {
        NodeRef n = g_tree.CreateUnnamed(Kind::Directory, L"");
        NTSTATUS s = handle_for(n, st::Success);
        g_tree.Unpin(n);
        return s;
    }
    case DirPlan::Route::Found:
        if (d.node->kind != Kind::Directory) return fail((d.p.attributes & kObjOpenIf) ? st::ObjectTypeMismatch : nts::NameCollision);
        if (!(d.p.attributes & kObjOpenIf)) return fail(nts::NameCollision);
        if (!MapAccess(access, Kind::Directory)) return fail(st::AccessDenied);
        return handle_for(d.node, nts::NameExists);
    case DirPlan::Route::Leaf: {
        NodeRef n = g_tree.CreateChild(d.node, d.leaf, Kind::Directory, L"", (d.p.attributes & kObjPermanent) != 0);
        if (!n) return fail(nts::NameCollision);
        NTSTATUS s = handle_for(n, st::Success);
        g_tree.Unpin(n);
        return s;
    }
    }
    return fail(st::Unsuccessful);
}

NTSTATUS NTAPI Xl_NtOpenDirectoryObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    if (!handle || !GuestWritable(handle, sizeof(HANDLE))) return st::AccessViolation;
    DirPlan d = PlanDir(oa, false, true);
    auto fail = [&](NTSTATUS s) { PutHandle(handle, nullptr); return s; };
    switch (d.route) {
    case DirPlan::Route::Done:
        return fail(d.status);
    case DirPlan::Route::Forward: {
        // No KnownDlls: Wine's loader then searches the file system through the VFS (E3).
        if (!d.p.root_handle && IsKnownDlls(d.p.name)) return fail(st::ObjectNameNotFound);
        return Host().NtOpenDirectoryObject(handle, access, oa);
    }
    case DirPlan::Route::Found: {
        if (d.node->kind != Kind::Directory) return fail(st::ObjectTypeMismatch);
        if (!MapAccess(access, Kind::Directory)) return fail(st::AccessDenied);
        HANDLE h = NewHandle(d.node, access, d.p.attributes);
        if (!h) return fail(st::NoMemory);
        PutHandle(handle, h);
        return st::Success;
    }
    case DirPlan::Route::Leaf:
        return fail(st::ObjectNameNotFound);
    default:
        return fail(st::Unsuccessful);
    }
}

NTSTATUS NTAPI Xl_NtCreateSymbolicLinkObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PUNICODE_STRING target) {
    if (!handle || !GuestWritable(handle, sizeof(HANDLE))) return st::AccessViolation;
    auto fail = [&](NTSTATUS s) { PutHandle(handle, nullptr); return s; };
    if (!target || !GuestReadable(target, sizeof(UNICODE_STRING))) return fail(st::AccessViolation);
    const UNICODE_STRING t = *target;
    if (!t.MaximumLength) return fail(st::InvalidParameter);
    if (!t.Buffer) return fail(st::AccessViolation);
    if (t.Length && (reinterpret_cast<std::uintptr_t>(t.Buffer) & 1)) return fail(nts::DatatypeMisalignment);
    if (t.Length && !GuestReadable(t.Buffer, t.Length)) return fail(st::AccessViolation);
    if (!oa) return fail(st::AccessViolation);
    const std::wstring tgt(t.Buffer, t.Length / sizeof(wchar_t));
    DirPlan d = PlanDir(oa, true, false);
    auto handle_for = [&](const NodeRef& node, NTSTATUS ok) -> NTSTATUS {
        HANDLE h = NewHandle(node, access, d.p.attributes);
        if (!h) return fail(st::NoMemory);
        PutHandle(handle, h);
        return ok;
    };
    switch (d.route) {
    case DirPlan::Route::Done:
        return fail(d.status);
    case DirPlan::Route::Forward: {
        using Fn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);
        auto fn = HostProc<Fn>("NtCreateSymbolicLinkObject");
        return fn ? fn(handle, access, oa, target) : fail(nts::NotImplemented);
    }
    case DirPlan::Route::Anonymous: {
        NodeRef n = g_tree.CreateUnnamed(Kind::SymbolicLink, tgt);
        NTSTATUS s = handle_for(n, st::Success);
        g_tree.Unpin(n);
        return s;
    }
    case DirPlan::Route::Found:
        if (d.node->kind != Kind::SymbolicLink) return fail(st::ObjectTypeMismatch);
        if (!(d.p.attributes & kObjOpenIf)) return fail(nts::NameCollision);
        if (!MapAccess(access, Kind::SymbolicLink)) return fail(st::AccessDenied);
        return handle_for(d.node, st::Success);   // Windows 10 answers an OBJ_OPENIF create of a link with success
    case DirPlan::Route::Leaf: {
        NodeRef n = g_tree.CreateChild(d.node, d.leaf, Kind::SymbolicLink, tgt, (d.p.attributes & kObjPermanent) != 0);
        if (!n) return fail(nts::NameCollision);
        NTSTATUS s = handle_for(n, st::Success);
        g_tree.Unpin(n);
        return s;
    }
    }
    return fail(st::Unsuccessful);
}

NTSTATUS NTAPI Xl_NtOpenSymbolicLinkObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa) {
    if (!handle || !GuestWritable(handle, sizeof(HANDLE))) return st::AccessViolation;
    DirPlan d = PlanDir(oa, false, false);
    auto fail = [&](NTSTATUS s) { PutHandle(handle, nullptr); return s; };
    switch (d.route) {
    case DirPlan::Route::Done:
        return fail(d.status);
    case DirPlan::Route::Forward:
        if (!d.p.root_handle && IsKnownDlls(d.p.name)) return fail(st::ObjectNameNotFound);
        return Host().NtOpenSymbolicLinkObject(handle, access, oa);
    case DirPlan::Route::Found: {
        if (d.node->kind != Kind::SymbolicLink) return fail(st::ObjectTypeMismatch);
        if (!MapAccess(access, Kind::SymbolicLink)) return fail(st::AccessDenied);
        HANDLE h = NewHandle(d.node, access, d.p.attributes);
        if (!h) return fail(st::NoMemory);
        PutHandle(handle, h);
        return st::Success;
    }
    case DirPlan::Route::Leaf:
        return fail(st::ObjectNameNotFound);
    default:
        return fail(st::Unsuccessful);
    }
}

NTSTATUS NTAPI Xl_NtQuerySymbolicLinkObject(HANDLE h, PUNICODE_STRING out, PULONG ret_len) {
    HandleRec rec;
    ObjectKind k;
    if (!OurHandle(h, &rec, &k)) {
        using Fn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);
        auto fn = HostProc<Fn>("NtQuerySymbolicLinkObject");
        return fn ? fn(h, out, ret_len) : nts::NotImplemented;
    }
    if (k != ObjectKind::SymbolicLink) return st::ObjectTypeMismatch;
    if (!(rec.access & ob::kSymlinkQuery)) return st::AccessDenied;
    if (!out || !GuestWritable(out, sizeof(UNICODE_STRING))) return st::AccessViolation;
    if (ret_len && !GuestWritable(ret_len, sizeof(ULONG))) return st::AccessViolation;
    UNICODE_STRING header;
    if (NTSTATUS ls = probe::Load<UNICODE_STRING>(out, &header)) return ls;
    const std::wstring target = g_tree.Target(rec.node);
    const ULONG need = static_cast<ULONG>(target.size() * sizeof(wchar_t) + sizeof(wchar_t));
    PutUlong(ret_len, need);
    if (header.MaximumLength < need) return st::BufferTooSmall;
    if (NTSTATUS cs = probe::CopyToUser(header.Buffer, target.c_str(), need)) return cs;
    header.Length = static_cast<USHORT>(need - sizeof(wchar_t));
    return probe::Store<UNICODE_STRING>(out, header);
}

NTSTATUS NTAPI Xl_NtQueryDirectoryObject(HANDLE h, PVOID buffer, ULONG size, BOOLEAN single, BOOLEAN restart, PULONG context,
                                         PULONG ret_len) {
    HandleRec rec;
    ObjectKind k;
    if (!OurHandle(h, &rec, &k)) {
        using Fn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
        auto fn = HostProc<Fn>("NtQueryDirectoryObject");
        return fn ? fn(h, buffer, size, single, restart, context, ret_len) : nts::NotImplemented;
    }
    if (k != ObjectKind::Directory) return st::ObjectTypeMismatch;
    if (!(rec.access & ob::kDirectoryQuery)) return st::AccessDenied;
    if (!context || !GuestWritable(context, sizeof(ULONG))) return st::AccessViolation;
    if (size && !GuestWritable(buffer, size)) return st::AccessViolation;
    if (ret_len && !GuestWritable(ret_len, sizeof(ULONG))) return st::AccessViolation;
    ULONG index = 0;
    if (!restart)
        if (NTSTATUS ls = probe::Load<ULONG>(context, &index)) return ls;
    if (g_tree.PathOf(rec.node) == L"\\GLOBAL??" || g_tree.PathOf(rec.node) == L"\\??") RefreshDriveLinks();
    std::vector<ob::DirEntry> entries = ListDirectory(rec.node);
    // Built in a private buffer (pointers as they will be in the caller's), then copied out.
    std::vector<char> tmp(size ? size : 1);
    ULONG written = 0, ret = 0;
    const NTSTATUS s = ob::PackDirectoryEntries(entries, tmp.data(), size, single != 0, restart != 0, &index, &ret, buffer, &written);
    if (written)
        if (NTSTATUS cs = probe::CopyToUser(buffer, tmp.data(), written)) return cs;
    PutUlong(ret_len, ret);
    if (s == st::Success || s == nts::MoreEntries) PutUlong(context, index);
    return s;
}

// ---- NtQueryObject ---------------------------------------------------------------------------------------------------------

// Writes `bytes` (built with pointers relative to offset 0 of the block) into the caller's buffer, fixing the one
// UNICODE_STRING pointer at the start: `text_offset` is where the characters are.
NTSTATUS WriteNameInfo(const std::wstring& name, PVOID buf, ULONG len, PULONG ret) {
    const ULONG n = static_cast<ULONG>(name.size() * sizeof(wchar_t));
    const ULONG need = name.empty() ? sizeof(UNICODE_STRING) : static_cast<ULONG>(sizeof(UNICODE_STRING)) + n + sizeof(wchar_t);
    if (ret && !GuestWritable(ret, sizeof(ULONG))) return st::AccessViolation;
    PutUlong(ret, need);
    if (len < need) return st::InfoLengthMismatch;
    std::vector<std::uint8_t> out(need, 0);
    auto* us = reinterpret_cast<UNICODE_STRING*>(out.data());
    if (!name.empty()) {
        std::memcpy(out.data() + sizeof(UNICODE_STRING), name.c_str(), n + sizeof(wchar_t));
        us->Buffer = reinterpret_cast<wchar_t*>(static_cast<std::uint8_t*>(buf) + sizeof(UNICODE_STRING));
        us->Length = static_cast<USHORT>(n);
        us->MaximumLength = static_cast<USHORT>(n + sizeof(wchar_t));
    }
    return probe::CopyToUser(buf, out.data(), need);
}

// OBJECT_TYPE_INFORMATION of the host's "Directory" / "SymbolicLink" type (real mappings, indices, masks), counts ours.
constexpr ULONG kTypeInfoSize = 0x68;
struct TypeRow {
    std::uint8_t bytes[kTypeInfoSize]{};
    bool valid = false;
};

TypeRow HostTypeRow(const wchar_t* name) {
    TypeRow row;
    auto query = HostProc<NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG)>("NtQueryObject");
    if (query) {
        std::vector<std::uint8_t> buf(128 * 1024);
        ULONG ret = 0;
        if (NtSuccess(query(nullptr, kObjectTypesInformation, buf.data(), static_cast<ULONG>(buf.size()), &ret))) {
            const ULONG count = *reinterpret_cast<ULONG*>(buf.data());
            std::uint8_t* p = buf.data() + sizeof(ULONG_PTR);
            for (ULONG i = 0; i < count && p + kTypeInfoSize <= buf.data() + buf.size(); ++i) {
                auto* us = reinterpret_cast<UNICODE_STRING*>(p);
                const std::uint8_t* text = reinterpret_cast<const std::uint8_t*>(us->Buffer);
                if (std::wstring_view(us->Buffer, us->Length / sizeof(wchar_t)) == name) {
                    std::memcpy(row.bytes, p, kTypeInfoSize);
                    row.valid = true;
                    break;
                }
                p = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(text) + us->MaximumLength + sizeof(ULONG_PTR) - 1) &
                                                    ~(sizeof(ULONG_PTR) - 1));
            }
        }
    }
    if (!row.valid) {   // the access masks Windows defines for the two types
        const bool dir = std::wstring_view(name) == L"Directory";
        auto* ulongs = reinterpret_cast<ULONG*>(row.bytes);
        ulongs[0x44 / 4] = dir ? 0x20003 : 0x20001;    // GenericRead
        ulongs[0x48 / 4] = dir ? 0x2000C : 0x20000;    // GenericWrite
        ulongs[0x4C / 4] = dir ? 0x20003 : 0x20001;    // GenericExecute
        ulongs[0x50 / 4] = dir ? 0xF000F : 0xF0001;    // GenericAll
        ulongs[0x54 / 4] = dir ? 0x1F000F : 0x1F0001;  // ValidAccessMask
        row.valid = true;
    }
    return row;
}

// What NtQueryObject needs to know about an object the host cannot describe (it is emulated here).
struct Described {
    std::wstring type, name;
    ACCESS_MASK access = 0;
    ULONG attributes = 0, handles = 1;
    bool permanent = false;
    int count_kind = -1;   // 0 directory, 1 symbolic link: type counts are ours
};

NTSTATUS QueryDescribed(const Described& d, ULONG cls, PVOID buf, ULONG len, PULONG ret) {
    auto set_ret = [&](ULONG v) -> bool {
        if (!ret) return true;
        if (!GuestWritable(ret, sizeof(ULONG))) return false;
        PutUlong(ret, v);
        return true;
    };
    switch (cls) {
    case kObjectBasicInformation: {
        constexpr ULONG kSize = 0x38;
        if (!set_ret(kSize)) return st::AccessViolation;
        if (len < kSize) return st::InfoLengthMismatch;
        std::uint8_t block[kSize] = {};
        auto* u = reinterpret_cast<ULONG*>(block);
        u[0] = (d.attributes & kObjInherit) | (d.permanent ? kObjPermanent : 0);
        u[1] = d.access;
        u[2] = d.handles;
        u[3] = d.handles + 1;
        u[9] = d.name.empty() ? 0 : static_cast<ULONG>(sizeof(UNICODE_STRING) + d.name.size() * sizeof(wchar_t) + sizeof(wchar_t));
        u[10] = kTypeInfoSize + static_cast<ULONG>(d.type.size() * sizeof(wchar_t) + sizeof(wchar_t));
        return probe::CopyToUser(buf, block, kSize);
    }
    case kObjectNameInformation:
        return WriteNameInfo(d.name, buf, len, ret);
    case kObjectTypeInformation: {
        const ULONG need = kTypeInfoSize + static_cast<ULONG>(d.type.size() * sizeof(wchar_t) + sizeof(wchar_t));
        if (!set_ret(need)) return st::AccessViolation;
        if (len < need) return st::InfoLengthMismatch;
        TypeRow row = HostTypeRow(d.type.c_str());
        std::vector<std::uint8_t> block(need, 0);
        std::memcpy(block.data(), row.bytes, kTypeInfoSize);
        auto* us = reinterpret_cast<UNICODE_STRING*>(block.data());
        std::memcpy(block.data() + kTypeInfoSize, d.type.c_str(), d.type.size() * sizeof(wchar_t) + sizeof(wchar_t));
        us->Buffer = reinterpret_cast<wchar_t*>(static_cast<std::uint8_t*>(buf) + kTypeInfoSize);
        us->Length = static_cast<USHORT>(d.type.size() * sizeof(wchar_t));
        us->MaximumLength = static_cast<USHORT>(us->Length + sizeof(wchar_t));
        if (d.count_kind >= 0) {
            auto* u = reinterpret_cast<ULONG*>(block.data());
            const ULONG objects = static_cast<ULONG>(g_tree.Count(d.count_kind == 0 ? Kind::Directory : Kind::SymbolicLink));
            const ULONG handles = static_cast<ULONG>(g_handle_count[d.count_kind]);
            u[0x10 / 4] = objects;
            u[0x14 / 4] = handles;
            u[0x28 / 4] = std::max<ULONG>(u[0x28 / 4], objects);
            u[0x2C / 4] = std::max<ULONG>(u[0x2C / 4], handles);
        }
        return probe::CopyToUser(buf, block.data(), need);
    }
    default:
        return nts::NotImplemented;
    }
}

NTSTATUS QueryOurs(const HandleRec& rec, ObjectKind kind, ULONG cls, PVOID buf, ULONG len, PULONG ret) {
    const bool dir = kind == ObjectKind::Directory;
    Described d;
    d.type = dir ? L"Directory" : L"SymbolicLink";
    d.name = g_tree.PathOf(rec.node);
    d.access = rec.access;
    d.attributes = rec.attributes;
    d.handles = rec.node->pins;
    d.permanent = rec.node->permanent;
    d.count_kind = dir ? 0 : 1;
    return QueryDescribed(d, cls, buf, len, ret);
}

#if WIN10XBOX_HAVE_REGISTRY
// A registry key handle is emulated by core/registry: the host has never heard of it.
NTSTATUS QueryKeyHandle(HANDLE h, ULONG cls, PVOID buf, ULONG len, PULONG ret) {
    Described d;
    d.type = L"Key";
    d.access = 0xF003F;
    if (cls == kObjectNameInformation || cls == kObjectBasicInformation) {
        alignas(8) std::uint8_t kn[8192];
        ULONG kret = 0;
        if (NtSuccess(registry::Reg_NtQueryKey(h, 3 /*KeyNameInformation*/, kn, sizeof kn, &kret))) {
            const ULONG bytes = *reinterpret_cast<ULONG*>(kn);
            d.name.assign(reinterpret_cast<wchar_t*>(kn + sizeof(ULONG)), bytes / sizeof(wchar_t));
        }
    }
    return QueryDescribed(d, cls, buf, len, ret);
}
#endif

#if WIN10XBOX_HAVE_TOKEN
NTSTATUS QueryTokenHandle(ULONG cls, PVOID buf, ULONG len, PULONG ret) {
    Described d;
    d.type = L"Token";
    d.access = 0xF01FF;
    return QueryDescribed(d, cls, buf, len, ret);
}
#endif

// The guest path of a host object this layer named, when the handle's name says so.
bool NamedHandleGuestPath(HANDLE h, std::wstring* out) {
    {
        AcquireSRWLockShared(&g_sync_lock);
        auto it = g_sync.find(reinterpret_cast<std::uintptr_t>(h));
        bool found = it != g_sync.end();
        NodeRef dir;
        std::wstring leaf;
        if (found) {
            dir = it->second.dir;
            leaf = it->second.leaf;
        }
        ReleaseSRWLockShared(&g_sync_lock);
        if (found) {
            const std::wstring dp = g_tree.PathOf(dir);
            *out = dp.empty() ? std::wstring() : (dp == L"\\" ? dp + leaf : dp + L"\\" + leaf);
            return true;
        }
    }
    if (!g_host.ready) return false;
    auto query = HostProc<NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG)>("NtQueryObject");
    if (!query) return false;
    alignas(8) std::uint8_t buf[2048];
    ULONG ret = 0;
    if (!NtSuccess(query(h, kObjectNameInformation, buf, sizeof buf, &ret))) return false;
    auto* us = reinterpret_cast<UNICODE_STRING*>(buf);
    if (!us->Buffer || !us->Length) return false;
    std::wstring_view full(us->Buffer, us->Length / sizeof(wchar_t));
    const std::size_t slash = full.rfind(L'\\');
    std::wstring_view leaf = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
    bool hashed = false;
    std::wstring gp;
    if (!ob::DemangleName(g_host.prefix, leaf, &gp, &hashed)) return false;
    if (hashed) {
        AcquireSRWLockShared(&g_sync_lock);
        auto it = g_long_names.find(gp);
        const bool known = it != g_long_names.end();
        if (known) gp = it->second;
        ReleaseSRWLockShared(&g_sync_lock);
        if (!known) return false;
    }
    *out = std::move(gp);
    return true;
}

NTSTATUS NTAPI Xl_NtQueryObject(HANDLE h, ULONG cls, PVOID buf, ULONG len, PULONG ret) {
    HandleRec rec;
    ObjectKind k;
    if (OurHandle(h, &rec, &k)) {
        NTSTATUS s = QueryOurs(rec, k, cls, buf, len, ret);
        if (s != nts::NotImplemented) return s;
    }
#if WIN10XBOX_HAVE_REGISTRY
    else if (registry::IsKeyHandle(h)) {
        NTSTATUS s = QueryKeyHandle(h, cls, buf, len, ret);
        if (s != nts::NotImplemented) return s;
    }
#endif
#if WIN10XBOX_HAVE_TOKEN
    else if (token::IsTokenHandle(h)) {
        NTSTATUS s = QueryTokenHandle(cls, buf, len, ret);
        if (s != nts::NotImplemented) return s;
    }
#endif
    else if (cls == kObjectNameInformation && h) {
        std::wstring name;
        if (NamedHandleGuestPath(h, &name)) return WriteNameInfo(name, buf, len, ret);
    }
    return QueryObjectHostFile(h, cls, buf, len, ret);
}

NTSTATUS NTAPI Xl_NtMakeTemporaryObject(HANDLE h) {
    HandleRec rec;
    if (OurHandle(h, &rec)) {
        if (!(rec.access & ob::kDelete)) return st::AccessDenied;
        g_tree.MakePermanent(rec.node, false);
        return st::Success;
    }
    auto fn = HostProc<NTSTATUS(NTAPI*)(HANDLE)>("NtMakeTemporaryObject");
    return fn ? fn(h) : nts::NotImplemented;
}

}  // namespace

// ---- hooks used by NtClose / NtDuplicateObject / sections ---------------------------------------------------------------

void CloseEmulatedDirectory(const EmulatedObject& o) {
    auto* rec = static_cast<HandleRec*>(o.extra);
    if (!rec) return;
    AcquireSRWLockExclusive(&g_rec_lock);
    NodeRef node = rec->node;
    delete rec;
    ReleaseSRWLockExclusive(&g_rec_lock);
    InterlockedDecrement(&g_handle_count[node->kind == Kind::Directory ? 0 : 1]);
    g_tree.Unpin(node);
}

HANDLE DuplicateEmulatedDirectory(const EmulatedObject& o, ACCESS_MASK access, ULONG options) {
    auto* rec = static_cast<HandleRec*>(o.extra);
    if (!rec) return nullptr;
    HandleRec copy;
    AcquireSRWLockShared(&g_rec_lock);
    copy = *rec;
    ReleaseSRWLockShared(&g_rec_lock);
    return NewHandle(copy.node, (options & DUPLICATE_SAME_ACCESS) ? copy.access : access, copy.attributes);
}

void ForgetNamedHandle(HANDLE h) {
    AcquireSRWLockExclusive(&g_sync_lock);
    auto it = g_sync.find(reinterpret_cast<std::uintptr_t>(h));
    NodeRef dir;
    if (it != g_sync.end()) {
        dir = std::move(it->second.dir);
        g_sync.erase(it);
    }
    ReleaseSRWLockExclusive(&g_sync_lock);
    if (dir) g_tree.Unpin(dir);
}

void NoteDuplicatedNamedHandle(HANDLE src, HANDLE dup) {
    NodeRef dir;
    SyncRec copy;
    AcquireSRWLockShared(&g_sync_lock);
    auto it = g_sync.find(reinterpret_cast<std::uintptr_t>(src));
    bool found = it != g_sync.end();
    if (found) copy = it->second;
    ReleaseSRWLockShared(&g_sync_lock);
    if (!found || !dup) return;
    g_tree.Pin(copy.dir);
    AcquireSRWLockExclusive(&g_sync_lock);
    g_sync[reinterpret_cast<std::uintptr_t>(dup)] = copy;
    ReleaseSRWLockExclusive(&g_sync_lock);
}

NTSTATUS CreateNamedHostObject(const wchar_t* type, PHANDLE handle, POBJECT_ATTRIBUTES oa,
                               const std::function<NTSTATUS(POBJECT_ATTRIBUTES, PHANDLE)>& call) {
    return SyncCall(type, true, handle, oa, call);
}

int ResolveFileNameThroughNamespace(const OBJECT_ATTRIBUTES* oa, NTSTATUS* status, std::wstring* absolute) {
    // Every file open comes through here: look at the cheap things first. (The callers already read the name.)
    if (!oa || !oa->ObjectName) return 0;
    if (oa->RootDirectory) {
        if (!OurHandle(oa->RootDirectory, nullptr)) return 0;
    } else {
        const UNICODE_STRING* n = oa->ObjectName;
        if (n->Length < 3 * sizeof(wchar_t) || !n->Buffer || n->Buffer[0] != L'\\') return 0;
        const wchar_t c = static_cast<wchar_t>(std::towupper(n->Buffer[1]));
        if (c != L'B' && c != L'S' && c != L'K' && c != L'O' && c != L'?' && c != L'D' && c != L'G') return 0;
    }
    if (oa->Length != sizeof(OBJECT_ATTRIBUTES)) return 0;   // `oa` is the caller's capture (TranslateOA)
    ParsedOA p = ParseOA(oa);
    if (p.status || p.null_oa || p.null_name || p.empty_name) return 0;
    bool ours = p.root_ours;
    if (!ours) {
        if (p.root_handle || p.name[0] != L'\\') return 0;
        std::wstring_view full(p.name);
        std::wstring_view rest = full.substr(1);
        const std::size_t e = rest.find(L'\\');
        std::wstring_view first = rest.substr(0, e);
        auto is = [&](std::wstring_view n) { return ob::NameEquals(first, n, true); };
        if (is(L"BaseNamedObjects") || is(L"Sessions") || is(L"KernelObjects") || is(L"ObjectTypes")) {
            ours = true;
        } else if ((is(L"??") || is(L"DosDevices") || is(L"GLOBAL??")) && e != std::wstring_view::npos) {
            // An ordinary \??\C:\x is the file layer's. Only a link the guest made leads into the tree.
            std::wstring_view after = rest.substr(e + 1);
            std::wstring_view second = after.substr(0, after.find(L'\\'));
            if (!second.empty()) {
                ob::Resolution r = g_tree.Resolve(NodeRef(), std::wstring(L"\\??\\").append(second), true, false);
                ours = r.Found() && !r.node->kernel;
            }
        }
        if (!ours) return 0;
    }
    ob::Resolution r = g_tree.Resolve(p.root_ours ? p.root.node : NodeRef(), p.name, p.ci(), true);
    if (r.status) { *status = r.status; return 1; }
    if (r.foreign) { *absolute = r.rest; return 2; }
    *status = r.Found() ? st::ObjectTypeMismatch : st::ObjectNameNotFound;
    return 1;
}

// ---- links provided by other modules (the driver host) -----------------------------------------------------------------------

static std::wstring KernelLinkPath(const std::wstring& path) {
    constexpr std::wstring_view dos = L"\\DosDevices\\";
    if (path.size() > dos.size() && ob::NameEquals(std::wstring_view(path).substr(0, dos.size()), dos, true))
        return L"\\??\\" + path.substr(dos.size());
    return path;
}

}  // namespace win10xbox::dispatch::detail

namespace win10xbox::dispatch {

void RegisterNamespaceLink(const std::wstring& path, const std::wstring& target) {
    detail::g_tree.AddKernelNode(detail::KernelLinkPath(path), detail::Kind::SymbolicLink, target);
}

void UnregisterNamespaceLink(const std::wstring& path) { detail::g_tree.RemoveKernelNode(detail::KernelLinkPath(path)); }

}  // namespace win10xbox::dispatch

namespace win10xbox::dispatch::detail {

// ---- start-up ---------------------------------------------------------------------------------------------------------------

void InitializeObjectNamespace() {
    const unsigned session = PebField<ULONG>(0x2C0);
    // An AppContainer host (the console): its named objects live in \Sessions\<n>\AppContainerNamedObjects\<sid>.
    std::wstring ac_sid;
    {
        using OpenTok = NTSTATUS(NTAPI*)(HANDLE, ACCESS_MASK, PHANDLE);
        using QueryTok = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        auto open = HostProc<OpenTok>("NtOpenProcessToken");
        auto query = HostProc<QueryTok>("NtQueryInformationToken");
        auto convert = HostProc<NTSTATUS(NTAPI*)(PUNICODE_STRING, PVOID, BOOLEAN)>("RtlConvertSidToUnicodeString");
        auto free_us = HostProc<void(NTAPI*)(PUNICODE_STRING)>("RtlFreeUnicodeString");
        HANDLE tok = nullptr;
        if (open && query && convert && free_us && NtSuccess(open(GetCurrentProcess(), TOKEN_QUERY, &tok))) {
            ULONG is_ac = 0, ret = 0;
            if (NtSuccess(query(tok, 29 /*TokenIsAppContainer*/, &is_ac, sizeof is_ac, &ret)) && is_ac) {
                alignas(8) std::uint8_t buf[256];
                if (NtSuccess(query(tok, 31 /*TokenAppContainerSid*/, buf, sizeof buf, &ret))) {
                    UNICODE_STRING us{};
                    if (NtSuccess(convert(&us, *reinterpret_cast<PVOID*>(buf), TRUE))) {
                        ac_sid.assign(us.Buffer, us.Length / sizeof(wchar_t));
                        free_us(&us);
                    }
                }
            }
            Host().NtClose(tok);
        }
    }
    g_host.ac = !ac_sid.empty();
    g_host.ac_sid = ac_sid;
    g_tree.Populate(session, ac_sid);
    RefreshDriveLinks();

    // The per-layer prefix: a program tree (a guest and the children it starts) shares one.
    wchar_t env[64];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_OBJNS", env, 64);
    std::wstring layer;
    if (n == 16) {
        layer.assign(env, n);
    } else {
        FILETIME created, exited, kernel, user;
        std::uint64_t seed = GetCurrentProcessId();
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            seed ^= (static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime) * 0x9E3779B97F4A7C15ull;
        seed ^= seed >> 29;
        seed *= 0xBF58476D1CE4E5B9ull;
        seed ^= seed >> 32;
        const wchar_t* hex = L"0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) layer += hex[(seed >> shift) & 0xF];
        SetEnvironmentVariableW(L"WIN10XBOX_OBJNS", layer.c_str());
    }
    g_host.layer = layer;
    g_host.prefix = L"W10X" + layer;

    // Where the host puts a named object of this process: ask it, by creating one the way kernelbase would.
    HANDLE probe = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\W10XPROBE" + layer).c_str());
    if (!probe) {
        WARN(dispatch, "object namespace: the host refused a named probe event (error %lu); named objects keep their host names",
             GetLastError());
        return;
    }
    auto query = HostProc<NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG)>("NtQueryObject");
    alignas(8) std::uint8_t buf[2048];
    ULONG ret = 0;
    if (query && NtSuccess(query(probe, kObjectNameInformation, buf, sizeof buf, &ret))) {
        auto* us = reinterpret_cast<UNICODE_STRING*>(buf);
        std::wstring full(us->Buffer, us->Length / sizeof(wchar_t));
        g_host.base = ob::ParentPath(full);
        g_host.ready = !g_host.base.empty() && g_host.base != L"\\";
    }
    CloseHandle(probe);
    TRACE(dispatch, "object namespace: session %u, host directory %ls, layer %ls%s", session, g_host.base.c_str(),
          layer.c_str(), g_host.ac ? " (AppContainer)" : "");
    if (!g_host.ready) WARN(dispatch, "object namespace: no host directory for named objects, names are not translated");
}

const ImplDef* ObjectImpls() {
    static const ImplDef k[] = {
        {"NtCreateDirectoryObject", reinterpret_cast<void*>(&Xl_NtCreateDirectoryObject)},
        {"NtOpenDirectoryObject", reinterpret_cast<void*>(&Xl_NtOpenDirectoryObject)},
        {"NtQueryDirectoryObject", reinterpret_cast<void*>(&Xl_NtQueryDirectoryObject)},
        {"NtCreateSymbolicLinkObject", reinterpret_cast<void*>(&Xl_NtCreateSymbolicLinkObject)},
        {"NtOpenSymbolicLinkObject", reinterpret_cast<void*>(&Xl_NtOpenSymbolicLinkObject)},
        {"NtQuerySymbolicLinkObject", reinterpret_cast<void*>(&Xl_NtQuerySymbolicLinkObject)},
        {"NtQueryObject", reinterpret_cast<void*>(&Xl_NtQueryObject)},
        {"NtMakeTemporaryObject", reinterpret_cast<void*>(&Xl_NtMakeTemporaryObject)},
        {"NtCreateEvent", reinterpret_cast<void*>(&Xl_NtCreateEvent)},
        {"NtOpenEvent", reinterpret_cast<void*>(&Xl_NtOpenEvent)},
        {"NtCreateMutant", reinterpret_cast<void*>(&Xl_NtCreateMutant)},
        {"NtOpenMutant", reinterpret_cast<void*>(&Xl_NtOpenMutant)},
        {"NtCreateSemaphore", reinterpret_cast<void*>(&Xl_NtCreateSemaphore)},
        {"NtOpenSemaphore", reinterpret_cast<void*>(&Xl_NtOpenSemaphore)},
        {"NtCreateTimer", reinterpret_cast<void*>(&Xl_NtCreateTimer)},
        {"NtOpenTimer", reinterpret_cast<void*>(&Xl_NtOpenTimer)},
        {"NtOpenSection", reinterpret_cast<void*>(&Xl_NtOpenSection)},
        {"NtCreateKeyedEvent", reinterpret_cast<void*>(&Xl_NtCreateKeyedEvent)},
        {"NtOpenKeyedEvent", reinterpret_cast<void*>(&Xl_NtOpenKeyedEvent)},
        {"NtCreateIoCompletion", reinterpret_cast<void*>(&Xl_NtCreateIoCompletion)},
        {"NtOpenIoCompletion", reinterpret_cast<void*>(&Xl_NtOpenIoCompletion)},
        {"NtCreateJobObject", reinterpret_cast<void*>(&Xl_NtCreateJobObject)},
        {"NtOpenJobObject", reinterpret_cast<void*>(&Xl_NtOpenJobObject)},
        {"NtAllocateReserveObject", reinterpret_cast<void*>(&Xl_NtAllocateReserveObject)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
