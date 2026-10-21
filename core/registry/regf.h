// REGF registry hive file support (read-only reader + whole-hive writer).
//
// Self-contained: depends only on <windows.h> and the C++ standard library so
// it is also compiled into the fixture generator (machine/hivex/mkfixture).
// Implements the documented REGF format: base block, hbin pages, and the
// nk / vk / lf / lh / li / ri / sk / db cells. Nothing here is derived from
// hivex source; hivex's lib/hivex-internal.h was only consulted as one of
// several format references. See machine/hivex/README.md.
//
// The reader never writes to the file it opens. All cell accesses are bounds
// checked so corrupt or hostile hives fail soft (empty/false) rather than crash.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::registry::regf {

// Registry value/key names compare case-insensitively (ordinal, upper-case
// folded). ASCII fast path, LCMapStringEx for the rest.
wchar_t UpcaseChar(wchar_t c);
std::wstring Upcase(std::wstring_view s);
bool EqualNoCase(std::wstring_view a, std::wstring_view b);

// In-memory tree used for writing hives and for snapshot/import of subtrees.
struct Value {
    std::wstring name;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
};
struct Key {
    std::wstring name;
    std::wstring class_name;
    std::uint64_t timestamp = 0;          // FILETIME
    bool symlink = false;                 // nk flag 0x10
    std::vector<Value> values;
    std::vector<std::unique_ptr<Key>> subkeys;
    Key* AddKey(std::wstring n);
    Value& AddValue(std::wstring n, std::uint32_t type, std::vector<std::uint8_t> data);
    Value& AddString(std::wstring n, std::wstring_view s, std::uint32_t type = 1 /*REG_SZ*/);
    Value& AddDword(std::wstring n, std::uint32_t v);
};

// Serialises a key tree as a complete REGF file (version 1.5, one lh/ri list
// per key, a shared sk security cell, db records for values > 16344 bytes).
// Subkeys are written sorted by upper-cased name; values keep their order.
// Deterministic: the same tree always produces the same bytes.
bool WriteHive(const Key& root, std::wstring_view hive_name, std::vector<std::uint8_t>* out);

// Read-only hive. Offsets ("cells") are relative to the first hbin (file+0x1000)
// exactly as stored in the file; 0 / 0xFFFFFFFF mean "none".
class Hive {
public:
    Hive();
    ~Hive();
    Hive(const Hive&) = delete;
    Hive& operator=(const Hive&) = delete;

    bool Open(const std::wstring& path);             // maps (or reads) the file read-only
    bool OpenMemory(std::vector<std::uint8_t> bytes);
    const std::wstring& Path() const { return path_; }
    std::uint32_t Root() const { return root_; }

    // nk records
    bool IsNk(std::uint32_t nk) const;
    std::uint16_t NkFlags(std::uint32_t nk) const;
    std::uint64_t NkTimestamp(std::uint32_t nk) const;
    std::wstring NkName(std::uint32_t nk) const;
    std::wstring NkClass(std::uint32_t nk) const;
    std::uint32_t NkSubkeyCount(std::uint32_t nk) const;
    std::uint32_t NkValueCount(std::uint32_t nk) const;
    bool NkSubkeys(std::uint32_t nk, std::vector<std::uint32_t>* out) const;   // stored order
    std::uint32_t NkFindSubkey(std::uint32_t nk, std::wstring_view name) const; // 0 if absent
    bool NkValues(std::uint32_t nk, std::vector<std::uint32_t>* out) const;    // vk offsets, stored order
    std::uint32_t NkFindValue(std::uint32_t nk, std::wstring_view name) const; // 0 if absent

    // vk records
    std::wstring VkName(std::uint32_t vk) const;
    std::uint32_t VkType(std::uint32_t vk) const;
    bool VkData(std::uint32_t vk, std::vector<std::uint8_t>* out) const;

    // Deep copy of a key (and optionally its subtree) into an in-memory tree.
    bool ToTree(std::uint32_t nk, Key* out, bool recurse = true) const;

private:
    const std::uint8_t* Cell(std::uint32_t off, std::uint32_t* size) const;
    bool ReadList(std::uint32_t list, int depth, std::vector<std::uint32_t>* out) const;
    bool ToTreeDepth(std::uint32_t nk, Key* out, bool recurse, int depth) const;
    void Close();

    const std::uint8_t* base_ = nullptr;   // first hbin (file + 0x1000)
    std::size_t len_ = 0;                  // bytes from base_
    std::uint32_t root_ = 0;
    std::wstring path_;
    std::vector<std::uint8_t> owned_;      // when read() rather than mapped
    void* file_ = nullptr;                 // HANDLE
    void* map_ = nullptr;                  // HANDLE
    const void* view_ = nullptr;
};

}  // namespace win10xbox::registry::regf
