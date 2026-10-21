// REGF hive reader and writer. See regf.h.
#include "regf.h"

#include <algorithm>
#include <cstring>

namespace win10xbox::registry::regf {

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr std::size_t kBaseBlock = 0x1000;
constexpr std::size_t kHbinHeader = 0x20;
constexpr std::uint32_t kMaxBlockData = 16344;   // db record block payload
constexpr std::size_t kMaxValueLen = 256u << 20;
constexpr std::size_t kMaxListEntries = 4u << 20;
constexpr int kMaxTreeDepth = 512;

inline std::uint16_t Rd16(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
inline std::uint32_t Rd32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline std::uint64_t Rd64(const std::uint8_t* p) { std::uint64_t v; std::memcpy(&v, p, 8); return v; }

bool AllNarrow(std::wstring_view s) {
    for (wchar_t c : s) if (c >= 0x80) return false;
    return true;
}

std::wstring Latin1ToWide(const std::uint8_t* p, std::size_t n) {
    std::wstring s(n, L'\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<wchar_t>(p[i]);
    return s;
}

std::wstring Utf16ToWide(const std::uint8_t* p, std::size_t bytes) {
    std::wstring s(bytes / 2, L'\0');
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<wchar_t>(Rd16(p + i * 2));
    return s;
}

// lh hash: h = h*37 + upcase(c) over the name.
std::uint32_t LhHash(std::wstring_view upper_name) {
    std::uint32_t h = 0;
    for (wchar_t c : upper_name) h = h * 37u + static_cast<std::uint32_t>(c);
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Case folding
// ---------------------------------------------------------------------------

wchar_t UpcaseChar(wchar_t c) {
    if (c < 0x80) return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - 32) : c;
    wchar_t out = c;
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, &c, 1, &out, 1, nullptr, nullptr, 0) != 1) return c;
    return out;
}

std::wstring Upcase(std::wstring_view s) {
    std::wstring r(s);
    for (auto& c : r) c = UpcaseChar(c);
    return r;
}

bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i] && UpcaseChar(a[i]) != UpcaseChar(b[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Key tree helpers
// ---------------------------------------------------------------------------

Key* Key::AddKey(std::wstring n) {
    subkeys.push_back(std::make_unique<Key>());
    subkeys.back()->name = std::move(n);
    subkeys.back()->timestamp = timestamp;
    return subkeys.back().get();
}

Value& Key::AddValue(std::wstring n, std::uint32_t type, std::vector<std::uint8_t> data) {
    Value v;
    v.name = std::move(n);
    v.type = type;
    v.data = std::move(data);
    values.push_back(std::move(v));
    return values.back();
}

Value& Key::AddString(std::wstring n, std::wstring_view s, std::uint32_t type) {
    std::vector<std::uint8_t> d((s.size() + 1) * 2, 0);
    for (std::size_t i = 0; i < s.size(); ++i) {
        d[i * 2] = static_cast<std::uint8_t>(s[i] & 0xFF);
        d[i * 2 + 1] = static_cast<std::uint8_t>((s[i] >> 8) & 0xFF);
    }
    return AddValue(std::move(n), type, std::move(d));
}

Value& Key::AddDword(std::wstring n, std::uint32_t v) {
    std::vector<std::uint8_t> d(4);
    std::memcpy(d.data(), &v, 4);
    return AddValue(std::move(n), 4 /*REG_DWORD*/, std::move(d));
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

namespace {

class Builder {
public:
    std::vector<std::uint8_t> data;   // everything after the 4 KiB base block

    std::uint32_t Alloc(std::size_t total) {
        std::size_t need = (total + 7) & ~static_cast<std::size_t>(7);
        if (need < 8) need = 8;
        if (hbin_size_ == 0 || cursor_ + need > hbin_start_ + hbin_size_) {
            CloseHbin();
            OpenHbin(need);
        }
        std::uint32_t off = static_cast<std::uint32_t>(cursor_);
        cursor_ += need;
        W32(off, static_cast<std::uint32_t>(-static_cast<std::int32_t>(need)));
        return off;
    }
    void Finish() { CloseHbin(); }

    void W8(std::size_t o, std::uint8_t v) { data[o] = v; }
    void W16(std::size_t o, std::uint16_t v) { std::memcpy(&data[o], &v, 2); }
    void W32(std::size_t o, std::uint32_t v) { std::memcpy(&data[o], &v, 4); }
    void W64(std::size_t o, std::uint64_t v) { std::memcpy(&data[o], &v, 8); }
    void WBytes(std::size_t o, const void* p, std::size_t n) { if (n) std::memcpy(&data[o], p, n); }
    void WId(std::size_t o, const char* id) { data[o + 4] = static_cast<std::uint8_t>(id[0]); data[o + 5] = static_cast<std::uint8_t>(id[1]); }

    std::uint32_t sk = 0;
    std::uint32_t key_count = 0;

private:
    void OpenHbin(std::size_t need) {
        std::size_t size = (need + kHbinHeader + 0xFFF) & ~static_cast<std::size_t>(0xFFF);
        if (size < 0x1000) size = 0x1000;
        hbin_start_ = data.size();
        hbin_size_ = size;
        data.resize(hbin_start_ + size, 0);
        std::memcpy(&data[hbin_start_], "hbin", 4);
        W32(hbin_start_ + 4, static_cast<std::uint32_t>(hbin_start_));
        W32(hbin_start_ + 8, static_cast<std::uint32_t>(size));
        cursor_ = hbin_start_ + kHbinHeader;
    }
    void CloseHbin() {
        if (hbin_size_ == 0) return;
        std::size_t remaining = hbin_start_ + hbin_size_ - cursor_;
        if (remaining >= 8) W32(cursor_, static_cast<std::uint32_t>(remaining));   // positive = free
        hbin_size_ = 0;
    }
    std::size_t hbin_start_ = 0, hbin_size_ = 0, cursor_ = 0;
};

std::vector<std::uint8_t> NarrowOrWide(std::wstring_view s, bool* narrow) {
    *narrow = AllNarrow(s);
    std::vector<std::uint8_t> r;
    if (*narrow) {
        for (wchar_t c : s) r.push_back(static_cast<std::uint8_t>(c));
    } else {
        for (wchar_t c : s) { r.push_back(static_cast<std::uint8_t>(c & 0xFF)); r.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFF)); }
    }
    return r;
}

std::uint32_t WriteValue(Builder& b, const Value& v) {
    bool narrow = true;
    auto name = NarrowOrWide(v.name, &narrow);
    std::uint32_t vk = b.Alloc(0x18 + name.size());
    b.WId(vk, "vk");
    b.W16(vk + 6, static_cast<std::uint16_t>(name.size()));
    const std::size_t len = v.data.size();
    if (len <= 4) {
        b.W32(vk + 8, 0x80000000u | static_cast<std::uint32_t>(len));
        if (len) b.WBytes(vk + 0xC, v.data.data(), len);
    } else if (len <= kMaxBlockData) {
        std::uint32_t d = b.Alloc(4 + len);
        b.WBytes(d + 4, v.data.data(), len);
        b.W32(vk + 8, static_cast<std::uint32_t>(len));
        b.W32(vk + 0xC, d);
    } else {
        std::uint32_t nblocks = static_cast<std::uint32_t>((len + kMaxBlockData - 1) / kMaxBlockData);
        std::vector<std::uint32_t> blocks;
        for (std::size_t pos = 0; pos < len; pos += kMaxBlockData) {
            std::size_t n = std::min<std::size_t>(kMaxBlockData, len - pos);
            std::uint32_t d = b.Alloc(4 + n);
            b.WBytes(d + 4, v.data.data() + pos, n);
            blocks.push_back(d);
        }
        std::uint32_t list = b.Alloc(4 + 4 * static_cast<std::size_t>(nblocks));
        for (std::uint32_t i = 0; i < nblocks; ++i) b.W32(list + 4 + 4 * i, blocks[i]);
        std::uint32_t db = b.Alloc(0x10);
        b.WId(db, "db");
        b.W16(db + 6, static_cast<std::uint16_t>(nblocks));
        b.W32(db + 8, list);
        b.W32(vk + 8, static_cast<std::uint32_t>(len));
        b.W32(vk + 0xC, db);
    }
    b.W32(vk + 0x10, v.type);
    b.W16(vk + 0x14, narrow && !name.empty() ? 1 : 0);
    b.WBytes(vk + 0x18, name.data(), name.size());
    return vk;
}

std::uint32_t WriteKey(Builder& b, const Key& k, std::uint32_t parent, bool is_root, int depth) {
    bool narrow = true;
    auto name = NarrowOrWide(k.name, &narrow);
    std::uint32_t nk = b.Alloc(0x50 + name.size());
    b.key_count++;
    b.WId(nk, "nk");
    std::uint16_t flags = 0;
    if (narrow) flags |= 0x20;
    if (is_root) flags |= 0x0C;
    if (k.symlink) flags |= 0x10;
    b.W16(nk + 6, flags);
    b.W64(nk + 8, k.timestamp);
    b.W32(nk + 0x14, parent);
    b.W32(nk + 0x1C, 0);
    b.W32(nk + 0x24, kNone);
    b.W32(nk + 0x30, b.sk);
    b.W32(nk + 0x34, kNone);
    b.W16(nk + 0x4C, static_cast<std::uint16_t>(name.size()));
    b.WBytes(nk + 0x50, name.data(), name.size());

    std::uint32_t max_sub_name = 0, max_sub_class = 0, max_val_name = 0, max_val_data = 0;

    // Subkeys, sorted by upper-cased name.
    if (!k.subkeys.empty() && depth < kMaxTreeDepth) {
        std::vector<std::pair<std::wstring, const Key*>> sorted;
        sorted.reserve(k.subkeys.size());
        for (auto& c : k.subkeys) sorted.emplace_back(Upcase(c->name), c.get());
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& c) { return a.first < c.first; });
        std::vector<std::uint32_t> offs, hashes;
        for (auto& [up, c] : sorted) {
            offs.push_back(WriteKey(b, *c, nk, false, depth + 1));
            hashes.push_back(AllNarrow(up) ? LhHash(up) : 0);
            max_sub_name = std::max<std::uint32_t>(max_sub_name, static_cast<std::uint32_t>(c->name.size() * 2));
            max_sub_class = std::max<std::uint32_t>(max_sub_class, static_cast<std::uint32_t>(c->class_name.size() * 2));
        }
        constexpr std::size_t kChunk = 500;
        std::vector<std::uint32_t> lists;
        for (std::size_t pos = 0; pos < offs.size(); pos += kChunk) {
            std::size_t n = std::min(kChunk, offs.size() - pos);
            std::uint32_t lh = b.Alloc(8 + 8 * n);
            b.WId(lh, "lh");
            b.W16(lh + 6, static_cast<std::uint16_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
                b.W32(lh + 8 + 8 * i, offs[pos + i]);
                b.W32(lh + 12 + 8 * i, hashes[pos + i]);
            }
            lists.push_back(lh);
        }
        std::uint32_t head = lists[0];
        if (lists.size() > 1) {
            head = b.Alloc(8 + 4 * lists.size());
            b.WId(head, "ri");
            b.W16(head + 6, static_cast<std::uint16_t>(lists.size()));
            for (std::size_t i = 0; i < lists.size(); ++i) b.W32(head + 8 + 4 * i, lists[i]);
        }
        b.W32(nk + 0x18, static_cast<std::uint32_t>(offs.size()));
        b.W32(nk + 0x20, head);
    } else {
        b.W32(nk + 0x20, kNone);
    }

    // Values.
    if (!k.values.empty()) {
        std::vector<std::uint32_t> vks;
        for (auto& v : k.values) {
            vks.push_back(WriteValue(b, v));
            max_val_name = std::max<std::uint32_t>(max_val_name, static_cast<std::uint32_t>(v.name.size() * 2));
            max_val_data = std::max<std::uint32_t>(max_val_data, static_cast<std::uint32_t>(v.data.size()));
        }
        std::uint32_t list = b.Alloc(4 + 4 * vks.size());
        for (std::size_t i = 0; i < vks.size(); ++i) b.W32(list + 4 + 4 * i, vks[i]);
        b.W32(nk + 0x28, static_cast<std::uint32_t>(vks.size()));
        b.W32(nk + 0x2C, list);
    } else {
        b.W32(nk + 0x2C, kNone);
    }

    // Class name.
    if (!k.class_name.empty()) {
        std::size_t bytes = k.class_name.size() * 2;
        std::uint32_t c = b.Alloc(4 + bytes);
        for (std::size_t i = 0; i < k.class_name.size(); ++i) b.W16(c + 4 + 2 * i, static_cast<std::uint16_t>(k.class_name[i]));
        b.W32(nk + 0x34, c);
        b.W16(nk + 0x4E, static_cast<std::uint16_t>(bytes));
    }

    b.W16(nk + 0x38, static_cast<std::uint16_t>(std::min<std::uint32_t>(max_sub_name, 0xFFFF)));
    b.W32(nk + 0x3C, max_sub_class);
    b.W32(nk + 0x40, max_val_name);
    b.W32(nk + 0x44, max_val_data);
    return nk;
}

// Self-relative SD: owner/group SYSTEM, DACL with one allow-everyone ACE.
std::vector<std::uint8_t> DefaultSecurityDescriptor() {
    static const std::uint8_t sid_system[12] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
    static const std::uint8_t sid_world[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    std::vector<std::uint8_t> sd(72, 0);
    sd[0] = 1;                       // revision
    sd[2] = 0x04; sd[3] = 0x80;      // SE_DACL_PRESENT | SE_SELF_RELATIVE
    sd[4] = 20;                      // owner offset
    sd[8] = 32;                      // group offset
    sd[16] = 44;                     // dacl offset
    std::memcpy(&sd[20], sid_system, 12);
    std::memcpy(&sd[32], sid_system, 12);
    sd[44] = 2;                      // ACL revision
    sd[46] = 28;                     // ACL size
    sd[48] = 1;                      // ACE count
    sd[52] = 0;                      // ACCESS_ALLOWED_ACE_TYPE
    sd[54] = 20;                     // ACE size
    sd[56] = 0x3F; sd[57] = 0x00; sd[58] = 0x0F; sd[59] = 0x00;   // KEY_ALL_ACCESS
    std::memcpy(&sd[60], sid_world, 12);
    return sd;
}

}  // namespace

bool WriteHive(const Key& root, std::wstring_view hive_name, std::vector<std::uint8_t>* out) {
    if (!out) return false;
    Builder b;
    auto sd = DefaultSecurityDescriptor();
    b.sk = b.Alloc(0x18 + sd.size());
    b.WId(b.sk, "sk");
    b.W32(b.sk + 8, b.sk);    // prev
    b.W32(b.sk + 0xC, b.sk);  // next
    b.W32(b.sk + 0x14, static_cast<std::uint32_t>(sd.size()));
    b.WBytes(b.sk + 0x18, sd.data(), sd.size());

    std::uint32_t root_off = WriteKey(b, root, kNone, true, 0);
    b.W32(b.sk + 0x10, b.key_count);   // refcount
    b.Finish();

    std::vector<std::uint8_t> file(kBaseBlock, 0);
    std::memcpy(&file[0], "regf", 4);
    auto w32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(&file[o], &v, 4); };
    w32(4, 1);
    w32(8, 1);
    std::uint64_t ts = root.timestamp;
    std::memcpy(&file[0xC], &ts, 8);
    w32(0x14, 1);      // major
    w32(0x18, 5);      // minor
    w32(0x1C, 0);      // type: primary file
    w32(0x20, 1);      // format: direct memory load
    w32(0x24, root_off);
    w32(0x28, static_cast<std::uint32_t>(b.data.size()));
    w32(0x2C, 1);      // clustering factor
    for (std::size_t i = 0; i < hive_name.size() && i < 31; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(hive_name[i]);
        std::memcpy(&file[0x30 + i * 2], &c, 2);
    }
    std::uint32_t sum = 0;
    for (std::size_t o = 0; o < 0x1FC; o += 4) { std::uint32_t v; std::memcpy(&v, &file[o], 4); sum ^= v; }
    if (sum == 0xFFFFFFFFu) sum = 0xFFFFFFFEu; else if (sum == 0) sum = 1;
    w32(0x1FC, sum);

    out->clear();
    out->reserve(file.size() + b.data.size());
    out->insert(out->end(), file.begin(), file.end());
    out->insert(out->end(), b.data.begin(), b.data.end());
    return true;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

Hive::Hive() = default;
Hive::~Hive() { Close(); }

void Hive::Close() {
    if (view_) UnmapViewOfFile(view_);
    if (map_) CloseHandle(static_cast<HANDLE>(map_));
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file_));
    view_ = nullptr; map_ = nullptr; file_ = nullptr;
    owned_.clear();
    base_ = nullptr; len_ = 0; root_ = 0;
}

bool Hive::Open(const std::wstring& path) {
    Close();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < static_cast<LONGLONG>(kBaseBlock + kHbinHeader) ||
        sz.QuadPart > 0x7FFFFFFFLL) {
        CloseHandle(f);
        return false;
    }
    const std::uint8_t* whole = nullptr;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m) {
        const void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        if (v) { view_ = v; map_ = m; file_ = f; whole = static_cast<const std::uint8_t*>(v); }
        else CloseHandle(m);
    }
    if (!whole) {
        owned_.resize(static_cast<std::size_t>(sz.QuadPart));
        std::size_t got = 0;
        while (got < owned_.size()) {
            DWORD n = 0;
            DWORD want = static_cast<DWORD>(std::min<std::size_t>(owned_.size() - got, 1u << 24));
            if (!ReadFile(f, owned_.data() + got, want, &n, nullptr) || n == 0) break;
            got += n;
        }
        CloseHandle(f);
        if (got != owned_.size()) { owned_.clear(); return false; }
        whole = owned_.data();
    }
    path_ = path;
    if (std::memcmp(whole, "regf", 4) != 0) { Close(); return false; }
    base_ = whole + kBaseBlock;
    len_ = static_cast<std::size_t>(sz.QuadPart) - kBaseBlock;
    root_ = Rd32(whole + 0x24);
    if (!IsNk(root_)) { Close(); return false; }
    return true;
}

bool Hive::OpenMemory(std::vector<std::uint8_t> bytes) {
    Close();
    if (bytes.size() < kBaseBlock + kHbinHeader || std::memcmp(bytes.data(), "regf", 4) != 0) return false;
    owned_ = std::move(bytes);
    base_ = owned_.data() + kBaseBlock;
    len_ = owned_.size() - kBaseBlock;
    root_ = Rd32(owned_.data() + 0x24);
    path_.clear();
    if (!IsNk(root_)) { Close(); return false; }
    return true;
}

const std::uint8_t* Hive::Cell(std::uint32_t off, std::uint32_t* size) const {
    if (!base_ || off == kNone || (off & 3u)) return nullptr;
    if (static_cast<std::uint64_t>(off) + 8 > len_) return nullptr;
    std::int32_t sl;
    std::memcpy(&sl, base_ + off, 4);
    if (sl >= 0) return nullptr;                       // free cell
    std::int64_t n = -static_cast<std::int64_t>(sl);
    if (n < 8 || static_cast<std::uint64_t>(off) + static_cast<std::uint64_t>(n) > len_) return nullptr;
    if (size) *size = static_cast<std::uint32_t>(n);
    return base_ + off;
}

bool Hive::IsNk(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    return p && sz >= 0x50 && p[4] == 'n' && p[5] == 'k';
}

std::uint16_t Hive::NkFlags(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    return (p && sz >= 0x50) ? Rd16(p + 6) : 0;
}

std::uint64_t Hive::NkTimestamp(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    return (p && sz >= 0x50) ? Rd64(p + 8) : 0;
}

std::wstring Hive::NkName(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    if (!p || sz < 0x50) return {};
    std::size_t n = Rd16(p + 0x4C);
    if (0x50 + n > sz) n = sz - 0x50;
    return (Rd16(p + 6) & 0x20) ? Latin1ToWide(p + 0x50, n) : Utf16ToWide(p + 0x50, n);
}

std::wstring Hive::NkClass(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    if (!p || sz < 0x50) return {};
    std::size_t n = Rd16(p + 0x4E);
    if (n == 0) return {};
    std::uint32_t csz;
    const std::uint8_t* c = Cell(Rd32(p + 0x34), &csz);
    if (!c) return {};
    if (4 + n > csz) n = csz - 4;
    return Utf16ToWide(c + 4, n);
}

std::uint32_t Hive::NkSubkeyCount(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    return (p && sz >= 0x50) ? Rd32(p + 0x18) : 0;
}

std::uint32_t Hive::NkValueCount(std::uint32_t nk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    return (p && sz >= 0x50) ? Rd32(p + 0x28) : 0;
}

bool Hive::ReadList(std::uint32_t list, int depth, std::vector<std::uint32_t>* out) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(list, &sz);
    if (!p || sz < 8) return false;
    std::size_t n = Rd16(p + 6);
    const char c0 = static_cast<char>(p[4]), c1 = static_cast<char>(p[5]);
    if (c0 == 'l' && (c1 == 'f' || c1 == 'h')) {
        if (8 + n * 8 > sz) return false;
        for (std::size_t i = 0; i < n; ++i) out->push_back(Rd32(p + 8 + i * 8));
    } else if (c0 == 'l' && c1 == 'i') {
        if (8 + n * 4 > sz) return false;
        for (std::size_t i = 0; i < n; ++i) out->push_back(Rd32(p + 8 + i * 4));
    } else if (c0 == 'r' && c1 == 'i') {
        if (depth >= 2 || 8 + n * 4 > sz) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (!ReadList(Rd32(p + 8 + i * 4), depth + 1, out)) return false;
    } else {
        return false;
    }
    return out->size() <= kMaxListEntries;
}

bool Hive::NkSubkeys(std::uint32_t nk, std::vector<std::uint32_t>* out) const {
    out->clear();
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    if (!p || sz < 0x50) return false;
    if (Rd32(p + 0x18) == 0) return true;
    return ReadList(Rd32(p + 0x20), 0, out);
}

std::uint32_t Hive::NkFindSubkey(std::uint32_t nk, std::wstring_view name) const {
    std::vector<std::uint32_t> kids;
    if (!NkSubkeys(nk, &kids)) return 0;
    for (std::uint32_t c : kids)
        if (IsNk(c) && EqualNoCase(NkName(c), name)) return c;
    return 0;
}

bool Hive::NkValues(std::uint32_t nk, std::vector<std::uint32_t>* out) const {
    out->clear();
    std::uint32_t sz;
    const std::uint8_t* p = Cell(nk, &sz);
    if (!p || sz < 0x50) return false;
    std::size_t n = Rd32(p + 0x28);
    if (n == 0) return true;
    std::uint32_t lsz;
    const std::uint8_t* l = Cell(Rd32(p + 0x2C), &lsz);
    if (!l || n > kMaxListEntries || 4 + n * 4 > lsz) return false;
    out->reserve(n);
    for (std::size_t i = 0; i < n; ++i) out->push_back(Rd32(l + 4 + i * 4));
    return true;
}

std::uint32_t Hive::NkFindValue(std::uint32_t nk, std::wstring_view name) const {
    std::vector<std::uint32_t> vks;
    if (!NkValues(nk, &vks)) return 0;
    for (std::uint32_t v : vks) {
        std::uint32_t sz;
        const std::uint8_t* p = Cell(v, &sz);
        if (!p || sz < 0x18 || p[4] != 'v' || p[5] != 'k') continue;
        if (EqualNoCase(VkName(v), name)) return v;
    }
    return 0;
}

std::wstring Hive::VkName(std::uint32_t vk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(vk, &sz);
    if (!p || sz < 0x18) return {};
    std::size_t n = Rd16(p + 6);
    if (0x18 + n > sz) n = sz - 0x18;
    return (Rd16(p + 0x14) & 1) ? Latin1ToWide(p + 0x18, n) : Utf16ToWide(p + 0x18, n);
}

std::uint32_t Hive::VkType(std::uint32_t vk) const {
    std::uint32_t sz;
    const std::uint8_t* p = Cell(vk, &sz);
    return (p && sz >= 0x18) ? Rd32(p + 0x10) : 0;
}

bool Hive::VkData(std::uint32_t vk, std::vector<std::uint8_t>* out) const {
    out->clear();
    std::uint32_t sz;
    const std::uint8_t* p = Cell(vk, &sz);
    if (!p || sz < 0x18) return false;
    std::uint32_t raw = Rd32(p + 8);
    if (raw & 0x80000000u) {
        std::uint32_t len = raw & 0x7FFFFFFFu;
        if (len > 4) return false;
        out->assign(p + 0xC, p + 0xC + len);
        return true;
    }
    std::size_t len = raw;
    if (len == 0) return true;
    if (len > kMaxValueLen) return false;
    std::uint32_t csz;
    const std::uint8_t* c = Cell(Rd32(p + 0xC), &csz);
    if (!c) return false;
    if (csz >= 0x10 && c[4] == 'd' && c[5] == 'b' && len > kMaxBlockData) {
        std::size_t nblocks = Rd16(c + 6);
        std::uint32_t lsz;
        const std::uint8_t* l = Cell(Rd32(c + 8), &lsz);
        if (l && nblocks > 0 && nblocks * kMaxBlockData >= len && 4 + nblocks * 4 <= lsz) {
            out->reserve(len);
            for (std::size_t i = 0; i < nblocks && out->size() < len; ++i) {
                std::uint32_t bsz;
                const std::uint8_t* blk = Cell(Rd32(l + 4 + i * 4), &bsz);
                if (!blk) { out->clear(); return false; }
                std::size_t take = std::min<std::size_t>(len - out->size(), bsz - 4);
                out->insert(out->end(), blk + 4, blk + 4 + take);
            }
            if (out->size() != len) { out->clear(); return false; }
            return true;
        }
    }
    if (len > csz - 4) return false;
    out->assign(c + 4, c + 4 + len);
    return true;
}

bool Hive::ToTree(std::uint32_t nk, Key* out, bool recurse) const {
    return ToTreeDepth(nk, out, recurse, 0);
}

bool Hive::ToTreeDepth(std::uint32_t nk, Key* out, bool recurse, int depth) const {
    if (!IsNk(nk) || depth > kMaxTreeDepth) return false;
    out->name = NkName(nk);
    out->class_name = NkClass(nk);
    out->timestamp = NkTimestamp(nk);
    out->symlink = (NkFlags(nk) & 0x10) != 0;
    std::vector<std::uint32_t> vks;
    NkValues(nk, &vks);
    for (std::uint32_t v : vks) {
        Value val;
        val.name = VkName(v);
        val.type = VkType(v);
        if (!VkData(v, &val.data)) continue;
        out->values.push_back(std::move(val));
    }
    if (recurse) {
        std::vector<std::uint32_t> kids;
        NkSubkeys(nk, &kids);
        for (std::uint32_t c : kids) {
            auto k = std::make_unique<Key>();
            if (ToTreeDepth(c, k.get(), true, depth + 1)) out->subkeys.push_back(std::move(k));
        }
    }
    return true;
}

}  // namespace win10xbox::registry::regf
