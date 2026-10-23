// The object manager namespace a guest sees: directory objects and symbolic links, looked up the way Windows'
// ObpLookupObjectName does, and the enumeration format of NtQueryDirectoryObject. Pure data structure: no syscalls,
// no host calls, so tests/unit can drive it directly. core/dispatch/nt_object.cpp binds it to the Nt* entry points
// and to the host (named synchronisation objects live in the host's own namespace, see docs/DISPATCHER.md
// "Object manager namespace").
//
// What the tree holds: directories, symbolic links and kernel-provided leaves (devices). A named event, mutex,
// semaphore, timer, section... is NOT a node: it is a host object whose host name encodes its guest path
// (nt_object.cpp), so that a child process, which is another host process, finds the same object by the same name.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace win10xbox::dispatch::detail::ob {

enum class Kind : std::uint8_t { Directory, SymbolicLink, Leaf };

struct Node;
using NodeRef = std::shared_ptr<Node>;

struct Node {
    Kind kind = Kind::Directory;
    std::wstring name;                 // as created; empty for an unnamed object
    std::wstring type_name;            // "Directory", "SymbolicLink", or the type of a Leaf ("Device")
    std::wstring target;               // SymbolicLink
    std::weak_ptr<Node> parent;        // empty for the root and for unnamed objects
    NodeRef fallback;                  // a directory whose misses continue in another one (\?? -> \GLOBAL??)
    std::vector<NodeRef> children;     // Directory
    std::uint32_t pins = 0;            // handles (and sync-object handles named below it) keeping a temporary node alive
    bool permanent = false;
    bool kernel = false;               // part of the layer's own layout, not created by a guest
    std::wstring tag;                  // kernel nodes: who provides them (ReplaceKernelLinks)
    std::uint64_t serial = 0;
};

// Standard access bits of the two object types the tree owns.
constexpr ACCESS_MASK kDirectoryQuery = 0x1, kDirectoryTraverse = 0x2, kDirectoryCreateObject = 0x4,
                      kDirectoryCreateSubdirectory = 0x8, kDirectoryAllAccess = 0xF000F;
constexpr ACCESS_MASK kSymlinkQuery = 0x1, kSymlinkAllAccess = 0xF0001;
constexpr ACCESS_MASK kDelete = 0x10000, kSynchronize = 0x100000, kMaximumAllowed = 0x02000000;
constexpr ULONG kObjCaseInsensitive = 0x40, kObjOpenIf = 0x80, kObjPermanent = 0x10, kObjOpenLink = 0x100;

// Windows statuses used here (own names: windows.h defines some STATUS_* as DWORD).
namespace status {
constexpr NTSTATUS Success = 0, MoreEntries = 0x105, NameExists = 0x40000000, NoMoreEntries = (NTSTATUS)0x8000001A,
                   InfoLengthMismatch = (NTSTATUS)0xC0000004, InvalidParameter = (NTSTATUS)0xC000000D,
                   AccessDenied = (NTSTATUS)0xC0000022, BufferTooSmall = (NTSTATUS)0xC0000023,
                   TypeMismatch = (NTSTATUS)0xC0000024, NameInvalid = (NTSTATUS)0xC0000033,
                   NameNotFound = (NTSTATUS)0xC0000034, NameCollision = (NTSTATUS)0xC0000035,
                   PathNotFound = (NTSTATUS)0xC000003A, PathSyntaxBad = (NTSTATUS)0xC000003B,
                   NoMemory = (NTSTATUS)0xC0000017;
}

// One row of a directory listing.
struct DirEntry {
    std::wstring name, type;
};

// Case-insensitive equality the way the kernel compares names (ordinal, system upcase table).
bool NameEquals(std::wstring_view a, std::wstring_view b, bool case_insensitive);

// Result of resolving a name.
struct Resolution {
    NTSTATUS status = status::Success;   // failure to resolve (the Nt* call returns it)
    NodeRef node;                        // the object found, or (leaf non-empty) the directory the leaf would live in
    std::wstring leaf;                   // final component, not present in the tree
    std::wstring dir_path;               // canonical path of `node` when `leaf` is set
    bool foreign = false;                // the first component is not ours: the name belongs to the host
    std::wstring rest;                   // foreign: the absolute name to continue with (a link may have been followed)
    bool Found() const { return status == status::Success && !foreign && leaf.empty() && node; }
    bool Leaf() const { return status == status::Success && !foreign && !leaf.empty(); }
};

class Tree {
public:
    Tree();

    // Builds the layer's own layout: \, \BaseNamedObjects (+ Global, Local, Session), \Sessions\<n>
    // (+ BaseNamedObjects, AppContainerNamedObjects[\<sid>]), \KernelObjects, \ObjectTypes, \??, \GLOBAL??,
    // \DosDevices. `ac_sid` empty: the host is not an AppContainer. Idempotent for the same arguments.
    void Populate(unsigned session, const std::wstring& ac_sid);
    unsigned Session() const { return session_; }

    // ObpLookupObjectName. `root` is the RootDirectory object (null for an absolute name). `follow_final`: a
    // symbolic link as the last component is followed (false when the caller asked for the link itself).
    Resolution Resolve(const NodeRef& root, std::wstring_view name, bool case_insensitive, bool follow_final);

    // Creates `leaf` under `dir` (as Resolve returned it) with one pin held by the caller. The name must be free.
    NodeRef CreateChild(const NodeRef& dir, std::wstring_view leaf, Kind kind, std::wstring_view target, bool permanent);
    // An object with no name: no parent, one pin.
    NodeRef CreateUnnamed(Kind kind, std::wstring_view target);
    // Layer-provided entries (the layout above, drive letters, devices). Replaces an existing one of that name.
    NodeRef AddKernelNode(std::wstring_view path, Kind kind, std::wstring_view type_or_target);
    void RemoveKernelNode(std::wstring_view path);
    // Removes every kernel-provided entry of directory `dir_path` whose `tag` matches (see AddKernelNode callers).
    void ReplaceKernelLinks(std::wstring_view dir_path, const std::vector<std::pair<std::wstring, std::wstring>>& links,
                            std::wstring_view tag);

    void Pin(const NodeRef& n);
    void Unpin(const NodeRef& n);          // drops a pin; a temporary node nobody holds is removed (and its parents)
    void MakePermanent(const NodeRef& n, bool permanent);
    // Canonical path ("\Sessions\1\BaseNamedObjects"); empty for an unnamed node.
    std::wstring PathOf(const NodeRef& n);
    std::wstring Target(const NodeRef& n);
    // Children of a directory (name, type), in a stable order.
    std::vector<DirEntry> List(const NodeRef& dir);
    NodeRef Root();
    std::uint64_t Count(Kind k);           // nodes of that kind that exist (for NtQueryObject type information)

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    unsigned session_ = 1;
};

// NtQueryDirectoryObject over a list: fills `buffer` with OBJECT_DIRECTORY_INFORMATION rows followed by an all-zero
// row and the strings, exactly as Windows lays them out, and returns what the syscall returns. `context` is the
// caller's resume index (read unless `restart`, written on success).
// `user_base`, when given, is the address the finished buffer will have in the caller (the rows are built in `buffer`,
// a private copy, and copied out); `*written` receives how many bytes of `buffer` the call filled in.
NTSTATUS PackDirectoryEntries(const std::vector<DirEntry>& entries, void* buffer, ULONG size, bool single, bool restart,
                              ULONG* context, ULONG* ret_size, void* user_base = nullptr, ULONG* written = nullptr);

std::wstring ParentPath(std::wstring_view path);   // "\\a\\b" -> "\\a", "\\a" -> "\\", no parent -> ""
std::wstring LeafOf(std::wstring_view path);

// The host name of a named synchronisation object (event, mutex, section...). The object lives in the host's own
// named-object directory under one name that encodes its whole guest path, `<prefix>!<escaped path>`: '\\' becomes
// '|', '|' and '%' are percent-escaped, so the guest path is recovered exactly (for NtQueryObject names and for
// directory listings answered from the host). A path whose escaped form is longer than kMaxPlainName is named
// `<prefix>#<64-bit hash of the upper-cased path>`; the caller keeps the path for those.
constexpr std::size_t kMaxPlainName = 200;
std::wstring MangleName(std::wstring_view prefix, std::wstring_view guest_path, bool* hashed = nullptr);
// False when `host_leaf` is not one of ours. For a hashed name `*guest_path` receives the hash digits and `*hashed`
// is true.
bool DemangleName(std::wstring_view prefix, std::wstring_view host_leaf, std::wstring* guest_path, bool* hashed = nullptr);

}  // namespace win10xbox::dispatch::detail::ob
