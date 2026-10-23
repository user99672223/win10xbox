// The object manager namespace (core/dispatch/ob_namespace.cpp, nt_object.cpp): the tree and its lookup rules, the
// OBJECT_DIRECTORY_INFORMATION layout NtQueryDirectoryObject fills, the host names of named objects, and the
// dispatcher entry points on top of them (directory objects, symbolic links, a named event listed in its directory).
#include "check.h"
#include "win10xbox/dispatch.h"
#include "../../core/dispatch/internal.h"
#include "../../core/dispatch/ob_namespace.h"
#include <windows.h>
#include <winternl.h>
#include <cstring>
#include <string>
#include <vector>

using namespace win10xbox::dispatch;
using namespace win10xbox::dispatch::detail;
namespace ob = win10xbox::dispatch::detail::ob;

namespace {

constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kNameExists = 0x40000000;
constexpr NTSTATUS kMoreEntries = 0x105;
constexpr NTSTATUS kNoMoreEntries = static_cast<NTSTATUS>(0x8000001A);
constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS kAccessViolation = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kInvalidParameter = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kAccessDenied = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS kTypeMismatch = static_cast<NTSTATUS>(0xC0000024);
constexpr NTSTATUS kNameInvalid = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS kNameNotFound = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS kNameCollision = static_cast<NTSTATUS>(0xC0000035);
constexpr NTSTATUS kPathNotFound = static_cast<NTSTATUS>(0xC000003A);
constexpr NTSTATUS kPathSyntaxBad = static_cast<NTSTATUS>(0xC000003B);

struct Row { UNICODE_STRING Name, TypeName; };   // OBJECT_DIRECTORY_INFORMATION

std::wstring Str(const UNICODE_STRING& u) { return std::wstring(u.Buffer, u.Length / sizeof(wchar_t)); }

// ---- the tree -------------------------------------------------------------------------------------------------------

TEST(ob_layout_exists) {
    ob::Tree t;
    t.Populate(3, L"");
    for (const wchar_t* p : {L"\\", L"\\BaseNamedObjects", L"\\Sessions", L"\\Sessions\\3", L"\\Sessions\\3\\BaseNamedObjects",
                             L"\\Sessions\\3\\AppContainerNamedObjects", L"\\KernelObjects", L"\\ObjectTypes", L"\\??",
                             L"\\GLOBAL??"}) {
        ob::Resolution r = t.Resolve(nullptr, p, true, true);
        CHECK_MSG(r.Found() && r.node->kind == ob::Kind::Directory, "%ls", p);
    }
    // \DosDevices is a link to \??; Global / Local / Session links exist in both BaseNamedObjects directories.
    ob::Resolution dos = t.Resolve(nullptr, L"\\DosDevices", true, false);
    CHECK(dos.Found() && dos.node->kind == ob::Kind::SymbolicLink && dos.node->target == L"\\??");
    ob::Resolution viaLink = t.Resolve(nullptr, L"\\DosDevices", true, true);
    CHECK(viaLink.Found() && t.PathOf(viaLink.node) == L"\\??");
    ob::Resolution local = t.Resolve(nullptr, L"\\Sessions\\3\\BaseNamedObjects\\Local", true, false);
    CHECK(local.Found() && local.node->target == L"\\Sessions\\3\\BaseNamedObjects");
    ob::Resolution global = t.Resolve(nullptr, L"\\BaseNamedObjects\\Global", true, false);
    CHECK(global.Found() && global.node->target == L"\\BaseNamedObjects");
    ob::Resolution bno = t.Resolve(nullptr, L"\\BaseNamedObjects\\Session\\3", true, true);
    CHECK(bno.Found() && t.PathOf(bno.node) == L"\\Sessions\\3\\BaseNamedObjects");
}

TEST(ob_appcontainer_directory) {
    ob::Tree t;
    t.Populate(2, L"S-1-15-2-1-2-3-4-5-6-7");
    ob::Resolution r = t.Resolve(nullptr, L"\\Sessions\\2\\AppContainerNamedObjects\\S-1-15-2-1-2-3-4-5-6-7", true, true);
    CHECK(r.Found() && r.node->kind == ob::Kind::Directory);
    ob::Tree plain;
    plain.Populate(2, L"");
    CHECK(plain.Resolve(nullptr, L"\\Sessions\\2\\AppContainerNamedObjects\\S-1-15-2-1-2-3-4-5-6-7", true, true).Leaf());
}

TEST(ob_lookup_statuses) {
    ob::Tree t;
    t.Populate(1, L"");
    auto status = [&](const wchar_t* name, bool ci = true) { return t.Resolve(nullptr, name, ci, true).status; };
    CHECK_EQ(status(L"BaseNamedObjects"), kPathSyntaxBad);
    CHECK_EQ(status(L""), kPathSyntaxBad);
    CHECK_EQ(status(L"\\BaseNamedObjects\\"), kNameInvalid);
    CHECK_EQ(status(L"\\\\BaseNamedObjects"), kNameInvalid);
    CHECK_EQ(status(L"\\BaseNamedObjects\\\\om.c-test"), kNameInvalid);
    CHECK_EQ(status(L"\\BaseNamedObjects\\om.c-test\\"), kPathNotFound);
    CHECK_EQ(status(L"\\BASENamedObjects\\test", true), kSuccess);
    CHECK(t.Resolve(nullptr, L"\\BASENamedObjects\\test", false, true).foreign);   // case-sensitive unless OBJ_CASE_INSENSITIVE: not ours
    CHECK_EQ(status(L"\\BasedNamedObjects\\Local\\x\\y"), kSuccess);       // a miss in the root belongs to the host...
    CHECK(t.Resolve(nullptr, L"\\BasedNamedObjects\\Local\\x\\y", true, true).foreign);
    // ...but a miss deeper in is ours.
    CHECK_EQ(status(L"\\BaseNamedObjects\\nope\\x"), kPathNotFound);
    ob::Resolution leaf = t.Resolve(nullptr, L"\\Sessions\\1\\BaseNamedObjects\\Local\\evt", true, true);
    CHECK(leaf.Leaf());
    CHECK_EQ(leaf.leaf, std::wstring(L"evt"));
    CHECK_EQ(leaf.dir_path, std::wstring(L"\\Sessions\\1\\BaseNamedObjects"));   // Local is this session's directory
    ob::Resolution global_leaf = t.Resolve(nullptr, L"\\Sessions\\1\\BaseNamedObjects\\Global\\evt", true, true);
    CHECK_EQ(global_leaf.dir_path, std::wstring(L"\\BaseNamedObjects"));             // Global is the machine-wide one
}

TEST(ob_relative_to_a_root) {
    ob::Tree t;
    t.Populate(1, L"");
    ob::NodeRef bno = t.Resolve(nullptr, L"\\BaseNamedObjects", true, true).node;
    CHECK(t.Resolve(bno, L"", true, true).Found());                              // empty name: the root itself
    CHECK_EQ(t.Resolve(bno, L"\\x", true, true).status, kPathSyntaxBad);          // rooted name under a root handle
    CHECK_EQ(t.Resolve(bno, L"om.c-test\\", true, true).status, kPathNotFound);
    ob::NodeRef link = t.Resolve(nullptr, L"\\BaseNamedObjects\\Local", true, false).node;
    CHECK_EQ(t.Resolve(link, L"x", true, true).status, kTypeMismatch);            // a link is no directory
    CHECK(t.Resolve(link, L"", true, true).Found());                              // but an empty name is the link itself
}

TEST(ob_create_prune_and_pins) {
    ob::Tree t;
    t.Populate(1, L"");
    ob::NodeRef bno = t.Resolve(nullptr, L"\\BaseNamedObjects", true, true).node;
    ob::NodeRef a = t.CreateChild(bno, L"a", ob::Kind::Directory, L"", false);
    CHECK(a != nullptr);
    CHECK(t.CreateChild(bno, L"a", ob::Kind::Directory, L"", false) == nullptr);   // the name is taken
    ob::NodeRef b = t.CreateChild(a, L"b", ob::Kind::Directory, L"", false);
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\a\\b", true, true).Found());
    t.Unpin(a);                                    // a still has a child: stays
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\a", true, true).Found());
    t.Unpin(b);                                    // the last handle of the child: both go
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\a", true, true).Leaf());
    ob::NodeRef perm = t.CreateChild(bno, L"perm", ob::Kind::Directory, L"", true);
    t.Unpin(perm);
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\perm", true, true).Found());   // permanent: stays
    t.MakePermanent(perm, false);
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\perm", true, true).Leaf());    // made temporary, nobody holds it
}

TEST(ob_symbolic_links) {
    ob::Tree t;
    t.Populate(1, L"");
    ob::NodeRef bno = t.Resolve(nullptr, L"\\BaseNamedObjects", true, true).node;
    ob::NodeRef dir = t.CreateChild(bno, L"d", ob::Kind::Directory, L"", false);
    ob::NodeRef link = t.CreateChild(bno, L"l", ob::Kind::SymbolicLink, L"\\BaseNamedObjects\\d", false);
    ob::Resolution through = t.Resolve(nullptr, L"\\BaseNamedObjects\\l\\evt", true, true);
    CHECK(through.Leaf() && through.dir_path == L"\\BaseNamedObjects\\d");
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\l", true, true).node == dir);       // followed
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\l", true, false).node == link);     // asked for the link itself
    // A loop ends instead of spinning.
    ob::NodeRef x = t.CreateChild(bno, L"x", ob::Kind::SymbolicLink, L"\\BaseNamedObjects\\y", false);
    ob::NodeRef y = t.CreateChild(bno, L"y", ob::Kind::SymbolicLink, L"\\BaseNamedObjects\\x", false);
    CHECK(t.Resolve(nullptr, L"\\BaseNamedObjects\\x\\z", true, true).status != kSuccess);
    // Where a link leads out of the tree the caller gets the name to continue with.
    t.AddKernelNode(L"\\GLOBAL??\\NUL", ob::Kind::SymbolicLink, L"\\Device\\Null");
    ob::Resolution out = t.Resolve(nullptr, L"\\??\\NUL", true, true);          // \?? continues in \GLOBAL??
    CHECK(out.foreign);
    CHECK_EQ(out.rest, std::wstring(L"\\Device\\Null"));
    (void)dir; (void)link; (void)x; (void)y;
}

// ---- NtQueryDirectoryObject's buffer --------------------------------------------------------------------------------------

std::vector<ob::DirEntry> TwoMutants() { return {{L"Telamon", L"Mutant"}, {L"Oileus", L"Mutant"}}; }

TEST(ob_directory_layout_single_entry) {
    alignas(8) char buffer[200];
    std::memset(buffer, 0xcc, sizeof buffer);
    ULONG context = 0xdeadbeef, ret = 0xdeadbeef;
    NTSTATUS s = ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, true, &context, &ret);
    CHECK_EQ(s, kSuccess);
    CHECK_EQ(context, 1ul);
    auto* rows = reinterpret_cast<Row*>(buffer);
    CHECK_EQ(Str(rows[0].Name), std::wstring(L"Telamon"));
    CHECK_EQ(Str(rows[0].TypeName), std::wstring(L"Mutant"));
    CHECK_EQ(rows[0].Name.MaximumLength, static_cast<USHORT>(rows[0].Name.Length + 2));
    CHECK_EQ(rows[0].TypeName.MaximumLength, static_cast<USHORT>(rows[0].TypeName.Length + 2));
    // The array is ended by an all-zero row; the strings follow it, NUL-terminated, name then type.
    Row zero{};
    CHECK(std::memcmp(&rows[1], &zero, sizeof zero) == 0);
    CHECK(reinterpret_cast<char*>(rows[0].Name.Buffer) == buffer + 2 * sizeof(Row));
    CHECK(rows[0].Name.Buffer[7] == 0);
    CHECK(reinterpret_cast<char*>(rows[0].TypeName.Buffer) == buffer + 2 * sizeof(Row) + 16);
    CHECK_EQ(ret, static_cast<ULONG>(2 * sizeof(Row) + 16 + 14));
}

TEST(ob_directory_layout_resume_and_end) {
    alignas(8) char buffer[200];
    ULONG context = 0, ret = 0;
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, true, &context, &ret), kSuccess);
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, false, &context, &ret), kSuccess);
    CHECK_EQ(context, 2ul);
    CHECK_EQ(Str(reinterpret_cast<Row*>(buffer)[0].Name), std::wstring(L"Oileus"));
    ret = 0xdeadbeef;
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, false, &context, &ret), kNoMoreEntries);
    CHECK_EQ(context, 2ul);                               // untouched
    CHECK_EQ(ret, static_cast<ULONG>(sizeof(Row)));
    Row zero{};
    CHECK(std::memcmp(buffer, &zero, sizeof zero) == 0);   // the terminator row is written even when there is nothing
    // Restart resets the index.
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, true, &context, &ret), kSuccess);
    CHECK_EQ(context, 1ul);
}

TEST(ob_directory_layout_all_entries) {
    alignas(8) char buffer[400];
    ULONG context = 0, ret = 0;
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, false, true, &context, &ret), kSuccess);
    CHECK_EQ(context, 2ul);
    auto* rows = reinterpret_cast<Row*>(buffer);
    CHECK_EQ(Str(rows[0].Name), std::wstring(L"Telamon"));
    CHECK_EQ(Str(rows[1].Name), std::wstring(L"Oileus"));
    Row zero{};
    CHECK(std::memcmp(&rows[2], &zero, sizeof zero) == 0);
    // 3 rows, then each name and type with its NUL: Telamon 16, Mutant 14, Oileus 14, Mutant 14.
    CHECK_EQ(ret, static_cast<ULONG>(3 * sizeof(Row) + 16 + 14 + 14 + 14));
}

TEST(ob_directory_layout_short_buffers) {
    alignas(8) char buffer[400];
    ULONG context = 0xdeadbeef, ret = 0;
    // One entry needs 2 rows + the strings: a smaller buffer answers STATUS_BUFFER_TOO_SMALL with the size it needs.
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof buffer, true, true, &context, &ret), kSuccess);
    const ULONG needed = ret;
    context = 0xdeadbeef;
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, 0, true, true, &context, &ret), kBufferTooSmall);
    CHECK_EQ(ret, needed);
    CHECK_EQ(context, 0xdeadbeeful);
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, needed - 1, true, true, &context, &ret), kBufferTooSmall);
    CHECK_EQ(ret, needed);
    // Asking for all of them with room for one: the first is returned, STATUS_MORE_ENTRIES says there are more.
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, needed, false, true, &context, &ret), kMoreEntries);
    CHECK_EQ(context, 1ul);
    // Nothing fits at all.
    context = 0xdeadbeef;
    CHECK_EQ(ob::PackDirectoryEntries(TwoMutants(), buffer, sizeof(Row) + 4, false, true, &context, &ret), kMoreEntries);
    CHECK_EQ(context, 0ul);
}

TEST(ob_directory_layout_empty) {
    alignas(8) char buffer[64];
    std::memset(buffer, 0xcc, sizeof buffer);
    ULONG context = 0xdeadbeef, ret = 0xdeadbeef;
    CHECK_EQ(ob::PackDirectoryEntries({}, buffer, sizeof buffer, true, true, &context, &ret), kNoMoreEntries);
    CHECK_EQ(context, 0xdeadbeeful);
    CHECK_EQ(ret, static_cast<ULONG>(sizeof(Row)));
    Row zero{};
    CHECK(std::memcmp(buffer, &zero, sizeof zero) == 0);
    ret = 0xdeadbeef;
    CHECK_EQ(ob::PackDirectoryEntries({}, buffer, 0, false, true, &context, &ret), kNoMoreEntries);   // no room for the row
    CHECK_EQ(ret, static_cast<ULONG>(sizeof(Row)));
}

// ---- host names ----------------------------------------------------------------------------------------------------------------

TEST(ob_host_names_round_trip) {
    const std::wstring prefix = L"W10X0123456789abcdef";
    for (const wchar_t* path : {L"\\Sessions\\1\\BaseNamedObjects\\evt", L"\\BaseNamedObjects\\a|b%c\\d", L"\\BaseNamedObjects\\x%7Cy",
                                L"\\BaseNamedObjects\\with space"}) {
        bool hashed = true;
        std::wstring host = ob::MangleName(prefix, path, &hashed);
        CHECK(!hashed);
        CHECK(host.find(L'\\') == std::wstring::npos);   // a host object name has no separator
        std::wstring back;
        CHECK(ob::DemangleName(prefix, host, &back, &hashed));
        CHECK_EQ(back, std::wstring(path));
    }
    // Not ours: another layer, or something unrelated.
    std::wstring ignored;
    CHECK(!ob::DemangleName(prefix, L"W10X0000000000000000!x", &ignored));
    CHECK(!ob::DemangleName(prefix, L"Local_thing", &ignored));
    // A long path becomes a fixed-length hash that is stable and case-insensitive.
    std::wstring long_path = L"\\BaseNamedObjects\\" + std::wstring(5000, L'a');
    bool hashed = false;
    std::wstring h1 = ob::MangleName(prefix, long_path, &hashed);
    CHECK(hashed);
    CHECK(h1.size() <= prefix.size() + 17);
    std::wstring upper = L"\\BaseNamedObjects\\" + std::wstring(5000, L'A');
    CHECK(ob::MangleName(prefix, upper, nullptr) == h1);
    std::wstring other = L"\\BaseNamedObjects\\" + std::wstring(4999, L'a') + L"b";
    CHECK(ob::MangleName(prefix, other, nullptr) != h1);
    std::wstring digits;
    CHECK(ob::DemangleName(prefix, h1, &digits, &hashed) && hashed && digits.size() == 16);
}

// ---- through the dispatcher entry points -----------------------------------------------------------------------------------------

using FnCreateDir = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using FnQueryDir = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
using FnCreateLink = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);
using FnQueryLink = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);
using FnCreateEvent = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
using FnQueryObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using FnClose = NTSTATUS(NTAPI*)(HANDLE);

template <class Fn> Fn Impl(const char* name) {
    Initialize();
    const Entry* e = LookupByName(name);
    return e ? reinterpret_cast<Fn>(e->impl) : nullptr;
}

struct Name {
    std::wstring text;
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    Name(const std::wstring& t, HANDLE root = nullptr, ULONG attributes = 0x40 /*OBJ_CASE_INSENSITIVE*/) : text(t) {
        us.Buffer = text.data();
        us.Length = static_cast<USHORT>(text.size() * 2);
        us.MaximumLength = static_cast<USHORT>(text.size() * 2 + 2);
        oa = {};
        oa.Length = sizeof oa;
        oa.RootDirectory = root;
        oa.ObjectName = &us;
        oa.Attributes = attributes;
    }
};

TEST(ob_entry_points_directories_and_links) {
    auto create_dir = Impl<FnCreateDir>("NtCreateDirectoryObject");
    auto open_dir = Impl<FnCreateDir>("NtOpenDirectoryObject");
    auto create_link = Impl<FnCreateLink>("NtCreateSymbolicLinkObject");
    auto open_link = Impl<FnCreateDir>("NtOpenSymbolicLinkObject");
    auto query_link = Impl<FnQueryLink>("NtQuerySymbolicLinkObject");
    auto close = Impl<FnClose>("NtClose");
    CHECK(create_dir && open_dir && create_link && open_link && query_link && close);
    if (!create_dir || !open_dir || !create_link || !open_link || !query_link || !close) return;

    Name base(L"\\BaseNamedObjects\\w10x-unit-dir");
    HANDLE dir = nullptr, again = nullptr;
    CHECK_EQ(create_dir(&dir, 0x1F000F, &base.oa), kSuccess);
    CHECK(dir != nullptr);
    CHECK_EQ(create_dir(&again, 0x1F000F, &base.oa), kNameCollision);
    CHECK(again == nullptr);
    base.oa.Attributes |= 0x80;   // OBJ_OPENIF
    CHECK_EQ(create_dir(&again, 0x1F000F, &base.oa), kNameExists);
    CHECK(again != nullptr);
    close(again);
    base.oa.Attributes &= ~0x80u;
    CHECK_EQ(open_dir(&again, 0, &base.oa), kAccessDenied);                // an existing directory, no access asked
    // Opened relative to itself, the empty name is the directory.
    Name self(L"", dir);
    CHECK_EQ(open_dir(&again, 1, &self.oa), kSuccess);
    close(again);
    // A directory is no symbolic link, and the other way round.
    CHECK_EQ(open_link(&again, 1, &base.oa), kTypeMismatch);
    Name missing(L"\\BaseNamedObjects\\w10x-unit-dir\\nope");
    CHECK_EQ(open_dir(&again, 1, &missing.oa), kNameNotFound);
    Name deeper(L"\\BaseNamedObjects\\w10x-unit-dir\\nope\\deeper");
    CHECK_EQ(open_dir(&again, 1, &deeper.oa), kPathNotFound);
    Name trailing(L"\\BaseNamedObjects\\");
    CHECK_EQ(open_dir(&again, 1, &trailing.oa), kNameInvalid);

    // A link made in it, queried with the Windows buffer rules.
    UNICODE_STRING target;
    std::wstring target_text = L"\\BaseNamedObjects";
    target.Buffer = target_text.data();
    target.Length = static_cast<USHORT>(target_text.size() * 2);
    target.MaximumLength = static_cast<USHORT>(target.Length + 2);
    Name link_name(L"l", dir);
    HANDLE link = nullptr;
    CHECK_EQ(create_link(&link, 1, &link_name.oa, &target), kSuccess);
    wchar_t buf[64];
    UNICODE_STRING out{0, sizeof buf, buf};
    ULONG len = 0xdeadbeef;
    CHECK_EQ(query_link(link, &out, &len), kSuccess);
    CHECK_EQ(len, static_cast<ULONG>(target.Length + 2));
    CHECK_EQ(Str(out), target_text);
    out.MaximumLength = target.Length;                     // no room for the terminating NUL
    out.Length = 0x4444;
    len = 0xdeadbeef;
    CHECK_EQ(query_link(link, &out, &len), kBufferTooSmall);
    CHECK_EQ(len, static_cast<ULONG>(target.Length + 2));
    CHECK_EQ(out.Length, static_cast<USHORT>(0x4444));
    target.MaximumLength = 0;
    CHECK_EQ(create_link(&again, 1, &link_name.oa, &target), kInvalidParameter);
    target.MaximumLength = static_cast<USHORT>(target.Length + 2);
    CHECK_EQ(create_link(&again, 1, nullptr, &target), kAccessViolation);
    // A link followed in the middle of a name.
    Name via(L"\\BaseNamedObjects\\w10x-unit-dir\\l\\w10x-unit-dir");
    CHECK_EQ(open_dir(&again, 1, &via.oa), kSuccess);
    close(again);
    close(link);
    close(dir);
    // Nobody holds the directory any more: it is gone.
    CHECK_EQ(open_dir(&again, 1, &base.oa), kNameNotFound);
}

TEST(ob_entry_points_enumerate_named_event) {
    auto create_dir = Impl<FnCreateDir>("NtCreateDirectoryObject");
    auto query_dir = Impl<FnQueryDir>("NtQueryDirectoryObject");
    auto create_event = Impl<FnCreateEvent>("NtCreateEvent");
    auto query_object = Impl<FnQueryObject>("NtQueryObject");
    auto close = Impl<FnClose>("NtClose");
    CHECK(create_dir && query_dir && create_event && query_object && close);
    if (!create_dir || !query_dir || !create_event || !query_object || !close) return;

    Name dir_name(L"\\BaseNamedObjects\\w10x-unit-enum");
    HANDLE dir = nullptr, ev = nullptr, ev2 = nullptr;
    CHECK_EQ(create_dir(&dir, 0x1F000F, &dir_name.oa), kSuccess);
    alignas(8) char buffer[512];
    ULONG context = 0xdeadbeef, ret = 0;
    CHECK_EQ(query_dir(dir, buffer, sizeof buffer, TRUE, TRUE, &context, &ret), kNoMoreEntries);
    CHECK_EQ(context, 0xdeadbeeful);

    Name one(L"first", dir), two(L"second", dir);
    CHECK_EQ(create_event(&ev, 0x1F0003, &one.oa, 0, FALSE), kSuccess);
    CHECK_EQ(create_event(&ev2, 0x1F0003, &two.oa, 1, FALSE), kSuccess);
    // The host object carries the layer's name, not the guest's; NtQueryObject says the guest's.
    alignas(8) char name_buf[512];
    CHECK_EQ(query_object(ev, 1, name_buf, sizeof name_buf, &ret), kSuccess);
    const std::wstring expected = L"\\BaseNamedObjects\\w10x-unit-enum\\first";
    CHECK_EQ(Str(*reinterpret_cast<UNICODE_STRING*>(name_buf)), expected);
    CHECK_EQ(ret, static_cast<ULONG>(sizeof(UNICODE_STRING) + expected.size() * 2 + 2));
    CHECK_EQ(query_object(ev, 1, name_buf, sizeof(UNICODE_STRING), &ret), kInfoLengthMismatch);

    context = 0;
    CHECK_EQ(query_dir(dir, buffer, sizeof buffer, FALSE, TRUE, &context, &ret), kSuccess);
    CHECK_EQ(context, 2ul);
    auto* rows = reinterpret_cast<Row*>(buffer);
    CHECK_EQ(Str(rows[0].Name), std::wstring(L"first"));
    CHECK_EQ(Str(rows[0].TypeName), std::wstring(L"Event"));
    CHECK_EQ(Str(rows[1].Name), std::wstring(L"second"));
    Row zero{};
    CHECK(std::memcmp(&rows[2], &zero, sizeof zero) == 0);
    // A bad context pointer is an access violation, whatever the restart flag says.
    CHECK_EQ(query_dir(dir, buffer, sizeof buffer, TRUE, TRUE, nullptr, &ret), kAccessViolation);

    close(ev);
    close(ev2);
    close(dir);
}

TEST(ob_links_registered_by_other_modules) {
    auto open_link = Impl<FnCreateDir>("NtOpenSymbolicLinkObject");
    auto open_dir = Impl<FnCreateDir>("NtOpenDirectoryObject");
    auto query_link = Impl<FnQueryLink>("NtQuerySymbolicLinkObject");
    auto query_dir = Impl<FnQueryDir>("NtQueryDirectoryObject");
    auto close = Impl<FnClose>("NtClose");
    CHECK(open_link && open_dir && query_link && query_dir && close);
    if (!open_link || !open_dir || !query_link || !query_dir || !close) return;

    // The driver host mirrors the DOS-device links drivers create.
    RegisterNamespaceLink(L"\\DosDevices\\W10xUnitDevice", L"\\Device\\W10xUnitDevice");
    Name link(L"\\??\\W10xUnitDevice");
    HANDLE h = nullptr;
    CHECK_EQ(open_link(&h, 1, &link.oa), kSuccess);
    wchar_t buf[64];
    UNICODE_STRING out{0, sizeof buf, buf};
    ULONG len = 0;
    CHECK_EQ(query_link(h, &out, &len), kSuccess);
    CHECK_EQ(Str(out), std::wstring(L"\\Device\\W10xUnitDevice"));
    close(h);

    Name dos(L"\\??");
    HANDLE dir = nullptr;
    CHECK_EQ(open_dir(&dir, 1, &dos.oa), kSuccess);
    alignas(8) char buffer[16384];
    ULONG context = 0, ret = 0;
    CHECK_EQ(query_dir(dir, buffer, sizeof buffer, FALSE, TRUE, &context, &ret), kSuccess);
    bool listed = false;
    for (auto* r = reinterpret_cast<Row*>(buffer); r->Name.Buffer; ++r)
        listed = listed || (Str(r->Name) == L"W10xUnitDevice" && Str(r->TypeName) == L"SymbolicLink");
    CHECK(listed);
    close(dir);

    UnregisterNamespaceLink(L"\\DosDevices\\W10xUnitDevice");
    CHECK_EQ(open_link(&h, 1, &link.oa), kNameNotFound);
}

}  // namespace
