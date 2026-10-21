// Export table of the user-mode ntoskrnl/hal and the import binder: how a
// driver's `import ntoskrnl.exe!Name` becomes an address.
//
//   1. an implemented export (drivers/ntoskrnl, drivers/kmdf)
//   2. Zw*/Nt*: the dispatcher's implementation of the same syscall (so a
//      driver sees the emulated registry, VFS, clock ...), else the host ntdll
//   3. a Rtl*/CRT function forwarded to the host ntdll (pure functions only)
//   4. a loud stub: traces FIXME and returns STATUS_NOT_IMPLEMENTED
#include "internal.h"
#include "win10xbox/dispatch.h"
#include <atomic>
#include <deque>
#include <map>
#include <unordered_map>

namespace win10xbox::drivers::detail {

extern const char* const kHostForwards[];
extern const char* const kHostAliases[][2];

namespace {

using ExportMap = std::unordered_map<std::string, void*>;

ExportMap BuildMap() {
    ExportMap m;
    const ExportGroup groups[] = {KeExports(), ExExports(), IoExports(), ObPsMmExports(), RtlExports(), DataExports(),
                                  RegistryExports(), WdfLdrExports()};
    for (const ExportGroup& g : groups)
        for (std::size_t i = 0; i < g.size; ++i) {
            if (!g.data[i].addr) continue;
            auto ins = m.emplace(g.data[i].name, g.data[i].addr);
            if (!ins.second) ERR(driver, "duplicate ntoskrnl export %s", g.data[i].name);
        }
    return m;
}

const ExportMap& Exports() {
    static const ExportMap m = BuildMap();
    return m;
}

SRWLOCK g_stub_lock = SRWLOCK_INIT;
std::deque<std::string>& StubNames() { static std::deque<std::string> d; return d; }
std::map<std::string, std::uint64_t>& StubCalls() { static std::map<std::string, std::uint64_t> m; return m; }

const char* InternName(const char* n) {
    AcquireSRWLockExclusive(&g_stub_lock);
    for (const std::string& s : StubNames())
        if (s == n) { ReleaseSRWLockExclusive(&g_stub_lock); return s.c_str(); }
    StubNames().emplace_back(n);
    const char* r = StubNames().back().c_str();
    ReleaseSRWLockExclusive(&g_stub_lock);
    return r;
}

bool InList(const char* const* list, const char* name) {
    for (; *list; ++list)
        if (!std::strcmp(*list, name)) return true;
    return false;
}

const char* HostAlias(const char* name) {
    for (int i = 0; kHostAliases[i][0]; ++i)
        if (!std::strcmp(kHostAliases[i][0], name)) return kHostAliases[i][1];
    return nullptr;
}

}  // namespace

void* FindExport(const char* name) {
    const ExportMap& m = Exports();
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second;
}

void* ResolveNtFunction(const char* nt_name) {
    if (const dispatch::Entry* e = dispatch::LookupByName(nt_name)) {
        switch (e->policy) {
        case dispatch::Policy::PassThrough: return e->host_fn;
        case dispatch::Policy::Translate:
        case dispatch::Policy::Emulate: return e->impl ? e->impl : e->host_fn;
        case dispatch::Policy::Todo: return nullptr;
        }
    }
    return dispatch::HostExport(nt_name);   // dispatcher not initialised (unit tests) or name unknown to it
}

void* BindImport(const char* module, const char* name, StubArena* arena, bool* is_stub) {
    (void)module;
    if (is_stub) *is_stub = false;
    if (void* p = FindExport(name)) return p;
    if ((name[0] == 'Z' && name[1] == 'w') || (name[0] == 'N' && name[1] == 't')) {
        std::string nt = std::string("Nt") + (name + 2);
        if (void* p = ResolveNtFunction(nt.c_str())) return p;
    }
    if (const char* a = HostAlias(name))
        if (void* p = dispatch::HostExport(a)) return p;
    if (InList(kHostForwards, name))
        if (void* p = dispatch::HostExport(name)) return p;
    if (is_stub) *is_stub = true;
    return arena->Add(name);
}

std::int64_t StubHandler(const char* name) {
    AcquireSRWLockExclusive(&g_stub_lock);
    ++StubCalls()[name];
    ReleaseSRWLockExclusive(&g_stub_lock);
    FIXME(driver, "unimplemented kernel API %s called by a driver, returning STATUS_NOT_IMPLEMENTED", name);
    return static_cast<std::int64_t>(static_cast<std::int32_t>(STATUS_NOT_IMPLEMENTED));
}

// ---- stub arena -------------------------------------------------------------------------------------------------

namespace {
constexpr std::size_t kThunkSize = 32;
constexpr std::size_t kPageSize = 4096;
}

void* StubArena::Add(const char* name) {
    const char* interned = InternName(name);
    if (pages_.empty() || pages_.back().used + kThunkSize > kPageSize) {
        auto* mem = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!mem) return nullptr;
        pages_.push_back(Page{mem, 0});
    }
    Page& pg = pages_.back();
    std::uint8_t* t = pg.mem + pg.used;
    pg.used += kThunkSize;
    const std::uint64_t arg = reinterpret_cast<std::uint64_t>(interned);
    const std::uint64_t fn = reinterpret_cast<std::uint64_t>(&StubHandler);
    std::size_t i = 0;
    t[i++] = 0x48; t[i++] = 0xB9; std::memcpy(t + i, &arg, 8); i += 8;      // mov rcx, name
    t[i++] = 0x48; t[i++] = 0xB8; std::memcpy(t + i, &fn, 8); i += 8;       // mov rax, StubHandler
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xEC; t[i++] = 0x28;             // sub rsp, 40
    t[i++] = 0xFF; t[i++] = 0xD0;                                           // call rax
    t[i++] = 0x48; t[i++] = 0x83; t[i++] = 0xC4; t[i++] = 0x28;             // add rsp, 40
    t[i++] = 0xC3;                                                          // ret
    while (i < kThunkSize) t[i++] = 0xCC;
    ++count_;
    return t;
}

bool StubArena::Seal() {
    bool ok = true;
    for (Page& pg : pages_) {
        DWORD old;
        if (!VirtualProtect(pg.mem, kPageSize, PAGE_EXECUTE_READ, &old)) ok = false;
        FlushInstructionCache(GetCurrentProcess(), pg.mem, kPageSize);
    }
    return ok;
}

StubArena::~StubArena() {
    for (Page& pg : pages_) VirtualFree(pg.mem, 0, MEM_RELEASE);
}

bool ProbeRange(const void* p, std::size_t len, bool write) {
    if (!p) return false;
    auto addr = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t end = addr + (len ? len : 1);
    if (end < addr) return false;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const DWORD ro = PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE;
        if (write ? !(mbi.Protect & rw) : !(mbi.Protect & (rw | ro))) return false;
        addr = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

}  // namespace win10xbox::drivers::detail

namespace win10xbox::drivers {

std::uint64_t StubCallCount(const char* api_name) {
    using namespace detail;
    AcquireSRWLockShared(&g_stub_lock);
    auto it = StubCalls().find(api_name);
    std::uint64_t n = it == StubCalls().end() ? 0 : it->second;
    ReleaseSRWLockShared(&g_stub_lock);
    return n;
}

bool IsExportImplemented(const char* name) {
    using namespace detail;
    if (FindExport(name)) return true;
    if ((name[0] == 'Z' && name[1] == 'w') || (name[0] == 'N' && name[1] == 't')) {
        std::string nt = std::string("Nt") + (name + 2);
        if (ResolveNtFunction(nt.c_str())) return true;
    }
    if (const char* a = HostAlias(name)) return dispatch::HostExport(a) != nullptr;
    return InList(kHostForwards, name) && dispatch::HostExport(name) != nullptr;
}

std::size_t ExportCount() { return detail::Exports().size(); }

}  // namespace win10xbox::drivers
