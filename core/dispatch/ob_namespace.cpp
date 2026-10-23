// The object manager namespace tree (see ob_namespace.h).
#include "ob_namespace.h"
#include <algorithm>
#include <cstring>

namespace win10xbox::dispatch::detail::ob {
namespace {

constexpr int kMaxLinkDepth = 16;
constexpr NTSTATUS kReparseNotResolved = static_cast<NTSTATUS>(0xC0000368);

// OBJECT_DIRECTORY_INFORMATION (winternl.h of the SDK does not carry it).
struct DirInfo {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
};
static_assert(sizeof(DirInfo) == 32 || sizeof(void*) != 8, "OBJECT_DIRECTORY_INFORMATION is 32 bytes on x64");

struct SharedLock {
    explicit SharedLock(SRWLOCK* l) : l_(l) { AcquireSRWLockShared(l_); }
    ~SharedLock() { ReleaseSRWLockShared(l_); }
    SRWLOCK* l_;
};
struct ExclusiveLock {
    explicit ExclusiveLock(SRWLOCK* l) : l_(l) { AcquireSRWLockExclusive(l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(l_); }
    SRWLOCK* l_;
};

}  // namespace

bool NameEquals(std::wstring_view a, std::wstring_view b, bool ci) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    if (!ci) return std::wmemcmp(a.data(), b.data(), a.size()) == 0;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ParentPath(std::wstring_view path) {
    std::size_t s = path.rfind(L'\\');
    if (s == std::wstring_view::npos) return {};
    if (s == 0) return L"\\";
    return std::wstring(path.substr(0, s));
}

std::wstring LeafOf(std::wstring_view path) {
    std::size_t s = path.rfind(L'\\');
    return std::wstring(s == std::wstring_view::npos ? path : path.substr(s + 1));
}

std::wstring MangleName(std::wstring_view prefix, std::wstring_view guest_path, bool* hashed) {
    std::wstring body;
    body.reserve(guest_path.size() + 8);
    for (wchar_t c : guest_path) {
        if (c == L'\\') body += L'|';
        else if (c == L'|') body += L"%7C";
        else if (c == L'%') body += L"%25";
        else body += c;
    }
    std::wstring out(prefix);
    if (body.size() <= kMaxPlainName) {
        if (hashed) *hashed = false;
        out += L'!';
        out += body;
        return out;
    }
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a over the upper-cased UTF-16 code units
    for (wchar_t c : guest_path) {
        const std::uint32_t u = static_cast<std::uint32_t>(c >= L'a' && c <= L'z' ? c - 32 : c);
        h = (h ^ (u & 0xFF)) * 1099511628211ull;
        h = (h ^ (u >> 8)) * 1099511628211ull;
    }
    const wchar_t* const kHex = L"0123456789abcdef";
    out += L'#';
    for (int shift = 60; shift >= 0; shift -= 4) out += kHex[(h >> shift) & 0xF];
    if (hashed) *hashed = true;
    return out;
}

bool DemangleName(std::wstring_view prefix, std::wstring_view host_leaf, std::wstring* guest_path, bool* hashed) {
    if (host_leaf.size() <= prefix.size() || host_leaf.compare(0, prefix.size(), prefix) != 0) return false;
    const wchar_t tag = host_leaf[prefix.size()];
    std::wstring_view body = host_leaf.substr(prefix.size() + 1);
    if (tag == L'#') {
        if (hashed) *hashed = true;
        if (guest_path) *guest_path = std::wstring(body);
        return true;
    }
    if (tag != L'!') return false;
    if (hashed) *hashed = false;
    std::wstring out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        wchar_t c = body[i];
        if (c == L'|') out += L'\\';
        else if (c == L'%') {
            if (body.substr(i, 3) == L"%7C") out += L'|';
            else if (body.substr(i, 3) == L"%25") out += L'%';
            else return false;
            i += 2;
        } else out += c;
    }
    if (guest_path) *guest_path = std::move(out);
    return true;
}

struct Tree::Impl {
    SRWLOCK lock = SRWLOCK_INIT;
    NodeRef root;
    std::uint64_t serial = 0;
    bool populated = false;
    unsigned populated_session = 0;
    std::wstring populated_sid;

    NodeRef Make(Kind kind) {
        auto n = std::make_shared<Node>();
        n->kind = kind;
        n->type_name = kind == Kind::Directory ? L"Directory" : kind == Kind::SymbolicLink ? L"SymbolicLink" : L"Device";
        n->serial = ++serial;
        return n;
    }

    // Lock held.
    NodeRef Find(const NodeRef& dir, std::wstring_view name, bool ci) {
        for (const NodeRef& c : dir->children)
            if (NameEquals(c->name, name, ci)) return c;
        if (dir->fallback)
            for (const NodeRef& c : dir->fallback->children)
                if (NameEquals(c->name, name, ci)) return c;
        return nullptr;
    }

    // Lock held.
    std::wstring Path(const NodeRef& n) {
        if (!n) return {};
        if (n == root) return L"\\";
        std::vector<std::wstring_view> parts;
        NodeRef cur = n;
        std::vector<NodeRef> keep;
        while (cur && cur != root) {
            parts.push_back(cur->name);
            NodeRef p = cur->parent.lock();
            if (!p) {
                if (cur != n || cur->name.empty()) return {};   // detached: an unnamed object has no path
                break;
            }
            keep.push_back(p);
            cur = p;
        }
        std::wstring out;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            out += L'\\';
            out.append(*it);
        }
        return out;
    }

    // Lock held. Adds `child` below `dir`.
    void Attach(const NodeRef& dir, const NodeRef& child) {
        child->parent = dir;
        dir->children.push_back(child);
    }

    // Lock held. Removes a temporary node nobody holds, then its parents.
    void Prune(NodeRef n) {
        while (n && n != root && !n->permanent && !n->kernel && n->pins == 0 && n->children.empty()) {
            NodeRef p = n->parent.lock();
            if (!p) return;
            auto it = std::find(p->children.begin(), p->children.end(), n);
            if (it != p->children.end()) p->children.erase(it);
            n->parent.reset();
            n = p;
        }
    }

    // Lock held. `path` is absolute and canonical; intermediate directories are created as kernel directories.
    NodeRef EnsureDir(std::wstring_view path) {
        NodeRef cur = root;
        std::size_t i = 1;
        while (i <= path.size() && !path.empty() && path != L"\\") {
            std::size_t e = path.find(L'\\', i);
            if (e == std::wstring_view::npos) e = path.size();
            std::wstring_view comp = path.substr(i, e - i);
            if (comp.empty()) break;
            NodeRef c = Find(cur, comp, true);
            if (!c || c->kind != Kind::Directory) {
                c = Make(Kind::Directory);
                c->name = std::wstring(comp);
                c->kernel = c->permanent = true;
                Attach(cur, c);
            }
            cur = c;
            i = e + 1;
        }
        return cur;
    }

    std::uint64_t CountRec(const NodeRef& n, Kind k) {
        std::uint64_t c = n->kind == k ? 1 : 0;
        for (const NodeRef& ch : n->children) c += CountRec(ch, k);
        return c;
    }
};

Tree::Tree() : impl_(std::make_shared<Impl>()) {
    impl_->root = impl_->Make(Kind::Directory);
    impl_->root->kernel = impl_->root->permanent = true;
}

NodeRef Tree::Root() { return impl_->root; }

void Tree::Populate(unsigned session, const std::wstring& ac_sid) {
    ExclusiveLock lk(&impl_->lock);
    if (impl_->populated && impl_->populated_session == session && impl_->populated_sid == ac_sid) return;
    session_ = session;
    Impl& m = *impl_;
    m.populated = true;
    m.populated_session = session;
    m.populated_sid = ac_sid;
    const std::wstring sess = L"\\Sessions\\" + std::to_wstring(session);
    for (const wchar_t* d : {L"\\BaseNamedObjects", L"\\Sessions", L"\\KernelObjects", L"\\ObjectTypes", L"\\GLOBAL??"})
        m.EnsureDir(d);
    m.EnsureDir(sess);
    m.EnsureDir(sess + L"\\BaseNamedObjects");
    m.EnsureDir(sess + L"\\AppContainerNamedObjects");
    m.EnsureDir(L"\\Sessions\\BNOLINKS");
    if (!ac_sid.empty()) m.EnsureDir(sess + L"\\AppContainerNamedObjects\\" + ac_sid);

    auto link = [&](const std::wstring& dir_path, const wchar_t* name, const std::wstring& target) {
        NodeRef dir = m.EnsureDir(dir_path);
        if (m.Find(dir, name, true)) return;
        NodeRef l = m.Make(Kind::SymbolicLink);
        l->name = name;
        l->target = target;
        l->kernel = l->permanent = true;
        m.Attach(dir, l);
    };
    // Global names resolve to the machine-wide directory, Local to this session's.
    link(L"\\BaseNamedObjects", L"Global", L"\\BaseNamedObjects");
    link(L"\\BaseNamedObjects", L"Local", L"\\BaseNamedObjects");
    link(L"\\BaseNamedObjects", L"Session", L"\\Sessions\\BNOLINKS");
    link(sess + L"\\BaseNamedObjects", L"Global", L"\\BaseNamedObjects");
    link(sess + L"\\BaseNamedObjects", L"Local", sess + L"\\BaseNamedObjects");
    link(sess + L"\\BaseNamedObjects", L"Session", L"\\Sessions\\BNOLINKS");
    link(L"\\Sessions\\BNOLINKS", std::to_wstring(session).c_str(), sess + L"\\BaseNamedObjects");
    // \?? (the caller's DosDevices) is a directory whose misses continue in \GLOBAL??; \DosDevices is a link to it.
    NodeRef dos = m.Find(m.root, L"??", false);
    if (!dos) {
        dos = m.Make(Kind::Directory);
        dos->name = L"??";
        dos->kernel = dos->permanent = true;
        m.Attach(m.root, dos);
    }
    dos->fallback = m.Find(m.root, L"GLOBAL??", false);
    link(L"\\", L"DosDevices", L"\\??");
}

Resolution Tree::Resolve(const NodeRef& root, std::wstring_view name, bool ci, bool follow_final) {
    SharedLock lk(&impl_->lock);
    Impl& m = *impl_;
    Resolution r;
    std::wstring path(name);
    NodeRef cur;
    std::size_t i = 0;
    int links = 0;
    bool rooted_by_handle = false;
    if (root) {
        if (!name.empty() && name[0] == L'\\') { r.status = status::PathSyntaxBad; return r; }
        if (name.empty()) { r.node = root; return r; }
        if (root->kind != Kind::Directory) { r.status = status::TypeMismatch; return r; }
        cur = root;
        i = 0;
        rooted_by_handle = true;
    } else {
        if (name.empty() || name[0] != L'\\') { r.status = status::PathSyntaxBad; return r; }
        cur = m.root;
        i = 1;
        if (name.size() == 1) { r.node = cur; return r; }
    }
    for (;;) {
        std::size_t e = path.find(L'\\', i);
        const bool more = e != std::wstring::npos;
        if (!more) e = path.size();
        if (e == i) {
            if (i >= path.size() && !more) {   // the walk consumed the whole name (a link target ending the path)
                r.node = cur;
                return r;
            }
            r.status = status::NameInvalid;
            return r;
        }
        std::wstring_view comp(path.data() + i, e - i);
        NodeRef child = m.Find(cur, comp, ci);
        if (!child) {
            if (cur == m.root) {   // not one of ours: the host's namespace
                r.foreign = true;
                r.node = cur;
                r.leaf = std::wstring(comp);
                r.rest = L"\\" + path.substr(i);
                return r;
            }
            if (more) { r.status = status::PathNotFound; return r; }
            r.node = cur;
            r.leaf = std::wstring(comp);
            r.dir_path = m.Path(cur);
            if (cur->fallback) r.dir_path = m.Path(cur);
            return r;
        }
        if (more) {
            const bool trailing = e + 1 == path.size();
            if (child->kind == Kind::Directory) {
                if (trailing) { r.status = status::NameInvalid; return r; }
                cur = child;
                i = e + 1;
                continue;
            }
            if (child->kind == Kind::SymbolicLink) {
                if (++links > kMaxLinkDepth) { r.status = kReparseNotResolved; return r; }
                if (!child->target.empty() && child->target[0] != L'\\') { r.status = status::PathNotFound; return r; }
                path = child->target + path.substr(e);
                cur = m.root;
                i = 1;
                continue;
            }
            r.status = status::TypeMismatch;
            return r;
        }
        if (child->kind == Kind::SymbolicLink && follow_final) {
            if (++links > kMaxLinkDepth) { r.status = kReparseNotResolved; return r; }
            if (child->target.empty() || child->target == L"\\") { r.node = m.root; return r; }
            if (child->target[0] != L'\\') { r.status = status::NameNotFound; return r; }
            path = child->target;
            cur = m.root;
            i = 1;
            continue;
        }
        r.node = child;
        (void)rooted_by_handle;
        return r;
    }
}

NodeRef Tree::CreateChild(const NodeRef& dir, std::wstring_view leaf, Kind kind, std::wstring_view target, bool permanent) {
    ExclusiveLock lk(&impl_->lock);
    Impl& m = *impl_;
    if (!dir || dir->kind != Kind::Directory) return nullptr;
    for (const NodeRef& c : dir->children)
        if (NameEquals(c->name, leaf, false)) return nullptr;
    NodeRef n = m.Make(kind);
    n->name = std::wstring(leaf);
    n->target = std::wstring(target);
    n->permanent = permanent;
    n->pins = 1;
    m.Attach(dir, n);
    return n;
}

NodeRef Tree::CreateUnnamed(Kind kind, std::wstring_view target) {
    ExclusiveLock lk(&impl_->lock);
    NodeRef n = impl_->Make(kind);
    n->target = std::wstring(target);
    n->pins = 1;
    return n;
}

NodeRef Tree::AddKernelNode(std::wstring_view path, Kind kind, std::wstring_view type_or_target) {
    ExclusiveLock lk(&impl_->lock);
    Impl& m = *impl_;
    std::wstring dir_path = ParentPath(path), leaf = LeafOf(path);
    NodeRef dir = m.EnsureDir(dir_path.empty() ? L"\\" : dir_path);
    for (auto it = dir->children.begin(); it != dir->children.end(); ++it)
        if (NameEquals((*it)->name, leaf, true)) { dir->children.erase(it); break; }
    NodeRef n = m.Make(kind);
    n->name = leaf;
    n->kernel = n->permanent = true;
    if (kind == Kind::SymbolicLink) n->target = std::wstring(type_or_target);
    else if (kind == Kind::Leaf && !type_or_target.empty()) n->type_name = std::wstring(type_or_target);
    m.Attach(dir, n);
    return n;
}

void Tree::RemoveKernelNode(std::wstring_view path) {
    ExclusiveLock lk(&impl_->lock);
    Impl& m = *impl_;
    NodeRef dir = m.EnsureDir(ParentPath(path));
    std::wstring leaf = LeafOf(path);
    for (auto it = dir->children.begin(); it != dir->children.end(); ++it)
        if ((*it)->kernel && NameEquals((*it)->name, leaf, true)) { dir->children.erase(it); return; }
}

void Tree::ReplaceKernelLinks(std::wstring_view dir_path, const std::vector<std::pair<std::wstring, std::wstring>>& links,
                              std::wstring_view tag) {
    ExclusiveLock lk(&impl_->lock);
    Impl& m = *impl_;
    NodeRef dir = m.EnsureDir(dir_path);
    dir->children.erase(std::remove_if(dir->children.begin(), dir->children.end(),
                                       [&](const NodeRef& c) { return c->kernel && c->tag == tag; }),
                        dir->children.end());
    for (const auto& [name, target] : links) {
        bool taken = false;
        for (const NodeRef& c : dir->children) taken = taken || NameEquals(c->name, name, true);
        if (taken) continue;
        NodeRef l = m.Make(Kind::SymbolicLink);
        l->name = name;
        l->target = target;
        l->kernel = l->permanent = true;
        l->tag = std::wstring(tag);
        m.Attach(dir, l);
    }
}

void Tree::Pin(const NodeRef& n) {
    if (!n) return;
    ExclusiveLock lk(&impl_->lock);
    ++n->pins;
}

void Tree::Unpin(const NodeRef& n) {
    if (!n) return;
    ExclusiveLock lk(&impl_->lock);
    if (n->pins) --n->pins;
    impl_->Prune(n);
}

void Tree::MakePermanent(const NodeRef& n, bool permanent) {
    if (!n) return;
    ExclusiveLock lk(&impl_->lock);
    n->permanent = permanent;
    if (!permanent) impl_->Prune(n);
}

std::wstring Tree::PathOf(const NodeRef& n) {
    SharedLock lk(&impl_->lock);
    return impl_->Path(n);
}

std::wstring Tree::Target(const NodeRef& n) {
    SharedLock lk(&impl_->lock);
    return n ? n->target : std::wstring();
}

std::vector<DirEntry> Tree::List(const NodeRef& dir) {
    SharedLock lk(&impl_->lock);
    std::vector<DirEntry> out;
    if (!dir) return out;
    for (const NodeRef& c : dir->children) out.push_back({c->name, c->type_name});
    return out;
}

std::uint64_t Tree::Count(Kind k) {
    SharedLock lk(&impl_->lock);
    return impl_->CountRec(impl_->root, k);
}

NTSTATUS PackDirectoryEntries(const std::vector<DirEntry>& entries, void* buffer, ULONG size, bool single, bool restart,
                              ULONG* context, ULONG* ret_size) {
    const ULONG index = restart ? 0 : (context ? *context : 0);
    const std::size_t total = entries.size();
    constexpr ULONG kRow = sizeof(DirInfo);
    NTSTATUS status = status::Success;

    // What the producer can hand over for this buffer (the server half in Wine, the object manager in Windows).
    std::size_t count = 0, total_len = 0, srv_size = 0;
    const std::size_t max_count = single ? 1 : static_cast<std::size_t>(-1);
    std::size_t i = 0;
    for (; i < max_count; ++i) {
        const std::size_t idx = static_cast<std::size_t>(index) + i;
        if (idx >= total) break;
        const std::size_t nl = entries[idx].name.size() * sizeof(wchar_t), tl = entries[idx].type.size() * sizeof(wchar_t);
        const std::size_t entry_size = (8 + nl + tl + 3) & ~static_cast<std::size_t>(3);
        total_len += nl + tl;
        if (srv_size + entry_size > size) { status = status::MoreEntries; break; }
        srv_size += entry_size;
    }
    count = i;

    // What fits as OBJECT_DIRECTORY_INFORMATION rows + strings, after the all-zero row that ends the array.
    std::size_t used_count = 0, used_size = kRow;
    for (i = 0; i < count; ++i) {
        const DirEntry& e = entries[index + i];
        const std::size_t entry_size = kRow + (e.name.size() + e.type.size()) * sizeof(wchar_t) + 2 * sizeof(wchar_t);
        if (used_size + entry_size > size) { status = status::MoreEntries; break; }
        ++used_count;
        used_size += entry_size;
    }

    auto* rows = static_cast<DirInfo*>(buffer);
    std::size_t pool = kRow * (used_count + 1);
    for (i = 0; i < used_count; ++i) {
        const DirEntry& e = entries[index + i];
        const USHORT nl = static_cast<USHORT>(e.name.size() * sizeof(wchar_t)), tl = static_cast<USHORT>(e.type.size() * sizeof(wchar_t));
        auto* base = static_cast<char*>(buffer);
        auto* np = reinterpret_cast<wchar_t*>(base + pool);
        rows[i].Name.Buffer = np;
        rows[i].Name.Length = nl;
        rows[i].Name.MaximumLength = static_cast<USHORT>(nl + sizeof(wchar_t));
        std::memcpy(np, e.name.data(), nl);
        np[e.name.size()] = 0;
        pool += nl + sizeof(wchar_t);
        auto* tp = reinterpret_cast<wchar_t*>(base + pool);
        rows[i].TypeName.Buffer = tp;
        rows[i].TypeName.Length = tl;
        rows[i].TypeName.MaximumLength = static_cast<USHORT>(tl + sizeof(wchar_t));
        std::memcpy(tp, e.type.data(), tl);
        tp[e.type.size()] = 0;
        pool += tl + sizeof(wchar_t);
    }
    if (size >= kRow) std::memset(&rows[used_count], 0, kRow);

    if (!count && status == status::Success) {
        if (ret_size) *ret_size = kRow;
        return status::NoMoreEntries;
    }
    if (single && !used_count) {
        if (ret_size) *ret_size = static_cast<ULONG>(2 * kRow + 2 * sizeof(wchar_t) + total_len);
        return status::BufferTooSmall;
    }
    if (context) *context = index + static_cast<ULONG>(used_count);
    if (ret_size) *ret_size = static_cast<ULONG>(pool);
    return status;
}

}  // namespace win10xbox::dispatch::detail::ob
