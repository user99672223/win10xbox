// win10xbox registry: virtual \Registry tree built from real REGF hives plus
// an in-memory write overlay. See include/win10xbox/registry.h.
//
// Model
//   * A Node is one registry key as the guest sees it. Nodes are created
//     lazily and are never freed until Shutdown(), so a handle can never
//     dangle (deleted keys stay as tombstones and report STATUS_KEY_DELETED).
//   * A Node may be backed by a key in a read-only regf::Hive (hive + nk).
//     Everything the guest changes is recorded on the Node (values set /
//     deleted, subkeys created / deleted, class, time); the hive file is
//     never opened for write.
//   * Reads merge: hive values/subkeys minus tombstones, plus overlay-only
//     entries. A key deleted and re-created starts empty (hive detached).
//   * \Registry, \Registry\Machine and \Registry\User are synthetic
//     "container" nodes; hives are mounted below them.
//
// One global mutex protects everything (no thread_local anywhere). Pointers
// the guest hands to the Reg_Nt* functions are dereferenced directly; the
// dispatcher is responsible for probing them.
#include "win10xbox/registry.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "json_mini.h"
#include "regf.h"
#include "win10xbox/config.h"
#include "win10xbox/probe.h"
#include "win10xbox/trace.h"

namespace win10xbox::registry {

namespace {

using regf::Upcase;

// ---- access rights / options (spelled out; headers disagree on availability)
constexpr ACCESS_MASK kKeyQueryValue = 0x0001, kKeySetValue = 0x0002,
                      kKeyEnumerateSubKeys = 0x0008, kKeyNotify = 0x0010,
                      kKeyRead = 0x20019, kKeyWrite = 0x20006, kKeyAll = 0xF003F,
                      kDelete = 0x00010000, kMaximumAllowed = 0x02000000,
                      kGenericRead = 0x80000000u, kGenericWrite = 0x40000000, kGenericExecute = 0x20000000,
                      kGenericAll = 0x10000000;
constexpr ULONG kRegOptionVolatile = 1, kRegOptionCreateLink = 2;
constexpr ULONG kObjOpenLink = 0x100;
constexpr ULONG kRegCreatedNewKey = 1, kRegOpenedExistingKey = 2;
constexpr ULONG kRegLink = 6;
constexpr ULONG kNotifyName = 1, kNotifyLastSet = 4;
constexpr NTSTATUS kStatusNotifyCleanup = 0x0000010B;
constexpr NTSTATUS kStatusRegistryCorrupt = static_cast<NTSTATUS>(0xC000014C);
constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003A);
constexpr NTSTATUS kStatusChildMustBeVolatile = static_cast<NTSTATUS>(0xC0000181);
constexpr std::size_t kMaxKeyNameChars = 255, kMaxValueNameChars = 16383;
constexpr int kMaxLinkDepth = 16;
constexpr std::uint64_t kHandleTag = 0x4B45590000000000ull;   // 'K','E','Y' in the top 24 bits
constexpr std::uint64_t kHandleIdMask = 0x000000FFFFFFFFFFull;

// ---- model ------------------------------------------------------------------

struct OvValue {
    std::wstring name;
    ULONG type = 0;
    std::vector<std::uint8_t> data;
    bool deleted = false;
    std::uint64_t seq = 0;
};

struct Val {
    std::wstring name;
    ULONG type = 0;
    std::vector<std::uint8_t> data;
};

struct Node;
struct ChildEnt {
    std::wstring name;
    std::wstring up;
    std::uint32_t nk = 0;
    Node* node = nullptr;
};

struct Node {
    std::wstring name;
    Node* parent = nullptr;
    std::shared_ptr<regf::Hive> hive;
    std::uint32_t nk = 0;
    bool container = false;   // synthetic: no values, no key creation
    bool is_mount = false;    // root of a hive (real or synthetic)
    bool loaded = false;      // mounted by NtLoadKey (may be unloaded)
    bool shadow = false;      // materialised from the parent's hive subkey
    bool deleted = false;
    bool link = false;        // created with REG_OPTION_CREATE_LINK
    bool is_volatile = false;
    bool has_class = false;
    std::wstring cls;
    std::uint64_t mtime = 0;  // 0: use the hive timestamp
    std::map<std::wstring, Node*> kids;        // upper-cased name
    std::map<std::wstring, OvValue> ov;        // upper-cased name
    std::uint64_t ov_seq = 0;
    bool kids_valid = false;
    std::vector<ChildEnt> kid_cache;
    bool vals_valid = false;
    std::vector<Val> val_cache;
};

struct HandleEnt {
    Node* node = nullptr;
    ACCESS_MASK access = 0;
};

// One request of NtNotifyChangeKey: the event to set (and the status block to fill) when the key changes.
struct Pending {
    HANDLE event = nullptr;
    PIO_STATUS_BLOCK iosb = nullptr;
    bool done = false;
};

// The notification of one key handle. Its filter and subtree flag are those of the handle's first request; later
// requests on the same handle only add their event (Wine's server does the same, and ntdll's reg test pins it).
struct Watch {
    Node* node = nullptr;
    bool tree = false;
    ULONG filter = 0;
    KeyId handle = 0;
    std::vector<std::shared_ptr<Pending>> pending;
};

struct Fidelity {
    bool has_windows = false, has_identity = false;
    ULONG major = 10, minor = 0, build = 19045, ubr = 3803;
    std::string csd, product_name = "Windows 10 Pro", display_version = "22H2";
    std::string computer_name = "DESKTOP-XBOX", user_name = "User";
    std::string user_sid = "S-1-5-21-1004336348-1177238915-682003330-1001";
    std::string cpu_vendor = "AuthenticAMD";
};

// The overlay as Initialize left it (fidelity seeds, COM classes...): what persistence never writes back, so a later
// machine.json edit still shows. Keyed by the upper-cased key path.
struct BaseVal { ULONG type = 0; bool deleted = false; std::vector<std::uint8_t> data; };
struct BaseKey { std::uint32_t fl = 0; std::wstring cls; std::map<std::wstring, BaseVal> vals; };

struct State {
    std::wstring root_dir;
    std::vector<std::unique_ptr<Node>> arena;
    Node* root = nullptr;     // \REGISTRY
    Node* machine = nullptr;  // \REGISTRY\MACHINE
    Node* user = nullptr;     // \REGISTRY\USER
    Node* system = nullptr;   // \REGISTRY\MACHINE\SYSTEM
    std::unordered_map<KeyId, HandleEnt> handles;
    KeyId next_handle = 1;
    std::list<std::shared_ptr<Watch>> watches;
    std::uint64_t init_time = 0;
    Fidelity fid;
    std::unordered_map<std::wstring, BaseKey> base;   // EnablePersistence: the seeded overlay
    bool persist = false;                // EnablePersistence ran
    bool persist_warned = false;         // the FIXME for an unwritable machine folder was logged
    std::string persisted;               // the serialized overlay as last loaded or saved
};

std::mutex g_mu;
std::condition_variable g_cv;
std::unique_ptr<State> g_st;
HostPathResolver g_resolver;
std::atomic<OpenDirectoryFn> g_ns_open{nullptr};
std::atomic<CloseObjectFn> g_ns_close{nullptr};

// ---- small helpers -----------------------------------------------------------

std::uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::uint8_t> StrData(const std::wstring& s) {
    std::vector<std::uint8_t> d((s.size() + 1) * 2, 0);
    std::memcpy(d.data(), s.data(), s.size() * 2);
    return d;
}

std::vector<std::uint8_t> DwordData(std::uint32_t v) {
    std::vector<std::uint8_t> d(4);
    std::memcpy(d.data(), &v, 4);
    return d;
}

std::wstring_view View(const UNICODE_STRING* us) {
    if (!us || !us->Buffer) return {};
    return std::wstring_view(us->Buffer, us->Length / sizeof(wchar_t));
}

void Put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) { std::memcpy(v.data() + off, &x, 4); }
void Put64(std::vector<std::uint8_t>& v, std::size_t off, std::uint64_t x) { std::memcpy(v.data() + off, &x, 8); }
void PutBytes(std::vector<std::uint8_t>& v, std::size_t off, const void* p, std::size_t n) { if (n) std::memcpy(v.data() + off, p, n); }

std::vector<std::wstring_view> Split(std::wstring_view p) {
    std::vector<std::wstring_view> out;
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == L'\\') ++i;
        std::size_t j = i;
        while (j < p.size() && p[j] != L'\\') ++j;
        if (j > i) out.push_back(p.substr(i, j - i));
        i = j;
    }
    return out;
}

ACCESS_MASK MapAccess(ACCESS_MASK a) {
    if (a & kMaximumAllowed) a |= kKeyAll;
    if (a & kGenericRead) a |= kKeyRead;
    if (a & kGenericWrite) a |= kKeyWrite;
    if (a & kGenericExecute) a |= kKeyRead;
    if (a & kGenericAll) a |= kKeyAll;
    return a & ~(kMaximumAllowed | kGenericRead | kGenericWrite | kGenericExecute | kGenericAll);
}

bool Backed(const Node* n) { return n->hive && n->nk; }

Node* NewNode(State& st, std::wstring name, Node* parent) {
    st.arena.push_back(std::make_unique<Node>());
    Node* n = st.arena.back().get();
    n->name = std::move(name);
    n->parent = parent;
    return n;
}

bool IsAncestorOrSelf(const Node* a, const Node* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

std::wstring PathOf(const Node* n) {
    std::vector<const Node*> chain;
    for (; n; n = n->parent) chain.push_back(n);
    std::wstring p;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) { p += L'\\'; p += (*it)->name; }
    return p;
}

// ---- change notification ----------------------------------------------------

void CompleteWatch(Watch& w, NTSTATUS status) {
    for (auto& p : w.pending) {
        p->done = true;
        if (p->iosb) {
            // The block belongs to the caller and may be gone by now (another thread freed it): a failed store is dropped.
            IO_STATUS_BLOCK done{};
            done.Status = status;
            done.Information = 0;
            probe::Store<IO_STATUS_BLOCK>(p->iosb, done);
        }
        if (p->event) SetEvent(p->event);
    }
    w.pending.clear();
}

void NotifyChange(State& st, Node* n, ULONG filter) {
    for (auto& w : st.watches)
        if (!w->pending.empty() && (w->filter & filter) && (w->node == n || (w->tree && IsAncestorOrSelf(w->node, n))))
            CompleteWatch(*w, 0);
    g_cv.notify_all();
}

void Invalidate(Node* n) { n->kids_valid = false; n->vals_valid = false; }

// ---- merged view of a key ---------------------------------------------------

Node* FindChild(State& st, Node* n, std::wstring_view comp);

const std::vector<ChildEnt>& Children(State& st, Node* n) {
    if (n->kids_valid) return n->kid_cache;
    n->kid_cache.clear();
    if (!n->deleted && Backed(n)) {
        std::vector<std::uint32_t> nks;
        n->hive->NkSubkeys(n->nk, &nks);
        for (std::uint32_t nk : nks) {
            if (!n->hive->IsNk(nk)) continue;
            ChildEnt e;
            e.name = n->hive->NkName(nk);
            e.up = Upcase(e.name);
            e.nk = nk;
            auto it = n->kids.find(e.up);
            if (it != n->kids.end()) {
                Node* k = it->second;
                if (k->shadow && k->nk == nk && !k->deleted) { e.node = k; n->kid_cache.push_back(std::move(e)); }
                continue;   // replaced or deleted in the overlay
            }
            n->kid_cache.push_back(std::move(e));
        }
    }
    if (!n->deleted) {
        for (auto& [up, k] : n->kids) {
            if (k->deleted || k->shadow) continue;
            ChildEnt e;
            e.name = k->name; e.up = up; e.nk = 0; e.node = k;
            bool dup = false;
            for (auto& x : n->kid_cache) if (x.up == up) { dup = true; break; }
            if (!dup) n->kid_cache.push_back(std::move(e));
        }
        if (n == st.system) {
            bool have = false;
            for (auto& x : n->kid_cache) if (x.up == L"CURRENTCONTROLSET") { have = true; break; }
            if (!have) {
                ChildEnt e;
                e.name = L"CurrentControlSet"; e.up = L"CURRENTCONTROLSET";
                n->kid_cache.push_back(std::move(e));
            }
        }
    }
    std::stable_sort(n->kid_cache.begin(), n->kid_cache.end(),
                     [](const ChildEnt& a, const ChildEnt& b) { return a.up < b.up; });
    n->kids_valid = true;
    return n->kid_cache;
}

bool FindValue(Node* n, std::wstring_view name, Val* out) {
    if (n->deleted || n->container) return false;
    std::wstring up = Upcase(name);
    auto it = n->ov.find(up);
    if (it != n->ov.end()) {
        if (it->second.deleted) return false;
        out->name = it->second.name; out->type = it->second.type; out->data = it->second.data;
        return true;
    }
    if (Backed(n)) {
        std::uint32_t vk = n->hive->NkFindValue(n->nk, name);
        if (!vk) return false;
        out->name = n->hive->VkName(vk);
        out->type = n->hive->VkType(vk);
        return n->hive->VkData(vk, &out->data);
    }
    return false;
}

const std::vector<Val>& Values(Node* n) {
    if (n->vals_valid) return n->val_cache;
    n->val_cache.clear();
    std::set<std::wstring> used;
    if (!n->deleted && !n->container && Backed(n)) {
        std::vector<std::uint32_t> vks;
        n->hive->NkValues(n->nk, &vks);
        for (std::uint32_t vk : vks) {
            Val v;
            v.name = n->hive->VkName(vk);
            std::wstring up = Upcase(v.name);
            auto it = n->ov.find(up);
            if (it != n->ov.end()) {
                used.insert(up);
                if (it->second.deleted) continue;
                v.type = it->second.type; v.data = it->second.data;
            } else {
                v.type = n->hive->VkType(vk);
                if (!n->hive->VkData(vk, &v.data)) continue;
            }
            n->val_cache.push_back(std::move(v));
        }
    }
    if (!n->deleted && !n->container) {
        std::vector<const OvValue*> extra;
        for (auto& [up, o] : n->ov) if (!o.deleted && !used.count(up)) extra.push_back(&o);
        std::sort(extra.begin(), extra.end(), [](const OvValue* a, const OvValue* b) { return a->seq < b->seq; });
        for (auto* o : extra) n->val_cache.push_back(Val{o->name, o->type, o->data});
    }
    n->vals_valid = true;
    return n->val_cache;
}

std::wstring ClassOf(const Node* n) {
    if (n->has_class) return n->cls;
    return Backed(n) ? n->hive->NkClass(n->nk) : std::wstring();
}

std::uint64_t TimeOf(const State& st, const Node* n) {
    if (n->mtime) return n->mtime;
    if (Backed(n)) { std::uint64_t t = n->hive->NkTimestamp(n->nk); if (t) return t; }
    return st.init_time;
}

bool IsLink(const Node* n) {
    return n->link || (Backed(n) && (n->hive->NkFlags(n->nk) & 0x10));
}

bool LinkTarget(Node* n, std::wstring* target) {
    Val v;
    if (!FindValue(n, L"SymbolicLinkValue", &v) || v.type != kRegLink) return false;
    target->assign(reinterpret_cast<const wchar_t*>(v.data.data()), v.data.size() / 2);
    return !target->empty();
}

Node* SelectedControlSet(State& st) {
    ULONG cur = 1;
    Node* sel = FindChild(st, st.system, L"Select");
    Val v;
    if (sel && FindValue(sel, L"Current", &v) && v.type == 4 && v.data.size() >= 4) std::memcpy(&cur, v.data.data(), 4);
    std::wstring nm = std::to_wstring(cur);
    while (nm.size() < 3) nm.insert(nm.begin(), L'0');
    Node* cs = FindChild(st, st.system, L"ControlSet" + nm);
    if (!cs) cs = FindChild(st, st.system, L"ControlSet001");
    return cs;
}

Node* FindChild(State& st, Node* n, std::wstring_view comp) {
    if (n->deleted) return nullptr;
    std::wstring up = Upcase(comp);
    auto it = n->kids.find(up);
    if (it != n->kids.end()) return it->second->deleted ? nullptr : it->second;
    if (Backed(n)) {
        std::uint32_t nk = n->hive->NkFindSubkey(n->nk, comp);
        if (nk) {
            Node* c = NewNode(st, n->hive->NkName(nk), n);
            c->hive = n->hive; c->nk = nk; c->shadow = true;
            n->kids[up] = c;
            return c;
        }
    }
    if (n == st.system && up == L"CURRENTCONTROLSET") return SelectedControlSet(st);
    return nullptr;
}

Node* EnsureNode(State& st, Node* parent, const ChildEnt& e) {
    return e.node ? e.node : FindChild(st, parent, e.name);
}

// ---- mutation helpers ---------------------------------------------------------

void MarkDeleted(State& st, Node* n) {
    if (n->deleted) return;
    n->deleted = true;
    for (auto& [up, k] : n->kids) MarkDeleted(st, k);
    Invalidate(n);
    if (n->parent) { Invalidate(n->parent); NotifyChange(st, n->parent, kNotifyName); }
    for (auto& w : st.watches) if (w->node == n) CompleteWatch(*w, kStatusNotifyCleanup);
    g_cv.notify_all();
}

Node* CreateChild(State& st, Node* parent, std::wstring_view comp) {
    Node* c = NewNode(st, std::wstring(comp), parent);
    c->mtime = NowFileTime();
    parent->kids[Upcase(comp)] = c;
    Invalidate(parent);
    parent->mtime = c->mtime;
    NotifyChange(st, parent, kNotifyName);
    return c;
}

// Make a node empty and detached from its hive (used by restore and by
// overlay replay of "deleted then re-created" keys).
void ResetKey(State& st, Node* n) {
    for (auto& [up, k] : n->kids) MarkDeleted(st, k);
    n->kids.clear();
    n->ov.clear();
    n->hive.reset();
    n->nk = 0;
    n->shadow = false;
    n->has_class = false; n->cls.clear();
    n->mtime = NowFileTime();
    Invalidate(n);
    if (n->parent) Invalidate(n->parent);
}

void StoreValue(State& st, Node* n, std::wstring_view name, ULONG type, const void* data, std::size_t size) {
    Val existing;
    std::wstring stored(name);
    if (FindValue(n, name, &existing)) stored = existing.name;
    OvValue& o = n->ov[Upcase(name)];
    o.name = std::move(stored);
    o.type = type;
    o.data.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    o.deleted = false;
    o.seq = ++n->ov_seq;
    n->mtime = NowFileTime();
    n->vals_valid = false;
    NotifyChange(st, n, kNotifyLastSet);
}

// ---- path resolution -----------------------------------------------------------

struct ResolveOpts {
    bool create = false;
    bool parents = false;     // create missing intermediate keys as well (internal callers; NtCreateKey never does)
    bool can_create = true;
    bool open_link = false;
    ULONG create_options = 0;
    const std::wstring* cls = nullptr;
};

// An absolute name that does not start at \Registry belongs to the object manager. NtOpenKey on Windows gets the
// namespace's answer: the leaf is missing (NAME_NOT_FOUND), a directory on the way is (PATH_NOT_FOUND), or the object
// is no key (TYPE_MISMATCH, e.g. \Device\Null). NtOpenDirectoryObject asks the host namespace the guest shares; the one
// thing it does differently is a device as the leaf (PATH_NOT_FOUND, its parse routine has no name to parse), which is
// told from a missing directory by the parent: if that is not missing, the leaf is an object that is no directory.
// The host functions come from SetHostNamespace: once a guest runs, "ntdll.dll" in the process names Wine's ntdll.
NTSTATUS NamespaceStatus(std::wstring_view path, std::size_t components) {
    OpenDirectoryFn open = g_ns_open.load();
    CloseObjectFn close = g_ns_close.load();
    if (!open || !close) {   // no dispatcher (unit tests): the process's own ntdll is the host's
        const HMODULE host = GetModuleHandleW(L"ntdll.dll");
        open = host ? reinterpret_cast<OpenDirectoryFn>(GetProcAddress(host, "NtOpenDirectoryObject")) : nullptr;
        close = host ? reinterpret_cast<CloseObjectFn>(GetProcAddress(host, "NtClose")) : nullptr;
    }
    if (!open || !close || path.size() >= 0x7FFF) return components > 1 ? kStatusObjectPathNotFound : nt::kObjectNameNotFound;
    auto open_directory = [&](std::wstring name) {
        UNICODE_STRING us;
        us.Buffer = name.data();
        us.Length = us.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        OBJECT_ATTRIBUTES oa{};
        oa.Length = sizeof oa;
        oa.ObjectName = &us;
        oa.Attributes = 0x40;   // OBJ_CASE_INSENSITIVE
        HANDLE h = nullptr;
        const NTSTATUS r = open(&h, 1 /*DIRECTORY_QUERY*/, &oa);
        if (r >= 0) close(h);
        return r;
    };
    const NTSTATUS s = open_directory(std::wstring(path));
    if (s >= 0) return nt::kObjectTypeMismatch;   // a directory is no key
    if (s == kStatusObjectPathNotFound && components > 1) {
        std::size_t end = path.size();
        while (end > 1 && path[end - 1] == L'\\') --end;   // trailing separators
        const std::size_t cut = path.rfind(L'\\', end - 1);
        if (cut != std::wstring_view::npos && cut > 0) {
            const NTSTATUS parent = open_directory(std::wstring(path.substr(0, cut)));
            TRACE(registry, "namespace %ls: %08lx, parent %08lx", std::wstring(path).c_str(),
                  static_cast<unsigned long>(s), static_cast<unsigned long>(parent));
            if (parent != nt::kObjectNameNotFound && parent != kStatusObjectPathNotFound) return nt::kObjectTypeMismatch;
        }
    }
    return s;
}

// Windows creates only the last component of a key name: a missing key on the way is NAME_NOT_FOUND, as for an open.
// `parents` lets the module's own set-up code create a whole path.
NTSTATUS Resolve(State& st, Node* start, std::wstring_view path, const ResolveOpts& o, Node** out,
                 bool* created, int depth = 0) {
    if (created) *created = false;
    Node* cur = start;
    auto comps = Split(path);
    std::size_t first = 0;
    if (!cur) {   // absolute
        if (path.size() >= 2 && path[1] == L'\\') return nt::kObjectNameInvalid;   // an empty first component
        if (comps.empty()) return nt::kObjectTypeMismatch;                         // "\" is the root directory object
        if (!regf::EqualNoCase(comps[0], L"REGISTRY")) return NamespaceStatus(path, comps.size());
        cur = st.root;
        first = 1;
    }
    for (std::size_t i = first; i < comps.size(); ++i) {
        std::wstring_view comp = comps[i];
        const bool last = i + 1 == comps.size();
        if (cur->deleted) return nt::kKeyDeleted;
        Node* next = FindChild(st, cur, comp);
        if (!next) {
            if (!o.create || (!last && !o.parents)) return nt::kObjectNameNotFound;
            if (cur->container) return nt::kAccessDenied;
            if (!o.can_create) return nt::kAccessDenied;
            if (comp.size() > kMaxKeyNameChars) return nt::kInvalidParameter;
            if (last && !o.parents && cur->is_volatile && !(o.create_options & kRegOptionVolatile))
                return kStatusChildMustBeVolatile;
            next = CreateChild(st, cur, comp);
            if (created) *created = true;
            if (last) {
                if (o.create_options & kRegOptionCreateLink) next->link = true;
                if (o.create_options & kRegOptionVolatile) next->is_volatile = true;
                if (o.cls && !o.cls->empty()) { next->has_class = true; next->cls = *o.cls; }
            }
            if (cur->is_volatile) next->is_volatile = true;
        } else if (IsLink(next) && (!last || !o.open_link)) {
            std::wstring target;
            if (depth >= kMaxLinkDepth) return nt::kInvalidParameter;
            if (!LinkTarget(next, &target)) return nt::kObjectNameNotFound;
            if (target.front() != L'\\') return nt::kObjectNameInvalid;   // a relative target is no path
            Node* t = nullptr;
            ResolveOpts lo;
            if (last && o.create) {   // creating through a link whose target is gone creates the target
                lo.create = true;
                lo.can_create = o.can_create;
                lo.create_options = o.create_options & kRegOptionVolatile;
                lo.cls = o.cls;
            }
            bool target_created = false;
            NTSTATUS s = Resolve(st, nullptr, target, lo, &t, &target_created, depth + 1);
            if (s < 0) return s;
            if (target_created && created) *created = true;
            next = t;
        }
        cur = next;
    }
    if (cur->deleted) return nt::kKeyDeleted;
    *out = cur;
    return nt::kSuccess;
}

// ---- handles -------------------------------------------------------------------

HANDLE EncodeHandle(KeyId id) {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(kHandleTag | ((id << 2) & kHandleIdMask)));
}

KeyId DecodeHandle(HANDLE h) {
    std::uint64_t v = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
    if ((v & ~kHandleIdMask) != kHandleTag || (v & 3)) return 0;
    return (v & kHandleIdMask) >> 2;
}

KeyId NewHandle(State& st, Node* n, ACCESS_MASK access) {
    KeyId id = st.next_handle++;
    st.handles[id] = HandleEnt{n, access};
    return id;
}

NTSTATUS GetHandle(State& st, HANDLE h, ACCESS_MASK need, Node** out) {
    KeyId id = DecodeHandle(h);
    auto it = id ? st.handles.find(id) : st.handles.end();
    if (it == st.handles.end()) return nt::kInvalidHandle;
    if ((it->second.access & need) != need) return nt::kAccessDenied;
    if (it->second.node->deleted) return nt::kKeyDeleted;
    *out = it->second.node;
    return nt::kSuccess;
}

// Parses OBJECT_ATTRIBUTES into (start node, relative-or-absolute path).
NTSTATUS ParseOA(State& st, POBJECT_ATTRIBUTES oa, Node** start, std::wstring_view* path, ACCESS_MASK* root_access) {
    if (!oa || oa->Length != sizeof(OBJECT_ATTRIBUTES) || !oa->ObjectName) return nt::kInvalidParameter;
    *path = View(oa->ObjectName);
    *start = nullptr;
    *root_access = kKeyAll;
    if (oa->RootDirectory) {
        KeyId id = DecodeHandle(oa->RootDirectory);
        auto it = id ? st.handles.find(id) : st.handles.end();
        if (it == st.handles.end()) return nt::kInvalidHandle;
        if (it->second.node->deleted) return nt::kKeyDeleted;
        if (!path->empty() && path->front() == L'\\') return nt::kObjectPathSyntaxBad;
        *start = it->second.node;
        *root_access = it->second.access;
    } else if (path->empty() || path->front() != L'\\') {
        return nt::kObjectPathSyntaxBad;
    }
    return nt::kSuccess;
}

// The caller's OBJECT_ATTRIBUTES is read once, here: NULL is an access violation (it is required), so is memory that
// cannot be read, and a Length other than sizeof(OBJECT_ATTRIBUTES) is STATUS_INVALID_PARAMETER.
NTSTATUS CaptureOA(const OBJECT_ATTRIBUTES* user, probe::ObjectAttributes* out) {
    if (!user) return probe::kAccessViolation;
    return probe::CaptureObjectAttributes(user, out);
}

NTSTATUS OpenCommon(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES user_oa, ULONG options) {
    // Windows probes the handle first, then captures the attributes, and a failed open leaves the handle NULL.
    if (NTSTATUS s = probe::ProbeWrite(KeyHandle, sizeof(HANDLE))) return s;
    probe::ObjectAttributes cap;
    if (NTSTATUS s = CaptureOA(user_oa, &cap)) return s;
    const OBJECT_ATTRIBUTES* oa = &cap.oa;
    if (NTSTATUS s = probe::Store<HANDLE>(KeyHandle, nullptr)) return s;
    if (!DesiredAccess) return nt::kAccessDenied;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* start; std::wstring_view path; ACCESS_MASK ra;
    NTSTATUS s = ParseOA(st, const_cast<POBJECT_ATTRIBUTES>(oa), &start, &path, &ra);
    if (s < 0) return s;
    ResolveOpts ro;
    (void)options;   // REG_OPTION_OPEN_LINK does not matter for an open: only OBJ_OPENLINK does
    ro.open_link = (oa->Attributes & kObjOpenLink) != 0;
    Node* n = nullptr;
    s = Resolve(st, start, path, ro, &n, nullptr);
    if (s < 0) {
        TRACE(registry, "open %ls (under %ls) failed: %08lx", std::wstring(path).c_str(),
              start ? PathOf(start).c_str() : L"\\", static_cast<unsigned long>(s));
        return s;
    }
    const HANDLE key = EncodeHandle(NewHandle(st, n, MapAccess(DesiredAccess)));
    if (NTSTATUS ws = probe::Store<HANDLE>(KeyHandle, key)) {
        st.handles.erase(DecodeHandle(key));   // the caller's handle slot vanished: do not leak the handle
        return ws;
    }
    TRACE(registry, "open %ls -> %p", PathOf(n).c_str(), key);
    return nt::kSuccess;
}

// ---- information buffers -------------------------------------------------------

NTSTATUS Emit(const std::vector<std::uint8_t>& full, std::size_t fixed, PVOID buf, ULONG Length, PULONG ResultLength) {
    // Both caller buffers are probed before either is written: a bad pointer is an access violation, not a fault.
    if (NTSTATUS s = probe::ProbeWrite(buf, Length)) return s;
    if (ResultLength)
        if (NTSTATUS s = probe::ProbeWrite(ResultLength, sizeof(ULONG), sizeof(ULONG))) return s;
    if (ResultLength)
        if (NTSTATUS s = probe::Store<ULONG>(ResultLength, static_cast<ULONG>(full.size()))) return s;
    if (Length < fixed) return nt::kBufferTooSmall;
    std::size_t n = std::min<std::size_t>(Length, full.size());
    if (n)
        if (NTSTATUS s = probe::CopyToUser(buf, full.data(), n)) return s;
    return n < full.size() ? nt::kBufferOverflow : nt::kSuccess;
}

// Builds KEY_*_INFORMATION for key `n` (reported leaf name `name`). NtEnumerateKey answers for the enumerated subkey in
// the classes Wine's server handles (basic, node, full, name, cached); anything else is an invalid class.
NTSTATUS BuildKeyInfo(State& st, ULONG cls, Node* n, const std::wstring& name, bool for_enumerate,
                      std::vector<std::uint8_t>* out, std::size_t* fixed) {
    const std::uint64_t t = TimeOf(st, n);
    const std::wstring& shown = cls == kKeyName ? PathOf(n) : name;
    const std::size_t nb = shown.size() * 2;
    switch (cls) {
        case kKeyBasic:
            *fixed = 16;
            out->assign(16 + nb, 0);
            Put64(*out, 0, t); Put32(*out, 12, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 16, shown.data(), nb);
            return nt::kSuccess;
        case kKeyNode: {
            std::wstring c = ClassOf(n);
            const std::size_t cb = c.size() * 2;
            *fixed = 24;
            out->assign(24 + nb + cb, 0);
            Put64(*out, 0, t);
            Put32(*out, 12, cb ? static_cast<std::uint32_t>(24 + nb) : 0xFFFFFFFFu);
            Put32(*out, 16, static_cast<std::uint32_t>(cb));
            Put32(*out, 20, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 24, shown.data(), nb);
            PutBytes(*out, 24 + nb, c.data(), cb);
            return nt::kSuccess;
        }
        case kKeyName:
            *fixed = 4;
            out->assign(4 + nb, 0);
            Put32(*out, 0, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 4, shown.data(), nb);
            return nt::kSuccess;
        case kKeyFull:
        case kKeyCached: {
            const auto& kids = Children(st, n);
            const auto& vals = Values(n);
            std::uint32_t max_name = 0, max_cls = 0, max_vname = 0, max_vdata = 0;
            for (auto& e : kids) {
                max_name = std::max<std::uint32_t>(max_name, static_cast<std::uint32_t>(e.name.size() * 2));
                if (Node* c = EnsureNode(st, n, e)) max_cls = std::max<std::uint32_t>(max_cls, static_cast<std::uint32_t>(ClassOf(c).size() * 2));
            }
            for (auto& v : vals) {
                max_vname = std::max<std::uint32_t>(max_vname, static_cast<std::uint32_t>(v.name.size() * 2));
                max_vdata = std::max<std::uint32_t>(max_vdata, static_cast<std::uint32_t>(v.data.size()));
            }
            if (cls == kKeyFull) {
                std::wstring c = ClassOf(n);
                const std::size_t cb = c.size() * 2;
                *fixed = 44;
                out->assign(44 + cb, 0);
                Put64(*out, 0, t);
                Put32(*out, 12, 44);
                Put32(*out, 16, static_cast<std::uint32_t>(cb));
                Put32(*out, 20, static_cast<std::uint32_t>(kids.size()));
                Put32(*out, 24, max_name); Put32(*out, 28, max_cls);
                Put32(*out, 32, static_cast<std::uint32_t>(vals.size()));
                Put32(*out, 36, max_vname); Put32(*out, 40, max_vdata);
                PutBytes(*out, 44, c.data(), cb);
            } else {
                // KEY_CACHED_INFORMATION has no name: LastWriteTime, TitleIndex, SubKeys, MaxNameLen, Values,
                // MaxValueNameLen, MaxValueDataLen, NameLength (the leaf's), padded to 40 bytes.
                *fixed = 40;
                out->assign(40, 0);
                Put64(*out, 0, t);
                Put32(*out, 12, static_cast<std::uint32_t>(kids.size()));
                Put32(*out, 16, max_name);
                Put32(*out, 20, static_cast<std::uint32_t>(vals.size()));
                Put32(*out, 24, max_vname); Put32(*out, 28, max_vdata);
                Put32(*out, 32, static_cast<std::uint32_t>(n->name.size() * 2));
            }
            return nt::kSuccess;
        }
        case kKeyFlags:
        case kKeyVirtualization:
        case kKeyHandleTags:
            if (for_enumerate) return nt::kInvalidInfoClass;
            *fixed = 4;
            out->assign(4, 0);
            return nt::kSuccess;
        default:
            return nt::kInvalidInfoClass;
    }
}

NTSTATUS BuildValueInfo(ULONG cls, const Val& v, std::vector<std::uint8_t>* out, std::size_t* fixed) {
    const std::size_t nb = v.name.size() * 2;
    const std::size_t dl = v.data.size();
    switch (cls) {
        case kValueBasic:
            *fixed = 12;
            out->assign(12 + nb, 0);
            Put32(*out, 4, v.type); Put32(*out, 8, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 12, v.name.data(), nb);
            return nt::kSuccess;
        case kValueFull: {
            *fixed = 20;
            std::size_t doff = (20 + nb + 3) & ~static_cast<std::size_t>(3);
            out->assign(doff + dl, 0);
            Put32(*out, 4, v.type); Put32(*out, 8, static_cast<std::uint32_t>(doff));
            Put32(*out, 12, static_cast<std::uint32_t>(dl)); Put32(*out, 16, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 20, v.name.data(), nb);
            PutBytes(*out, doff, v.data.data(), dl);
            return nt::kSuccess;
        }
        case kValueFullAlign64: {
            *fixed = 12;
            std::size_t doff = (12 + nb + 7) & ~static_cast<std::size_t>(7);
            out->assign(doff + dl, 0);
            Put32(*out, 0, v.type); Put32(*out, 4, static_cast<std::uint32_t>(dl)); Put32(*out, 8, static_cast<std::uint32_t>(nb));
            PutBytes(*out, 12, v.name.data(), nb);
            PutBytes(*out, doff, v.data.data(), dl);
            return nt::kSuccess;
        }
        case kValuePartial:
            *fixed = 12;
            out->assign(12 + dl, 0);
            Put32(*out, 4, v.type); Put32(*out, 8, static_cast<std::uint32_t>(dl));
            PutBytes(*out, 12, v.data.data(), dl);
            return nt::kSuccess;
        case kValuePartialAlign64:
            *fixed = 8;
            out->assign(8 + dl, 0);
            Put32(*out, 0, v.type); Put32(*out, 4, static_cast<std::uint32_t>(dl));
            PutBytes(*out, 8, v.data.data(), dl);
            return nt::kSuccess;
        default:
            return nt::kInvalidParameter;
    }
}

// ---- snapshot / import --------------------------------------------------------

bool Snapshot(State& st, Node* n, regf::Key* out, int depth) {
    if (depth > 512) return false;
    out->name = n->name;
    out->class_name = ClassOf(n);
    out->timestamp = TimeOf(st, n);
    out->symlink = IsLink(n);
    for (auto& v : Values(n)) out->values.push_back(regf::Value{v.name, v.type, v.data});
    std::vector<ChildEnt> kids = Children(st, n);   // copy: recursion may invalidate caches
    for (auto& e : kids) {
        if (e.up == L"CURRENTCONTROLSET" && e.nk == 0 && !e.node && n == st.system) continue;
        Node* c = EnsureNode(st, n, e);
        if (!c) continue;
        auto k = std::make_unique<regf::Key>();
        if (Snapshot(st, c, k.get(), depth + 1)) out->subkeys.push_back(std::move(k));
    }
    return true;
}

void Materialize(State& st, Node* dst, const regf::Key& k, int depth) {
    if (depth > 512) return;
    if (!k.class_name.empty()) { dst->has_class = true; dst->cls = k.class_name; }
    dst->link = k.symlink;
    for (auto& v : k.values) StoreValue(st, dst, v.name, v.type, v.data.data(), v.data.size());
    for (auto& c : k.subkeys) {
        Node* ch = FindChild(st, dst, c->name);
        if (ch) MarkDeleted(st, ch);
        ch = CreateChild(st, dst, c->name);
        Materialize(st, ch, *c, depth + 1);
    }
}

// ---- machine.json / fidelity -----------------------------------------------------

std::string ReadFileBytes(const std::wstring& path, bool strip_bom) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[8192];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) s.append(buf, n);
    CloseHandle(f);
    if (strip_bom && s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

void LoadFidelity(State& st) {
    Fidelity& f = st.fid;
    const auto& c = config::Current();
    f.major = c.os_major; f.minor = c.os_minor; f.build = c.os_build; f.ubr = c.os_ubr;
    f.csd.clear();
    if (!c.os_csd.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, c.os_csd.data(), static_cast<int>(c.os_csd.size()), nullptr, 0, nullptr, nullptr);
        f.csd.assign(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, c.os_csd.data(), static_cast<int>(c.os_csd.size()), f.csd.data(), n, nullptr, nullptr);
    }
    std::string text = ReadFileBytes(st.root_dir + L"\\machine.json", true);
    if (text.empty()) return;
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(&root) || !root.IsObject()) { WARN(registry, "machine.json is not valid JSON; ignored"); return; }
    auto str = [](const json::Value* o, const char* k, std::string* dst) {
        const json::Value* v = o ? o->Find(k) : nullptr;
        if (v && v->type == json::Value::Type::String) *dst = v->str;
    };
    auto num = [](const json::Value* o, const char* k, ULONG* dst) {
        const json::Value* v = o ? o->Find(k) : nullptr;
        if (v && v->type == json::Value::Type::Number) *dst = static_cast<ULONG>(v->num);
    };
    if (const json::Value* w = root.Find("windows"); w && w->IsObject()) {
        f.has_windows = true;
        num(w, "major", &f.major); num(w, "minor", &f.minor); num(w, "build", &f.build); num(w, "ubr", &f.ubr);
        str(w, "product_name", &f.product_name); str(w, "display_version", &f.display_version); str(w, "csd", &f.csd);
    }
    if (const json::Value* i = root.Find("identity"); i && i->IsObject()) {
        f.has_identity = true;
        str(i, "computer_name", &f.computer_name); str(i, "user_name", &f.user_name); str(i, "user_sid", &f.user_sid);
    }
    if (const json::Value* cpu = root.Find("cpu")) str(cpu, "vendor", &f.cpu_vendor);
}

// Create (if needed) the key at `rel` under `base`, ignoring access checks.
Node* EnsureKey(State& st, Node* base, std::wstring_view rel) {
    ResolveOpts o;
    o.create = true;
    o.parents = true;
    Node* out = nullptr;
    return Resolve(st, base, rel, o, &out, nullptr) >= 0 ? out : nullptr;
}

void SetStr(State& st, Node* base, std::wstring_view key, const wchar_t* name, const std::wstring& v, ULONG type = 1) {
    if (Node* k = EnsureKey(st, base, key)) { auto d = StrData(v); StoreValue(st, k, name, type, d.data(), d.size()); }
}
void SetDword(State& st, Node* base, std::wstring_view key, const wchar_t* name, ULONG v) {
    if (Node* k = EnsureKey(st, base, key)) { auto d = DwordData(v); StoreValue(st, k, name, 4, d.data(), d.size()); }
}

// The host's own base clock of logical processor 0, in MHz: the value Windows keeps at
// HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\~MHz (under Wine the host registry carries the same value, taken
// from /proc/cpuinfo). 0 when the host does not answer; the caller then leaves ~MHz out rather than invent one.
ULONG HostBaseMhz() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &k) !=
        ERROR_SUCCESS)
        return 0;
    DWORD type = 0, v = 0, size = sizeof v;
    const LONG r = RegQueryValueExW(k, L"~MHz", nullptr, &type, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD ? v : 0;
}

void ApplySoftwareFidelity(State& st, Node* sw) {
    const Fidelity& f = st.fid;
    const std::wstring cv = L"Microsoft\\Windows NT\\CurrentVersion";
    std::wstring build = std::to_wstring(f.build);
    std::wstring prod = Widen(f.product_name);
    SetStr(st, sw, cv, L"ProductName", prod);
    SetStr(st, sw, cv, L"EditionID", prod.find(L"Home") != std::wstring::npos ? L"Core" : L"Professional");
    SetStr(st, sw, cv, L"InstallationType", L"Client");
    SetStr(st, sw, cv, L"CurrentBuild", build);
    SetStr(st, sw, cv, L"CurrentBuildNumber", build);
    SetStr(st, sw, cv, L"CurrentVersion", L"6.3");
    SetDword(st, sw, cv, L"CurrentMajorVersionNumber", f.major);
    SetDword(st, sw, cv, L"CurrentMinorVersionNumber", f.minor);
    SetDword(st, sw, cv, L"UBR", f.ubr);
    SetDword(st, sw, cv, L"BaseBuildRevisionNumber", 1);
    SetStr(st, sw, cv, L"DisplayVersion", Widen(f.display_version));
    SetStr(st, sw, cv, L"ReleaseId", L"2009");
    SetStr(st, sw, cv, L"BuildLab", build + L".vb_release.191206-1406");
    SetStr(st, sw, cv, L"BuildLabEx", build + L".1.amd64fre.vb_release.191206-1406");
    SetStr(st, sw, cv, L"SystemRoot", L"C:\\Windows");
    SetStr(st, sw, cv, L"PathName", L"C:\\Windows");
    if (!f.csd.empty()) SetStr(st, sw, cv, L"CSDVersion", Widen(f.csd));
    const std::wstring wcv = L"Microsoft\\Windows\\CurrentVersion";
    Val pv;
    Node* k = EnsureKey(st, sw, wcv);
    if (k && !FindValue(k, L"ProgramFilesDir", &pv)) {   // never override a real hive's paths
        SetStr(st, sw, wcv, L"ProgramFilesDir", L"C:\\Program Files");
        SetStr(st, sw, wcv, L"ProgramFilesDir (x86)", L"C:\\Program Files (x86)");
        SetStr(st, sw, wcv, L"ProgramW6432Dir", L"C:\\Program Files");
        SetStr(st, sw, wcv, L"CommonFilesDir", L"C:\\Program Files\\Common Files");
    }
}

// The key tree every Windows SOFTWARE hive starts with and programs (ntdll's reg test, COM, the shell) expect to find:
// Classes with its registration trees, the 32-bit view Wow6432Node and Classes\Wow6432Node (Wow6432Node\Classes is
// the link Windows keeps to it), and COM's own IUnknown in Classes\Interface (Windows registers it in both views;
// Interface is never an empty key). Used when no SOFTWARE hive is supplied; a supplied hive has all of it already.
void ApplySoftwareSkeleton(State& st, Node* sw) {
    for (const wchar_t* k : {L"Classes\\Applications", L"Classes\\CLSID", L"Classes\\Directory", L"Classes\\Folder",
                            L"Classes\\TypeLib", L"Classes\\Wow6432Node\\CLSID", L"Classes\\Wow6432Node\\TypeLib"})
        EnsureKey(st, sw, k);
    for (const wchar_t* base : {L"Classes\\Interface\\", L"Classes\\Wow6432Node\\Interface\\"}) {
        const std::wstring iunknown = std::wstring(base) + L"{00000000-0000-0000-C000-000000000046}";
        SetStr(st, sw, iunknown, L"", L"IUnknown");
        SetStr(st, sw, iunknown + L"\\NumMethods", L"", L"3");
        SetStr(st, sw, iunknown + L"\\ProxyStubClsid32", L"", L"{00020424-0000-0000-C000-000000000046}");
    }
    if (Node* link = EnsureKey(st, sw, L"Wow6432Node\\Classes")) {
        link->link = true;
        const std::wstring target = L"\\Registry\\Machine\\Software\\Classes\\Wow6432Node";   // REG_LINK: no terminator
        StoreValue(st, link, L"SymbolicLinkValue", kRegLink, target.data(), target.size() * sizeof(wchar_t));
    }
}

// The in-process COM servers the Wine builtins provide and a program reaches through CoCreateInstance: WMI's locator
// (D18: Win32_Processor.Name is wbemprox reading the SMBIOS table). Windows registers them in
// SOFTWARE\Classes\CLSID\{...}\InprocServer32; a real hive's entry is never overridden.
void ApplyComClasses(State& st, Node* sw) {
    struct Class { const wchar_t* clsid; const wchar_t* name; };
    const Class kClasses[] = {
        {L"{4590F811-1D3A-11D0-891F-00AA004B2E24}", L"WBEM Locator"},
        {L"{674B6698-EE92-11D0-AD71-00C04FD8FDFF}", L"WBEM Call Context"},
        {L"{CB8555CC-9128-11D1-AD9B-00C04FD8FDFF}", L"WBEM Administrative Locator"},
    };
    for (const Class& c : kClasses) {
        const std::wstring key = std::wstring(L"Classes\\CLSID\\") + c.clsid;
        Val v;
        Node* server = EnsureKey(st, sw, key + L"\\InprocServer32");
        if (server && FindValue(server, L"", &v)) continue;
        SetStr(st, sw, key, L"", c.name);
        SetStr(st, sw, key + L"\\InprocServer32", L"", L"wbemprox.dll");
        SetStr(st, sw, key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    }

    // What shell32's DllRegisterServer writes (widl's registration script for shell32_classes.idl): CLSID\{..} with the
    // helpstring, InprocServer32 = shell32.dll with its ThreadingModel, and the ProgID pair where the idl declares one.
    // Rows are generated by tools/gen_shell32_classes.py. A real hive's entry is never overridden.
    struct ShellClass { const wchar_t* clsid; const wchar_t* name; const wchar_t* model; const wchar_t* progid; const wchar_t* vi_progid; };
    const ShellClass kShell[] = {
#include "shell32_classes.inc"
    };
    for (const ShellClass& c : kShell) {
        const std::wstring key = std::wstring(L"Classes\\CLSID\\") + c.clsid;
        Val v;
        Node* server = EnsureKey(st, sw, key + L"\\InprocServer32");
        if (server && FindValue(server, L"", &v)) continue;
        SetStr(st, sw, key, L"", c.name);
        SetStr(st, sw, key + L"\\InprocServer32", L"", L"shell32.dll");
        if (*c.model) SetStr(st, sw, key + L"\\InprocServer32", L"ThreadingModel", c.model);
        if (*c.progid) {
            SetStr(st, sw, key + L"\\ProgID", L"", c.progid);
            SetStr(st, sw, std::wstring(L"Classes\\") + c.progid, L"", c.name);
            SetStr(st, sw, std::wstring(L"Classes\\") + c.progid + L"\\CLSID", L"", c.clsid);
        }
        if (*c.vi_progid) {
            SetStr(st, sw, key + L"\\VersionIndependentProgID", L"", c.vi_progid);
            SetStr(st, sw, std::wstring(L"Classes\\") + c.vi_progid, L"", c.name);
            SetStr(st, sw, std::wstring(L"Classes\\") + c.vi_progid + L"\\CLSID", L"", c.clsid);
            if (*c.progid) SetStr(st, sw, std::wstring(L"Classes\\") + c.vi_progid + L"\\CurVer", L"", c.progid);
        }
    }
}

void ApplySystemFidelity(State& st, Node* sys, bool hive_missing) {
    const Fidelity& f = st.fid;
    if (hive_missing) {
        SetDword(st, sys, L"Select", L"Current", 1);
        SetDword(st, sys, L"Select", L"Default", 1);
        SetDword(st, sys, L"Select", L"Failed", 0);
        SetDword(st, sys, L"Select", L"LastKnownGood", 1);
        SetStr(st, sys, L"ControlSet001\\Control\\ProductOptions", L"ProductType", L"WinNT");
        SetDword(st, sys, L"ControlSet001\\Control\\Windows", L"CSDVersion", 0);
        // The system environment every Windows install has (programs such as Sysinternals PsInfo read it).
        const wchar_t* env = L"ControlSet001\\Control\\Session Manager\\Environment";
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        SetStr(st, sys, env, L"OS", L"Windows_NT");
        SetStr(st, sys, env, L"PROCESSOR_ARCHITECTURE", L"AMD64");
        SetStr(st, sys, env, L"NUMBER_OF_PROCESSORS", std::to_wstring(si.dwNumberOfProcessors));
        SetStr(st, sys, env, L"PATHEXT", L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
        SetStr(st, sys, env, L"ComSpec", L"%SystemRoot%\\system32\\cmd.exe", 2);
        SetStr(st, sys, env, L"windir", L"%SystemRoot%", 2);
        SetStr(st, sys, env, L"TEMP", L"%SystemRoot%\\TEMP", 2);
        SetStr(st, sys, env, L"TMP", L"%SystemRoot%\\TEMP", 2);
    }
    std::wstring cn = Widen(f.computer_name);
    SetStr(st, sys, L"ControlSet001\\Control\\ComputerName\\ComputerName", L"ComputerName", cn);
    SetStr(st, sys, L"ControlSet001\\Control\\ComputerName\\ActiveComputerName", L"ComputerName", cn);
    SetStr(st, sys, L"ControlSet001\\Services\\Tcpip\\Parameters", L"Hostname", cn);
    SetStr(st, sys, L"ControlSet001\\Services\\Tcpip\\Parameters", L"NV Hostname", cn);
}

// The audio stack (core/dispatch/unix_mmdevapi.cpp). Wine registers its builtin COM classes with DllRegisterServer
// into the prefix at creation; this machine has no prefix, so what mmdevapi's DllRegisterServer would write
// (dlls/mmdevapi/mmdevapi_classes.idl: CLSID_MMDeviceEnumerator, ThreadingModel Both) is written here, and the
// driver choice Wine keeps under HKCU\Software\Wine\Drivers selects the layer's own unix library ("winewasapi").
// A real hive's own registration of the class is left alone.
void ApplyAudioRegistration(State& st, Node* sw, Node* user) {
    const std::wstring clsid = L"Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}";
    Val v;
    Node* srv = EnsureKey(st, sw, clsid + L"\\InprocServer32");
    if (srv && !FindValue(srv, L"", &v)) {
        SetStr(st, sw, clsid, L"", L"MMDeviceEnumerator class");
        SetStr(st, sw, clsid + L"\\InprocServer32", L"", L"mmdevapi.dll");
        SetStr(st, sw, clsid + L"\\InprocServer32", L"ThreadingModel", L"Both");
    }
    if (!user) return;
    Node* drv = EnsureKey(st, user, L"Software\\Wine\\Drivers");
    if (drv && !FindValue(drv, L"Audio", &v)) SetStr(st, user, L"Software\\Wine\\Drivers", L"Audio", L"wasapi");
}

// The per-user and common shell folders a Windows 10 install has, as real directories under the machine root and as the
// registry values Explorer's profile setup writes (HKCU ...\Explorer\User Shell Folders as REG_EXPAND_SZ, ...\Shell
// Folders as the expanded paths; HKLM carries the Common* set). shell32's SHGetFolderPath and the known-folder code read
// them; a value a real hive already has is kept.
struct ShellFolder { const wchar_t* name; const wchar_t* rel; };   // rel is under %USERPROFILE%, or %APPDATA% (A) / %LOCALAPPDATA% (L)
const ShellFolder kUserFolders[] = {
    {L"Desktop", L"Desktop"}, {L"Personal", L"Documents"}, {L"My Pictures", L"Pictures"}, {L"My Music", L"Music"},
    {L"My Video", L"Videos"}, {L"{374DE290-123F-4565-9164-39C4925E467B}", L"Downloads"},
    {L"AppData", L"AppData\\Roaming"}, {L"Local AppData", L"AppData\\Local"},
    {L"Cache", L"AppData\\Local\\Microsoft\\Windows\\INetCache"}, {L"Cookies", L"AppData\\Local\\Microsoft\\Windows\\INetCookies"},
    {L"History", L"AppData\\Local\\Microsoft\\Windows\\History"}, {L"Favorites", L"Favorites"},
    {L"NetHood", L"AppData\\Roaming\\Microsoft\\Windows\\Network Shortcuts"},
    {L"PrintHood", L"AppData\\Roaming\\Microsoft\\Windows\\Printer Shortcuts"},
    {L"Recent", L"AppData\\Roaming\\Microsoft\\Windows\\Recent"}, {L"SendTo", L"AppData\\Roaming\\Microsoft\\Windows\\SendTo"},
    {L"Start Menu", L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu"},
    {L"Programs", L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs"},
    {L"Startup", L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"},
    {L"Templates", L"AppData\\Roaming\\Microsoft\\Windows\\Templates"},
    {L"{00BCFC5A-ED94-4e48-96A1-3F6217F21990}", L"AppData\\Local\\Microsoft\\Windows\\RoamingTiles"},
    {L"{A520A1A4-1780-4FF6-BD18-167343C5AF16}", L"AppData\\LocalLow"},
    {L"{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}", L"Saved Games"}, {L"{56784854-C6CB-462B-8169-88E350ACB882}", L"Contacts"},
    {L"{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}", L"Links"}, {L"{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}", L"Searches"},
};
const ShellFolder kCommonFolders[] = {   // rel is under C:\ProgramData unless it starts with '\' (then under C:\Users\Public)
    {L"Common AppData", L""}, {L"Common Templates", L"Microsoft\\Windows\\Templates"},
    {L"Common Start Menu", L"Microsoft\\Windows\\Start Menu"},
    {L"Common Programs", L"Microsoft\\Windows\\Start Menu\\Programs"},
    {L"Common Startup", L"Microsoft\\Windows\\Start Menu\\Programs\\StartUp"},
    {L"Common Administrative Tools", L"Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools"},
    {L"Common Desktop", L"\\Desktop"}, {L"Common Documents", L"\\Documents"},
    {L"CommonPictures", L"\\Pictures"}, {L"CommonMusic", L"\\Music"}, {L"CommonVideo", L"\\Videos"},
    {L"{3D644C9B-1FB8-4f30-9B45-F670235F79C0}", L"\\Downloads"},
};

void MakeDirs(const std::wstring& path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::size_t cut = path.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2) MakeDirs(path.substr(0, cut));
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        TRACE(registry, "cannot create %ls (error %lu)", path.c_str(), GetLastError());
}

void SetMissing(State& st, Node* base, const std::wstring& key, const wchar_t* name, const std::wstring& v, ULONG type) {
    Val cur;
    Node* k = EnsureKey(st, base, key);
    if (k && FindValue(k, name, &cur)) return;
    SetStr(st, base, key, name, v, type);
}

void ApplyShellFolders(State& st, Node* sw, Node* user, const std::wstring& user_name) {
    const std::wstring profile = L"C:\\Users\\" + user_name, pub = L"C:\\Users\\Public", pdata = L"C:\\ProgramData";
    const std::wstring host_profile = st.root_dir + L"\\Users\\" + user_name;
    const wchar_t* kExplorer = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
    for (const ShellFolder& f : kUserFolders) {
        MakeDirs(host_profile + L"\\" + f.rel);
        if (!user) continue;
        const std::wstring ex = kExplorer;
        SetMissing(st, user, ex + L"\\User Shell Folders", f.name, std::wstring(L"%USERPROFILE%\\") + f.rel, 2);
        SetMissing(st, user, ex + L"\\Shell Folders", f.name, profile + L"\\" + f.rel, 1);
    }
    for (const ShellFolder& f : kCommonFolders) {
        const bool in_public = f.rel[0] == L'\\';
        const std::wstring rel = f.rel;
        const std::wstring tail = rel.empty() || in_public ? rel : L"\\" + rel;   // "" or "\\x\\y"
        MakeDirs(st.root_dir + std::wstring(in_public ? L"\\Users\\Public" : L"\\ProgramData") + tail);
        const std::wstring ex = kExplorer;
        SetMissing(st, sw, ex + L"\\User Shell Folders", f.name, std::wstring(in_public ? L"%PUBLIC%" : L"%ProgramData%") + tail, 2);
        SetMissing(st, sw, ex + L"\\Shell Folders", f.name, (in_public ? pub : pdata) + tail, 1);
    }
}

Node* Mount(State& st, Node* parent, const std::wstring& name, const std::wstring& file, bool* missing = nullptr) {
    Node* n = NewNode(st, name, parent);
    n->is_mount = true;
    parent->kids[Upcase(name)] = n;
    std::shared_ptr<regf::Hive> h;
    if (!file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        h = std::make_shared<regf::Hive>();
        if (!h->Open(file)) {
            ERR(registry, "cannot read hive %ls (not a valid REGF file); using an empty hive", file.c_str());
            h.reset();
        } else {
            TRACE(registry, "mounted %ls at %ls", file.c_str(), PathOf(n).c_str());
        }
    }
    if (h) { n->hive = h; n->nk = h->Root(); }
    else TRACE(registry, "no hive for %ls (%ls): empty overlay", PathOf(n).c_str(), file.c_str());
    if (missing) *missing = !h;
    return n;
}

std::wstring FindUserDir(const std::wstring& root, const std::string& preferred) {
    std::wstring pref = Widen(preferred);
    if (!pref.empty() && GetFileAttributesW((root + L"\\Users\\" + pref + L"\\NTUSER.DAT").c_str()) != INVALID_FILE_ATTRIBUTES)
        return pref;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\Users\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (GetFileAttributesW((root + L"\\Users\\" + fd.cFileName + L"\\NTUSER.DAT").c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::wstring r = fd.cFileName;
                FindClose(h);
                return r;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return pref;
}

bool DefaultResolve(const std::wstring& root, const std::wstring& nt_path, std::wstring* host) {
    std::wstring s = nt_path;
    for (const wchar_t* pre : {L"\\??\\", L"\\DosDevices\\"}) {
        std::size_t n = wcslen(pre);
        if (s.size() >= n && regf::EqualNoCase(std::wstring_view(s).substr(0, n), pre)) { s.erase(0, n); break; }
    }
    if (s.size() >= 2 && s[1] == L':') {
        if (regf::UpcaseChar(s[0]) == L'C') *host = root + s.substr(2);
        else *host = s;
        return true;
    }
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') { *host = s; return true; }
    return false;
}

}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void SetHostPathResolver(HostPathResolver resolver) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_resolver = std::move(resolver);
}

void SetHostNamespace(OpenDirectoryFn open_directory, CloseObjectFn close) {
    g_ns_open.store(open_directory);
    g_ns_close.store(close);
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_st) {
        for (auto& w : g_st->watches) CompleteWatch(*w, kStatusNotifyCleanup);
        g_cv.notify_all();
    }
    g_st.reset();
}

NTSTATUS Initialize(const std::wstring& machine_root) {
    Shutdown();
    std::lock_guard<std::mutex> lk(g_mu);
    auto stp = std::make_unique<State>();
    State& st = *stp;
    st.root_dir = machine_root;
    while (!st.root_dir.empty() && (st.root_dir.back() == L'\\' || st.root_dir.back() == L'/')) st.root_dir.pop_back();
    st.init_time = NowFileTime();
    LoadFidelity(st);

    st.root = NewNode(st, L"REGISTRY", nullptr);
    st.root->container = true;
    st.machine = NewNode(st, L"MACHINE", st.root);
    st.machine->container = true;
    st.root->kids[L"MACHINE"] = st.machine;
    st.user = NewNode(st, L"USER", st.root);
    st.user->container = true;
    st.root->kids[L"USER"] = st.user;

    const std::wstring cfg = st.root_dir + L"\\Windows\\System32\\config\\";
    bool sys_missing = false, sw_missing = false;
    st.system = Mount(st, st.machine, L"SYSTEM", cfg + L"SYSTEM", &sys_missing);
    Node* sw = Mount(st, st.machine, L"SOFTWARE", cfg + L"SOFTWARE", &sw_missing);
    Mount(st, st.machine, L"SAM", cfg + L"SAM");
    Mount(st, st.machine, L"SECURITY", cfg + L"SECURITY");
    Node* hw = Mount(st, st.machine, L"HARDWARE", L"");
    hw->is_volatile = true;
    Mount(st, st.user, L".DEFAULT", cfg + L"DEFAULT");

    std::wstring user_dir = FindUserDir(st.root_dir, st.fid.user_name);
    std::wstring sid = Widen(st.fid.user_sid);
    std::wstring ntuser = user_dir.empty() ? L"" : st.root_dir + L"\\Users\\" + user_dir + L"\\NTUSER.DAT";
    std::wstring usrclass = user_dir.empty() ? L"" : st.root_dir + L"\\Users\\" + user_dir + L"\\AppData\\Local\\Microsoft\\Windows\\UsrClass.dat";
    Node* user_hive = Mount(st, st.user, sid, ntuser);
    Mount(st, st.user, sid + L"_Classes", usrclass);

    // Fidelity: fill what a missing hive lacks; override only what machine.json states.
    if (sw_missing) ApplySoftwareSkeleton(st, sw);
    if (sw_missing || st.fid.has_windows) ApplySoftwareFidelity(st, sw);
    ApplyComClasses(st, sw);
    if (sys_missing || st.fid.has_identity) ApplySystemFidelity(st, st.system, sys_missing);
    ApplyAudioRegistration(st, sw, user_hive);
    ApplyShellFolders(st, sw, user_hive, user_dir.empty() ? Widen(st.fid.user_name) : user_dir);
    if (sys_missing) {
        // The overlay-only SYSTEM needs the link target to exist for CurrentControlSet.
        EnsureKey(st, st.system, L"ControlSet001\\Control");
    }
    SetStr(st, hw, L"DESCRIPTION\\System\\CentralProcessor\\0", L"VendorIdentifier", Widen(st.fid.cpu_vendor));
    {
        // D18: the one CPU identity (brand, family/model/stepping) is the machine identity's; the clock is the host's.
        const auto& id = config::Current().machine_identity;
        const wchar_t* cpu0 = L"DESCRIPTION\\System\\CentralProcessor\\0";
        SetStr(st, hw, cpu0, L"ProcessorNameString", id.cpu_brand);
        SetStr(st, hw, cpu0, L"Identifier",
               L"AMD64 Family " + std::to_wstring(id.cpu_family) + L" Model " + std::to_wstring(id.cpu_model) +
                   L" Stepping " + std::to_wstring(id.cpu_stepping));
        if (const ULONG mhz = HostBaseMhz()) SetDword(st, hw, cpu0, L"~MHz", mhz);
    }
    EnsureKey(st, hw, L"DEVICEMAP");
    EnsureKey(st, hw, L"RESOURCEMAP");
    // Keys created above are baseline, not user changes: nothing to record.

    g_st = std::move(stp);
    TRACE(registry, "initialised, machine_root=%ls SYSTEM=%s SOFTWARE=%s user=%ls", machine_root.c_str(),
          sys_missing ? "(empty)" : "hive", sw_missing ? "(empty)" : "hive", sid.c_str());
    return nt::kSuccess;
}

// ============================================================================
// Handle plumbing
// ============================================================================

HANDLE ToHandle(KeyId id) { return EncodeHandle(id); }
KeyId FromHandle(HANDLE h) { return DecodeHandle(h); }

bool IsKeyHandle(HANDLE h) {
    KeyId id = DecodeHandle(h);
    if (!id) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_st && g_st->handles.count(id) != 0;
}

NTSTATUS Close(HANDLE KeyHandle) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    KeyId id = DecodeHandle(KeyHandle);
    auto it = id ? st.handles.find(id) : st.handles.end();
    if (it == st.handles.end()) return nt::kInvalidHandle;
    for (auto& w : st.watches) if (w->handle == id) CompleteWatch(*w, kStatusNotifyCleanup);
    st.watches.remove_if([id](const std::shared_ptr<Watch>& w) { return w->handle == id; });
    st.handles.erase(it);
    g_cv.notify_all();
    return nt::kSuccess;
}

// ============================================================================
// Reg_Nt*
// ============================================================================

NTSTATUS Reg_NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes) {
    return OpenCommon(KeyHandle, DesiredAccess, ObjectAttributes, 0);
}

NTSTATUS Reg_NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions) {
    return OpenCommon(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions);
}

// There is no transaction manager, so the transaction is not applied: the
// open is the plain NtOpenKey/NtOpenKeyEx one. The trace says when that happened.
NTSTATUS Reg_NtOpenKeyTransacted(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                 HANDLE Transaction) {
    TRACE(registry, "NtOpenKeyTransacted: transaction %p ignored", Transaction);
    return OpenCommon(KeyHandle, DesiredAccess, ObjectAttributes, 0);
}

NTSTATUS Reg_NtOpenKeyTransactedEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                   ULONG OpenOptions, HANDLE Transaction) {
    TRACE(registry, "NtOpenKeyTransactedEx: transaction %p ignored", Transaction);
    return OpenCommon(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions);
}

NTSTATUS Reg_NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                         ULONG /*TitleIndex*/, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition) {
    // Windows probes the outputs, then captures the attributes and the class (optional); a failed create leaves
    // the handle NULL.
    if (NTSTATUS s = probe::ProbeWrite(KeyHandle, sizeof(HANDLE))) return s;
    if (Disposition)
        if (NTSTATUS s = probe::ProbeWrite(Disposition, sizeof(ULONG), sizeof(ULONG))) return s;
    probe::ObjectAttributes cap;
    if (NTSTATUS s = CaptureOA(ObjectAttributes, &cap)) return s;
    probe::String class_in;
    if (Class)
        if (NTSTATUS s = probe::CaptureString(Class, &class_in)) return s;
    if (NTSTATUS s = probe::Store<HANDLE>(KeyHandle, nullptr)) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* start; std::wstring_view path; ACCESS_MASK ra;
    NTSTATUS s = ParseOA(st, &cap.oa, &start, &path, &ra);
    if (s < 0) return s;
    std::wstring cls(class_in.view());
    ResolveOpts ro;
    ro.create = true;
    ro.can_create = true;   // the parent's ACL decides on Windows, not the access of the handle it is reached through
    ro.open_link = (CreateOptions & kRegOptionCreateLink) || (cap.oa.Attributes & kObjOpenLink);
    ro.create_options = CreateOptions;
    ro.cls = &cls;
    Node* n = nullptr;
    bool created = false;
    s = Resolve(st, start, path, ro, &n, &created);
    if (s < 0) return s;
    if (!created && (CreateOptions & kRegOptionCreateLink)) return nt::kObjectNameCollision;   // the key exists
    const HANDLE key = EncodeHandle(NewHandle(st, n, MapAccess(DesiredAccess)));
    NTSTATUS ws = probe::Store<HANDLE>(KeyHandle, key);
    if (ws == 0 && Disposition)
        ws = probe::Store<ULONG>(Disposition, created ? kRegCreatedNewKey : kRegOpenedExistingKey);
    if (ws) {
        st.handles.erase(DecodeHandle(key));   // the caller's output vanished: do not leak the handle
        return ws;
    }
    TRACE(registry, "create %ls -> %p (%s)", PathOf(n).c_str(), key, created ? "new" : "existing");
    return nt::kSuccess;
}

NTSTATUS Reg_NtCreateKeyTransacted(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, HANDLE, PULONG) {
    FIXME(registry, "transacted registry is not supported");
    return nt::kNotImplemented;
}

NTSTATUS Reg_NtDeleteKey(HANDLE KeyHandle) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kDelete, &n);
    if (s == nt::kKeyDeleted) return nt::kSuccess;   // deleting a key that is already gone is not an error
    if (s < 0) return s;
    if (n->container || n->is_mount || !n->parent) return nt::kCannotDelete;
    if (!Children(st, n).empty()) return nt::kCannotDelete;
    TRACE(registry, "delete key %ls", PathOf(n).c_str());
    n->parent->kids[Upcase(n->name)] = n;   // keep the tombstone in the parent's map
    MarkDeleted(st, n);
    return nt::kSuccess;
}

NTSTATUS Reg_NtDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName) {
    probe::String value_name;
    if (!ValueName) return probe::kAccessViolation;
    if (NTSTATUS s = probe::CaptureString(ValueName, &value_name)) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeySetValue, &n);
    if (s < 0) return s;
    std::wstring_view name = value_name.view();
    Val v;
    if (!FindValue(n, name, &v)) return nt::kObjectNameNotFound;
    OvValue& o = n->ov[Upcase(name)];
    o.name = v.name; o.deleted = true; o.data.clear(); o.seq = ++n->ov_seq;
    n->mtime = NowFileTime();
    n->vals_valid = false;
    NotifyChange(st, n, kNotifyLastSet);
    return nt::kSuccess;
}

NTSTATUS Reg_NtEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass, PVOID KeyInformation,
                            ULONG Length, PULONG ResultLength) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyEnumerateSubKeys, &n);
    if (s < 0) return s;
    const auto& kids = Children(st, n);
    if (Index >= kids.size()) return nt::kNoMoreEntries;
    ChildEnt e = kids[Index];
    Node* c = EnsureNode(st, n, e);
    if (!c) return nt::kNoMoreEntries;
    std::vector<std::uint8_t> full;
    std::size_t fixed = 0;
    s = BuildKeyInfo(st, KeyInformationClass, c, e.name, true, &full, &fixed);
    if (s < 0) return s;
    return Emit(full, fixed, KeyInformation, Length, ResultLength);
}

NTSTATUS Reg_NtEnumerateValueKey(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInformationClass, PVOID KeyValueInformation,
                                 ULONG Length, PULONG ResultLength) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyQueryValue, &n);
    if (s < 0) return s;
    const auto& vals = Values(n);
    if (Index >= vals.size()) return nt::kNoMoreEntries;
    std::vector<std::uint8_t> full;
    std::size_t fixed = 0;
    s = BuildValueInfo(KeyValueInformationClass, vals[Index], &full, &fixed);
    if (s < 0) return s;
    return Emit(full, fixed, KeyValueInformation, Length, ResultLength);
}

NTSTATUS Reg_NtFlushKey(HANDLE KeyHandle) {
    NTSTATUS s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_st) return nt::kUnsuccessful;
        Node* n;
        s = GetHandle(*g_st, KeyHandle, 0, &n);
    }
    if (s >= 0) PersistOverlay();   // the overlay file is the only thing to flush; failing to write it is not the guest's error
    return s;
}

NTSTATUS Reg_NtNotifyChangeKey(HANDLE KeyHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID /*ApcContext*/,
                               PIO_STATUS_BLOCK IoStatusBlock, ULONG CompletionFilter, BOOLEAN WatchTree,
                               PVOID /*Buffer*/, ULONG /*BufferSize*/, BOOLEAN Asynchronous) {
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyNotify, &n);
    if (s < 0) return s;
    // The block is required (the I/O manager probes it), and it is written later, when the key changes.
    if (NTSTATUS ps = probe::ProbeWrite(IoStatusBlock, sizeof(IO_STATUS_BLOCK), sizeof(ULONG))) return ps;
    if (ApcRoutine) FIXME(registry, "APC completion of registry notifications is not supported; use an event");
    std::shared_ptr<Watch> w;
    const KeyId hid = DecodeHandle(KeyHandle);
    for (auto& x : st.watches) if (x->handle == hid) { w = x; break; }
    if (!w) {
        w = std::make_shared<Watch>();
        w->node = n; w->tree = WatchTree != 0; w->filter = CompletionFilter; w->handle = hid;
        st.watches.push_back(w);
    }
    auto p = std::make_shared<Pending>();
    p->event = Event; p->iosb = IoStatusBlock;
    w->pending.push_back(p);
    if (Event) ResetEvent(Event);   // the request is outstanding: the event is not signalled until the key changes
    if (Asynchronous) return nt::kPending;
    g_cv.wait(lk, [&] { return p->done; });
    return nt::kSuccess;
}

NTSTATUS Reg_NtNotifyChangeMultipleKeys(HANDLE MasterKeyHandle, ULONG Count, POBJECT_ATTRIBUTES, HANDLE Event,
                                        PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                                        ULONG CompletionFilter, BOOLEAN WatchTree, PVOID Buffer, ULONG BufferSize,
                                        BOOLEAN Asynchronous) {
    if (Count != 0) {
        FIXME(registry, "NtNotifyChangeMultipleKeys with subordinate keys is not supported");
        return nt::kNotImplemented;
    }
    return Reg_NtNotifyChangeKey(MasterKeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, CompletionFilter,
                                 WatchTree, Buffer, BufferSize, Asynchronous);
}

NTSTATUS Reg_NtQueryKey(HANDLE KeyHandle, ULONG KeyInformationClass, PVOID KeyInformation, ULONG Length,
                        PULONG ResultLength) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, 0, &n);
    if (s < 0) return s;
    std::vector<std::uint8_t> full;
    std::size_t fixed = 0;
    std::wstring name_copy = KeyInformationClass == kKeyName ? PathOf(n) : n->name;
    s = BuildKeyInfo(st, KeyInformationClass, n, name_copy, false, &full, &fixed);
    if (s < 0) return s;
    return Emit(full, fixed, KeyInformation, Length, ResultLength);
}

NTSTATUS Reg_NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass,
                             PVOID KeyValueInformation, ULONG Length, PULONG ResultLength) {
    probe::String value_name;
    if (!ValueName) return probe::kAccessViolation;
    if (NTSTATUS s = probe::CaptureString(ValueName, &value_name)) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyQueryValue, &n);
    if (s < 0) return s;
    Val v;
    if (!FindValue(n, value_name.view(), &v)) {
        TRACE(registry, "query %ls\\%ls: value not found", PathOf(n).c_str(), std::wstring(value_name.view()).c_str());
        return nt::kObjectNameNotFound;
    }
    std::vector<std::uint8_t> full;
    std::size_t fixed = 0;
    s = BuildValueInfo(KeyValueInformationClass, v, &full, &fixed);
    if (s < 0) return s;
    return Emit(full, fixed, KeyValueInformation, Length, ResultLength);
}

NTSTATUS Reg_NtQueryMultipleValueKey(HANDLE KeyHandle, ValueEntry* ValueEntries, ULONG EntryCount, PVOID ValueBuffer,
                                     PULONG BufferLength, PULONG RequiredBufferLength) {
    // Capture the entries (and every name in them) and the buffer length once; probe the outputs before any is written.
    ULONG buffer_length = 0;
    if (NTSTATUS s = probe::Load<ULONG>(BufferLength, &buffer_length)) return s;
    if (NTSTATUS s = probe::ProbeWrite(BufferLength, sizeof(ULONG), sizeof(ULONG))) return s;
    if (RequiredBufferLength)
        if (NTSTATUS s = probe::ProbeWrite(RequiredBufferLength, sizeof(ULONG), sizeof(ULONG))) return s;
    if (NTSTATUS s = probe::ProbeWrite(ValueEntries, std::size_t{EntryCount} * sizeof(ValueEntry))) return s;
    if (NTSTATUS s = probe::ProbeWrite(ValueBuffer, buffer_length)) return s;
    if (!ValueEntries) return nt::kInvalidParameter;
    std::vector<ValueEntry> entries(EntryCount);
    std::deque<probe::String> names;
    if (EntryCount)
        if (NTSTATUS s = probe::CopyFromUser(entries.data(), ValueEntries, std::size_t{EntryCount} * sizeof(ValueEntry))) return s;
    for (ULONG i = 0; i < EntryCount; ++i) {
        if (!entries[i].ValueName) return probe::kAccessViolation;
        if (NTSTATUS s = probe::CaptureString(entries[i].ValueName, &names.emplace_back())) return s;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyQueryValue, &n);
    if (s < 0) return s;
    std::vector<Val> found(EntryCount);
    for (ULONG i = 0; i < EntryCount; ++i)
        if (!FindValue(n, names[i].view(), &found[i])) return nt::kObjectNameNotFound;
    std::size_t total = 0;
    std::vector<std::size_t> offs(EntryCount);
    for (ULONG i = 0; i < EntryCount; ++i) {
        total = (total + 7) & ~static_cast<std::size_t>(7);
        offs[i] = total;
        total += found[i].data.size();
    }
    for (ULONG i = 0; i < EntryCount; ++i) {
        entries[i].Type = found[i].type;
        entries[i].DataLength = static_cast<ULONG>(found[i].data.size());
        entries[i].DataOffset = static_cast<ULONG>(offs[i]);
    }
    if (EntryCount)
        if (NTSTATUS ws = probe::CopyToUser(ValueEntries, entries.data(), std::size_t{EntryCount} * sizeof(ValueEntry))) return ws;
    if (RequiredBufferLength)
        if (NTSTATUS ws = probe::Store<ULONG>(RequiredBufferLength, static_cast<ULONG>(total))) return ws;
    if (total > buffer_length) return nt::kBufferOverflow;
    for (ULONG i = 0; i < EntryCount; ++i)
        if (!found[i].data.empty())
            if (NTSTATUS ws = probe::CopyToUser(static_cast<std::uint8_t*>(ValueBuffer) + offs[i], found[i].data.data(),
                                                found[i].data.size()))
                return ws;
    return probe::Store<ULONG>(BufferLength, static_cast<ULONG>(total));
}

NTSTATUS Reg_NtRenameKey(HANDLE KeyHandle, PUNICODE_STRING NewName) {
    probe::String new_name;
    if (!NewName) return probe::kAccessViolation;
    if (NTSTATUS s = probe::CaptureString(NewName, &new_name)) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    // The name is checked before the handle: an empty or path-like name is a parameter error whatever the handle is.
    std::wstring_view nn = new_name.view();
    if (nn.empty() || nn.size() > kMaxKeyNameChars || nn.find(L'\\') != std::wstring_view::npos) return nt::kInvalidParameter;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyWrite & ~0x20000u, &n);   // KEY_SET_VALUE | KEY_CREATE_SUB_KEY
    if (s < 0) return s;
    if (n->container || n->is_mount || !n->parent) return nt::kAccessDenied;
    Node* parent = n->parent;
    if (FindChild(st, parent, nn)) return nt::kCannotDelete;   // the name is taken, by this key itself or a sibling
    regf::Key snap;
    if (!Snapshot(st, n, &snap, 0)) return nt::kUnsuccessful;
    MarkDeleted(st, n);
    Node* nnode = CreateChild(st, parent, nn);
    Materialize(st, nnode, snap, 0);
    nnode->is_volatile = n->is_volatile;
    KeyId id = DecodeHandle(KeyHandle);
    st.handles[id].node = nnode;   // this handle now designates the renamed key
    return nt::kSuccess;
}

NTSTATUS Reg_NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG /*TitleIndex*/, ULONG Type, PVOID Data,
                           ULONG DataSize) {
    probe::String value_name;
    if (!ValueName) return probe::kAccessViolation;
    if (NTSTATUS s = probe::CaptureString(ValueName, &value_name)) return s;
    // The data is probed before it is sized into a buffer (a wild DataSize must not allocate), then copied once.
    if (NTSTATUS s = probe::ProbeRead(Data, DataSize)) return s;
    std::vector<std::uint8_t> data(DataSize);
    if (DataSize)
        if (NTSTATUS s = probe::CopyFromUser(data.data(), Data, DataSize)) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeySetValue, &n);
    if (s < 0) return s;
    std::wstring_view name = value_name.view();
    if (name.size() > kMaxValueNameChars) return nt::kInvalidParameter;
    if (n->container) return nt::kAccessDenied;
    // A symbolic link key holds one value, SymbolicLinkValue, and it is a REG_LINK.
    if (IsLink(n) && (Type != kRegLink || !regf::EqualNoCase(name, L"SymbolicLinkValue"))) return nt::kAccessDenied;
    StoreValue(st, n, name, Type, data.data(), DataSize);
    TRACE(registry, "set %ls\\%ls type=%lu size=%lu", PathOf(n).c_str(), std::wstring(name).c_str(), Type, DataSize);
    return nt::kSuccess;
}

NTSTATUS Reg_NtSetInformationKey(HANDLE KeyHandle, int KeySetInformationClass, PVOID KeySetInformation, ULONG Length) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeySetValue, &n);
    if (s < 0) return s;
    switch (KeySetInformationClass) {
        case 0:   // KeyWriteTimeInformation
            if (Length < 8) return nt::kInvalidParameter;
            return probe::CopyFromUser(&n->mtime, KeySetInformation, 8);
        case 1: case 2: case 3: case 4: case 5:   // Wow64Flags, ControlFlags, SetVirtualization, Debug, HandleTags
            TRACE(registry, "SetInformationKey class %d ignored", KeySetInformationClass);
            return nt::kSuccess;
        default:
            return nt::kInvalidInfoClass;
    }
}

NTSTATUS Reg_NtSaveKey(HANDLE KeyHandle, HANDLE FileHandle) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, 0, &n);
    if (s < 0) return s;
    regf::Key snap;
    if (!Snapshot(st, n, &snap, 0)) return nt::kUnsuccessful;
    std::vector<std::uint8_t> bytes;
    if (!regf::WriteHive(snap, n->name, &bytes)) return nt::kUnsuccessful;
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD w = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 24));
        if (!WriteFile(FileHandle, bytes.data() + done, want, &w, nullptr) || w == 0) return nt::kUnsuccessful;
        done += w;
    }
    return nt::kSuccess;
}

NTSTATUS Reg_NtRestoreKey(HANDLE KeyHandle, HANDLE FileHandle, ULONG /*Flags*/) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* n;
    NTSTATUS s = GetHandle(st, KeyHandle, kKeyWrite & ~0x20000u, &n);
    if (s < 0) return s;
    if (n->container) return nt::kAccessDenied;
    std::vector<std::uint8_t> bytes;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(FileHandle, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7FFFFFFFLL) return nt::kInvalidParameter;
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t got = 0;
    while (got < bytes.size()) {
        DWORD r = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - got, 1u << 24));
        if (!ReadFile(FileHandle, bytes.data() + got, want, &r, nullptr) || r == 0) break;
        got += r;
    }
    bytes.resize(got);
    regf::Hive h;
    if (!h.OpenMemory(std::move(bytes))) return kStatusRegistryCorrupt;
    regf::Key tree;
    if (!h.ToTree(h.Root(), &tree)) return kStatusRegistryCorrupt;
    ResetKey(st, n);
    n->mtime = NowFileTime();
    Materialize(st, n, tree, 0);
    NotifyChange(st, n, kNotifyName | kNotifyLastSet);
    return nt::kSuccess;
}

NTSTATUS Reg_NtReplaceKey(POBJECT_ATTRIBUTES, HANDLE, POBJECT_ATTRIBUTES) {
    FIXME(registry, "NtReplaceKey is not supported");
    return nt::kNotImplemented;
}

namespace {

NTSTATUS LoadCommon(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile, ULONG flags, ACCESS_MASK access,
                    PHANDLE out_handle) {
    probe::ObjectAttributes target, source;
    if (out_handle)
        if (NTSTATUS ps = probe::ProbeWrite(out_handle, sizeof(HANDLE))) return ps;
    if (NTSTATUS cs = CaptureOA(TargetKey, &target)) return cs;
    if (!SourceFile) return nt::kInvalidParameter;
    if (NTSTATUS cs = CaptureOA(SourceFile, &source)) return cs;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* start; std::wstring_view path; ACCESS_MASK ra;
    NTSTATUS s = ParseOA(st, &target.oa, &start, &path, &ra);
    if (s < 0) return s;
    if (!source.oa.ObjectName) return nt::kInvalidParameter;
    auto comps = Split(path);
    if (comps.empty()) return nt::kInvalidParameter;
    std::wstring_view leaf = comps.back();
    if (leaf.size() > kMaxKeyNameChars) return nt::kInvalidParameter;
    // Parent path: everything but the last component, resolved from the same start.
    std::wstring parent_path;
    for (std::size_t i = 0; i + 1 < comps.size(); ++i) { parent_path += L'\\'; parent_path += comps[i]; }
    Node* parent = nullptr;
    ResolveOpts ro;
    if (!start && parent_path.empty()) return nt::kInvalidParameter;
    s = Resolve(st, start, parent_path, ro, &parent, nullptr);
    if (s < 0) {
        // Application hives (REG_APP_HIVE) live under \REGISTRY\A, created on demand.
        if (!start && (flags & 0x10) && comps.size() == 3 && regf::EqualNoCase(comps[1], L"A")) {
            Node* a = FindChild(st, st.root, L"A");
            if (!a) { a = NewNode(st, L"A", st.root); a->container = true; st.root->kids[L"A"] = a; Invalidate(st.root); }
            parent = a;
        } else {
            return s;
        }
    }
    if (!parent->container) return nt::kAccessDenied;   // hives load only under \Registry\Machine, \Registry\User, \Registry\A
    if (FindChild(st, parent, leaf)) return nt::kObjectNameCollision;

    std::wstring nt_file(source.name.view()), host;
    bool ok = g_resolver ? g_resolver(nt_file, &host) : DefaultResolve(st.root_dir, nt_file, &host);
    if (!ok) return nt::kObjectNameNotFound;
    if (GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES) return nt::kObjectNameNotFound;
    auto hive = std::make_shared<regf::Hive>();
    if (!hive->Open(host)) return kStatusRegistryCorrupt;

    Node* m = NewNode(st, std::wstring(leaf), parent);
    m->is_mount = true; m->loaded = true; m->hive = hive; m->nk = hive->Root();
    parent->kids[Upcase(leaf)] = m;
    Invalidate(parent);
    NotifyChange(st, parent, kNotifyName);
    TRACE(registry, "loaded %ls at %ls", host.c_str(), PathOf(m).c_str());
    if (out_handle) {
        const HANDLE key = EncodeHandle(NewHandle(st, m, MapAccess(access)));
        if (NTSTATUS ws = probe::Store<HANDLE>(out_handle, key)) {
            st.handles.erase(DecodeHandle(key));
            return ws;
        }
    }
    return nt::kSuccess;
}

}  // namespace

NTSTATUS Reg_NtLoadKey(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile) {
    return LoadCommon(TargetKey, SourceFile, 0, 0, nullptr);
}

NTSTATUS Reg_NtLoadKeyEx(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile, ULONG Flags, HANDLE /*TrustClassKey*/,
                         HANDLE /*Event*/, ACCESS_MASK DesiredAccess, PHANDLE RootHandle, PVOID /*Reserved*/) {
    return LoadCommon(TargetKey, SourceFile, Flags, DesiredAccess, RootHandle);
}

// NtLoadKeyEx without trust key, event, access or root handle (Wine's NtLoadKey2 does the same).
NTSTATUS Reg_NtLoadKey2(POBJECT_ATTRIBUTES TargetKey, POBJECT_ATTRIBUTES SourceFile, ULONG Flags) {
    return LoadCommon(TargetKey, SourceFile, Flags, 0, nullptr);
}

NTSTATUS Reg_NtUnloadKey(POBJECT_ATTRIBUTES TargetKey) {
    probe::ObjectAttributes target;
    if (NTSTATUS cs = CaptureOA(TargetKey, &target)) return cs;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* start; std::wstring_view path; ACCESS_MASK ra;
    NTSTATUS s = ParseOA(st, &target.oa, &start, &path, &ra);
    if (s < 0) return s;
    ResolveOpts ro;
    ro.open_link = true;
    Node* n = nullptr;
    s = Resolve(st, start, path, ro, &n, nullptr);
    if (s < 0) return s;
    if (!n->is_mount) return nt::kInvalidParameter;
    if (!n->loaded) return nt::kCannotDelete;   // built-in hives cannot be unloaded
    MarkDeleted(st, n);
    return nt::kSuccess;
}

// NtQueryObject on a key handle: the basic information and the object name. Other classes are not implemented.
NTSTATUS Reg_NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation, ULONG Length,
                           PULONG ReturnLength) {
    constexpr ULONG kObjectBasicInformation = 0, kObjectNameInformation = 1;
    std::vector<std::uint8_t> full;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_st) return nt::kUnsuccessful;
        State& st = *g_st;
        KeyId id = DecodeHandle(Handle);
        auto it = id ? st.handles.find(id) : st.handles.end();
        if (it == st.handles.end()) return nt::kInvalidHandle;
        const Node* n = it->second.node;
        if (ObjectInformationClass == kObjectBasicInformation) {
            // A deleted key still has a handle with an access mask and a creation time.
            full.assign(56, 0);
            Put32(full, 4, it->second.access);
            ULONG handles = 0;
            for (auto& [hid, h] : st.handles) if (h.node == n) ++handles;
            Put32(full, 8, handles);
            Put32(full, 12, handles + 1);
            Put64(full, 48, TimeOf(st, n));
        } else if (ObjectInformationClass == kObjectNameInformation) {
            if (n->deleted) return nt::kKeyDeleted;
            // UNICODE_STRING { Length, MaximumLength, Buffer } then the text; Buffer is filled in below, for the
            // caller's address.
            const std::wstring path = PathOf(n);
            const std::size_t bytes = path.size() * sizeof(wchar_t);
            full.assign(sizeof(UNICODE_STRING) + bytes + sizeof(wchar_t), 0);
            Put32(full, 0, static_cast<std::uint32_t>(bytes | ((bytes + sizeof(wchar_t)) << 16)));
            PutBytes(full, sizeof(UNICODE_STRING), path.data(), bytes);
        } else {
            return nt::kNotImplemented;
        }
    }
    if (ReturnLength)
        if (NTSTATUS ps = probe::Store<ULONG>(ReturnLength, static_cast<ULONG>(full.size()))) return ps;
    if (Length < full.size()) return nt::kInfoLengthMismatch;
    if (ObjectInformationClass == kObjectNameInformation) {
        const std::uint64_t text = reinterpret_cast<std::uintptr_t>(ObjectInformation) + sizeof(UNICODE_STRING);
        Put64(full, 8, text);
    }
    return probe::CopyToUser(ObjectInformation, full.data(), full.size());
}

NTSTATUS Reg_NtQueryOpenSubKeys(POBJECT_ATTRIBUTES TargetKey, PULONG HandleCount) {
    if (NTSTATUS ps = probe::ProbeWrite(HandleCount, sizeof(ULONG), sizeof(ULONG))) return ps;
    probe::ObjectAttributes target;
    if (NTSTATUS cs = CaptureOA(TargetKey, &target)) return cs;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    Node* start; std::wstring_view path; ACCESS_MASK ra;
    NTSTATUS s = ParseOA(st, &target.oa, &start, &path, &ra);
    if (s < 0) return s;
    Node* n = nullptr;
    s = Resolve(st, start, path, ResolveOpts{}, &n, nullptr);
    if (s < 0) return s;
    ULONG count = 0;
    for (auto& [id, h] : st.handles) if (IsAncestorOrSelf(n, h.node)) ++count;
    return probe::Store<ULONG>(HandleCount, count);
}

// ============================================================================
// Convenience API
// ============================================================================

NTSTATUS OpenKey(std::wstring_view nt_path, KeyId* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    if (!out) return nt::kInvalidParameter;
    Node* n = nullptr;
    NTSTATUS s = Resolve(*g_st, nullptr, nt_path, ResolveOpts{}, &n, nullptr);
    if (s < 0) return s;
    *out = NewHandle(*g_st, n, kKeyAll);
    return nt::kSuccess;
}

namespace {
NTSTATUS KeyOfId(State& st, KeyId key, Node** n) { return GetHandle(st, EncodeHandle(key), 0, n); }

NTSTATUS ReadValueAt(std::wstring_view nt_path, std::wstring_view name, Val* v) {
    if (!g_st) return nt::kUnsuccessful;
    Node* n = nullptr;
    NTSTATUS s = Resolve(*g_st, nullptr, nt_path, ResolveOpts{}, &n, nullptr);
    if (s < 0) return s;
    return FindValue(n, name, v) ? nt::kSuccess : nt::kObjectNameNotFound;
}
}  // namespace

NTSTATUS QueryValue(KeyId key, std::wstring_view name, Value* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    Node* n;
    NTSTATUS s = KeyOfId(*g_st, key, &n);
    if (s < 0) return s;
    Val v;
    if (!FindValue(n, name, &v)) return nt::kObjectNameNotFound;
    if (out) { out->name = v.name; out->type = v.type; out->data = std::move(v.data); }
    return nt::kSuccess;
}

NTSTATUS QueryString(std::wstring_view nt_path, std::wstring_view name, std::wstring* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Val v;
    NTSTATUS s = ReadValueAt(nt_path, name, &v);
    if (s < 0) return s;
    if (v.type != 1 && v.type != 2) return nt::kObjectTypeMismatch;   // REG_SZ / REG_EXPAND_SZ
    if (out) {
        out->assign(reinterpret_cast<const wchar_t*>(v.data.data()), v.data.size() / 2);
        while (!out->empty() && out->back() == L'\0') out->pop_back();
    }
    return nt::kSuccess;
}

NTSTATUS QueryDword(std::wstring_view nt_path, std::wstring_view name, std::uint32_t* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Val v;
    NTSTATUS s = ReadValueAt(nt_path, name, &v);
    if (s < 0) return s;
    if (v.type != 4 || v.data.size() < 4) return nt::kObjectTypeMismatch;
    if (out) std::memcpy(out, v.data.data(), 4);
    return nt::kSuccess;
}

NTSTATUS SetValue(KeyId key, const Value& v) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    Node* n;
    NTSTATUS s = KeyOfId(*g_st, key, &n);
    if (s < 0) return s;
    if (n->container) return nt::kAccessDenied;
    StoreValue(*g_st, n, v.name, v.type, v.data.data(), v.data.size());
    return nt::kSuccess;
}

NTSTATUS EnumerateSubkeys(KeyId key, std::vector<std::wstring>* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    Node* n;
    NTSTATUS s = KeyOfId(*g_st, key, &n);
    if (s < 0) return s;
    if (out) { out->clear(); for (auto& e : Children(*g_st, n)) out->push_back(e.name); }
    return nt::kSuccess;
}

NTSTATUS EnumerateValues(KeyId key, std::vector<Value>* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    Node* n;
    NTSTATUS s = KeyOfId(*g_st, key, &n);
    if (s < 0) return s;
    if (out) { out->clear(); for (auto& v : Values(n)) out->push_back(Value{v.name, v.type, v.data}); }
    return nt::kSuccess;
}

void CloseKey(KeyId key) { Close(EncodeHandle(key)); }

// ============================================================================
// Overlay persistence: <machine_root>\registry-overlay.bin
// ============================================================================

namespace {

constexpr char kOverlayMagic[8] = {'W', '1', '0', 'X', 'O', 'V', 'L', '1'};
constexpr std::uint32_t kFlagDeleted = 1, kFlagCreated = 2, kFlagLink = 4, kFlagClass = 8;

struct Out {
    std::vector<std::uint8_t> b;
    void U32(std::uint32_t v) { auto o = b.size(); b.resize(o + 4); std::memcpy(&b[o], &v, 4); }
    void U64(std::uint64_t v) { auto o = b.size(); b.resize(o + 8); std::memcpy(&b[o], &v, 8); }
    void Bytes(const void* p, std::size_t n) { auto o = b.size(); b.resize(o + n); if (n) std::memcpy(&b[o], p, n); }
    void Str(const std::wstring& s) { U32(static_cast<std::uint32_t>(s.size())); Bytes(s.data(), s.size() * 2); }
};

struct In {
    const std::uint8_t* p; std::size_t n; std::size_t i = 0; bool ok = true;
    bool Need(std::size_t k) { if (i + k > n) ok = false; return ok; }
    std::uint32_t U32() { std::uint32_t v = 0; if (Need(4)) { std::memcpy(&v, p + i, 4); i += 4; } return v; }
    std::uint64_t U64() { std::uint64_t v = 0; if (Need(8)) { std::memcpy(&v, p + i, 8); i += 8; } return v; }
    std::wstring Str() {
        std::uint32_t len = U32();
        std::wstring s;
        if (ok && Need(static_cast<std::size_t>(len) * 2)) { s.assign(reinterpret_cast<const wchar_t*>(p + i), len); i += static_cast<std::size_t>(len) * 2; }
        return s;
    }
    std::vector<std::uint8_t> Blob() {
        std::uint32_t len = U32();
        std::vector<std::uint8_t> v;
        if (ok && Need(len)) { v.assign(p + i, p + i + len); i += len; }
        return v;
    }
};

bool Dirty(const Node* n) {
    if (n->container) return false;
    if (n->deleted) return true;
    if (!n->is_mount && !n->shadow) return true;    // created or re-created
    return n->link || n->has_class || !n->ov.empty();
}

std::uint32_t KeyFlags(const Node* n) {
    std::uint32_t fl = 0;
    if (n->deleted) fl |= kFlagDeleted;
    if (!n->is_mount && !n->shadow) fl |= kFlagCreated;
    if (n->link) fl |= kFlagLink;
    if (n->has_class) fl |= kFlagClass;
    return fl;
}

std::vector<const OvValue*> SortedValues(const Node* n) {
    std::vector<const OvValue*> vals;
    for (auto& [up, v] : n->ov) vals.push_back(&v);
    std::sort(vals.begin(), vals.end(), [](const OvValue* a, const OvValue* b) { return a->seq < b->seq; });
    return vals;
}

void CaptureWalk(State& st, const Node* n) {
    if (n->is_volatile) return;
    if (Dirty(n)) {
        BaseKey& b = st.base[Upcase(PathOf(n))];
        b.fl = KeyFlags(n);
        b.cls = n->cls;
        for (const OvValue* v : SortedValues(n)) {
            BaseVal& bv = b.vals[Upcase(v->name)];
            bv.type = v->type;
            bv.deleted = v->deleted;
            bv.data = v->data;
        }
    }
    if (n->deleted) return;
    for (auto& [up, k] : n->kids) CaptureWalk(st, k);
}

// A key (and the values of it) that still is what Initialize made is not written: only differences from st.base.
void SaveWalk(const State& st, const Node* n, Out& o, std::uint32_t* count) {
    if (n->is_volatile) return;
    if (Dirty(n)) {
        const std::uint32_t fl = KeyFlags(n);
        const BaseKey* b = nullptr;
        if (!st.base.empty()) {
            auto it = st.base.find(Upcase(PathOf(n)));
            if (it != st.base.end()) b = &it->second;
        }
        const bool same_key = b && b->fl == fl && b->cls == n->cls;
        std::vector<const OvValue*> vals;
        for (const OvValue* v : SortedValues(n)) {
            if (same_key) {
                auto bv = b->vals.find(Upcase(v->name));
                if (bv != b->vals.end() && bv->second.type == v->type && bv->second.deleted == v->deleted &&
                    bv->second.data == v->data)
                    continue;
            }
            vals.push_back(v);
        }
        if (!same_key || !vals.empty()) {
            ++*count;
            o.Str(PathOf(n));
            o.U32(fl);
            o.U64(n->mtime);
            o.Str(n->cls);
            o.U32(static_cast<std::uint32_t>(vals.size()));
            for (auto* v : vals) {
                o.U32(v->deleted ? 1 : 0);
                o.U32(v->type);
                o.Str(v->name);
                o.U32(static_cast<std::uint32_t>(v->data.size()));
                o.Bytes(v->data.data(), v->data.size());
            }
        }
    }
    if (n->deleted) return;
    for (auto& [up, k] : n->kids) SaveWalk(st, k, o, count);
}

std::string SerializeOverlay(const State& st, std::uint32_t* count_out) {
    Out body;
    std::uint32_t count = 0;
    SaveWalk(st, st.root, body, &count);
    Out all;
    all.Bytes(kOverlayMagic, 8);
    all.U32(count);
    all.Bytes(body.b.data(), body.b.size());
    if (count_out) *count_out = count;
    return std::string(reinterpret_cast<const char*>(all.b.data()), all.b.size());
}

// The lock file serialises the processes of one machine folder around loading and replacing the overlay file. It is
// opened with no sharing, so a second process waits (the OS releases it when a holder dies); the file itself stays.
// INVALID_HANDLE_VALUE: no lock (the folder is read-only, or a holder did not let go in 10 s); callers go on without it.
HANDLE LockOverlay(const State& st) {
    const std::wstring lock = st.root_dir + L"\\registry-overlay.lock";
    for (int i = 0; i < 1000; ++i) {
        HANDLE h = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return INVALID_HANDLE_VALUE;
        Sleep(10);
    }
    WARN(registry, "overlay lock %ls is held by another process: going on without it", lock.c_str());
    return INVALID_HANDLE_VALUE;
}

void UnlockOverlay(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Write the temp file, then replace the overlay with it: a reader sees the old or the new file, never half of one.
NTSTATUS WriteOverlayFile(const State& st, const std::string& bytes) {
    const std::wstring path = st.root_dir + L"\\registry-overlay.bin";
    const std::wstring tmp = path + L".tmp";
    HANDLE lock = LockOverlay(st);
    bool ok = false;
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::size_t done = 0;
        ok = true;
        while (done < bytes.size()) {
            DWORD w = 0;
            if (!WriteFile(f, bytes.data() + done, static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 24)), &w, nullptr) || !w) { ok = false; break; }
            done += w;
        }
        ok = ok && FlushFileBuffers(f);
        CloseHandle(f);
        if (ok) {
            ok = false;
            for (int i = 0; i < 50 && !ok; ++i) {   // an unlocked reader may still have the old file open
                ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                if (!ok) Sleep(20);
            }
        }
        if (!ok) DeleteFileW(tmp.c_str());
    }
    UnlockOverlay(lock);
    return ok ? nt::kSuccess : nt::kUnsuccessful;
}

// Save when `only_if_changed` is false or the overlay differs from what was last loaded or saved.
NTSTATUS SaveOverlayLocked(State& st, bool only_if_changed) {
    std::uint32_t count = 0;
    std::string bytes = SerializeOverlay(st, &count);
    if (only_if_changed && bytes == st.persisted) return nt::kSuccess;
    NTSTATUS s = WriteOverlayFile(st, bytes);
    if (s < 0) {
        if (!st.persist_warned) {
            st.persist_warned = true;
            FIXME(registry, "cannot write %ls\\registry-overlay.bin (error %lu): registry changes are not persisted",
                  st.root_dir.c_str(), static_cast<unsigned long>(GetLastError()));
        }
        return s;
    }
    st.persisted = std::move(bytes);
    TRACE(registry, "saved %u overlay keys to %ls\\registry-overlay.bin", count, st.root_dir.c_str());
    return nt::kSuccess;
}

NTSTATUS LoadOverlayLocked(State& st);

}  // namespace

NTSTATUS SaveOverlay() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    return SaveOverlayLocked(*g_st, false);
}

NTSTATUS EnablePersistence() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    State& st = *g_st;
    st.base.clear();
    CaptureWalk(st, st.root);
    NTSTATUS s = LoadOverlayLocked(st);
    if (s == nt::kObjectNameNotFound) s = nt::kSuccess;   // no file yet
    else if (s < 0) WARN(registry, "overlay file ignored from the first damaged record on (0x%08lx)", static_cast<unsigned long>(s));
    st.persisted = SerializeOverlay(st, nullptr);
    st.persist = true;
    return s;
}

void PersistOverlay() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_st && g_st->persist) SaveOverlayLocked(*g_st, true);
}

namespace {

NTSTATUS LoadOverlayLocked(State& st) {
    const std::wstring path = st.root_dir + L"\\registry-overlay.bin";
    HANDLE lock = LockOverlay(st);
    std::string raw = ReadFileBytes(path, false);
    UnlockOverlay(lock);
    if (raw.empty()) return nt::kObjectNameNotFound;
    In in{reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()};
    if (!in.Need(8) || std::memcmp(raw.data(), kOverlayMagic, 8) != 0) return kStatusRegistryCorrupt;
    in.i = 8;
    std::uint32_t count = in.U32();
    for (std::uint32_t r = 0; r < count && in.ok; ++r) {
        std::wstring kp = in.Str();
        std::uint32_t fl = in.U32();
        std::uint64_t mt = in.U64();
        std::wstring cls = in.Str();
        std::uint32_t nv = in.U32();
        struct Rec { bool del; ULONG type; std::wstring name; std::vector<std::uint8_t> data; };
        std::vector<Rec> recs;
        for (std::uint32_t i = 0; i < nv && in.ok; ++i) {
            Rec rc;
            rc.del = in.U32() != 0;
            rc.type = in.U32();
            rc.name = in.Str();
            rc.data = in.Blob();
            recs.push_back(std::move(rc));
        }
        if (!in.ok) break;
        ResolveOpts ro;
        ro.open_link = true;
        Node* n = nullptr;
        NTSTATUS s = Resolve(st, nullptr, kp, ro, &n, nullptr);
        if (s < 0) {
            if (fl & kFlagDeleted) continue;
            if (!(fl & kFlagCreated)) { WARN(registry, "overlay: %ls no longer exists; skipped", kp.c_str()); continue; }
            ro.create = true;
            ro.parents = true;
            s = Resolve(st, nullptr, kp, ro, &n, nullptr);
            if (s < 0) { WARN(registry, "overlay: cannot recreate %ls (0x%08lx)", kp.c_str(), static_cast<unsigned long>(s)); continue; }
        } else if ((fl & kFlagCreated) && (n->shadow || (n->is_mount && Backed(n)))) {
            ResetKey(st, n);
        }
        if (fl & kFlagLink) n->link = true;
        if (fl & kFlagClass) { n->has_class = true; n->cls = cls; }
        for (auto& rc : recs) {
            if (rc.del) {
                OvValue& o = n->ov[Upcase(rc.name)];
                o.name = rc.name; o.deleted = true; o.data.clear(); o.seq = ++n->ov_seq;
                n->vals_valid = false;
            } else {
                StoreValue(st, n, rc.name, rc.type, rc.data.data(), rc.data.size());
            }
        }
        if (fl & kFlagDeleted) MarkDeleted(st, n);
        if (mt) n->mtime = mt;
    }
    return in.ok ? nt::kSuccess : kStatusRegistryCorrupt;
}

}  // namespace

NTSTATUS LoadOverlay() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_st) return nt::kUnsuccessful;
    return LoadOverlayLocked(*g_st);
}

}  // namespace win10xbox::registry
