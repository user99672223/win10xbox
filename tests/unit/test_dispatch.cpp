// Dispatcher unit tests: table, policies, relay formatting, emulated handles,
// the asm entry (through hand-assembled thunks), forwarding patches, cpuid
// policy and unix calls.
#include "check.h"
#include "win10xbox/dispatch.h"
#include "../../core/dispatch/internal.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

using namespace win10xbox;
using namespace win10xbox::dispatch;

namespace {

constexpr NTSTATUS kNotImplemented = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034);

// mov r10,rcx ; mov eax,id ; mov r11,entry ; jmp r11 -- what a rewritten
// guest thunk amounts to (the entry is reached by jmp, with the caller's
// `call` return address on the stack).
void* MakeThunk(std::uint32_t id) {
    auto* p = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!p) return nullptr;
    std::uint8_t* c = p;
    *c++ = 0x4C; *c++ = 0x8B; *c++ = 0xD1;
    *c++ = 0xB8; std::memcpy(c, &id, 4); c += 4;
    *c++ = 0x49; *c++ = 0xBB;
    auto entry = reinterpret_cast<std::uint64_t>(SyscallEntryAddress());
    std::memcpy(c, &entry, 8); c += 8;
    *c++ = 0x41; *c++ = 0xFF; *c++ = 0xE3;
    DWORD old;
    VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4096);
    return p;
}

void FreeThunk(void* p) { VirtualFree(p, 0, MEM_RELEASE); }

std::uint32_t IdOf(const char* name) {
    const Entry* e = LookupByName(name);
    return e ? e->id : 0xffffffffu;
}

NTSTATUS NTAPI Sum12(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e,
                     std::uint64_t f, std::uint64_t g, std::uint64_t h, std::uint64_t i, std::uint64_t j,
                     std::uint64_t k, std::uint64_t l) {
    return static_cast<NTSTATUS>(a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j +
                                 11 * k + 12 * l);
}

int ReturnsTwo() { return 2; }

}  // namespace

TEST(dispatch_table_lookup) {
    CHECK_EQ(Initialize(), 0);
    CHECK(Count() >= 258);
    const Entry* e = LookupByName("NtClose");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->id, 0x000fu);
    CHECK_EQ(e->nargs, 1u);
    CHECK(Lookup(0x000f) == e);
    CHECK(e->host_fn != nullptr);
    const Entry* cf = LookupByName("NtCreateFile");
    CHECK(cf && cf->nargs == 11);
    CHECK(LookupByName("NtNoSuchSyscall") == nullptr);
    CHECK(Lookup(0x1fff) == nullptr);
    // Wine lists unimplemented syscalls with 0 args; the dispatcher corrects them.
    const Entry* te = LookupByName("NtTraceEvent");
    CHECK(te && te->nargs == 4);
    CHECK_EQ(win10xbox_syscall_nargs(0x000f), 1u);
    CHECK_EQ(win10xbox_syscall_nargs(0x1fff), 0u);
}

TEST(dispatch_policies_from_inc) {
    Initialize();
    auto pol = [](const char* n) { const Entry* e = LookupByName(n); return e ? e->policy : Policy::Todo; };
    CHECK(pol("NtAllocateVirtualMemory") == Policy::PassThrough);
    CHECK(pol("NtOpenDirectoryObject") == Policy::Translate);
    CHECK(pol("NtCreateSection") == Policy::Translate);
    CHECK(pol("NtTerminateProcess") == Policy::Emulate);
    CHECK(pol("NtCreateUserProcess") == Policy::Translate);
    CHECK(pol("NtShutdownSystem") == Policy::Emulate);   // privileged: test_syscalls_privileged.cpp
    const Entry* ck = LookupByName("NtCreateKey");
    CHECK(ck && (ck->policy == Policy::Emulate || ck->policy == Policy::PassThrough));
    CHECK(ck && ck->note && *ck->note);
}

TEST(dispatch_policy_parsing) {
    Policy p = Policy::Todo;
    CHECK(ParsePolicy("passthrough", &p) && p == Policy::PassThrough);
    CHECK(ParsePolicy("PassThrough", &p) && p == Policy::PassThrough);
    CHECK(ParsePolicy("TRANSLATE", &p) && p == Policy::Translate);
    CHECK(ParsePolicy("emulate", &p) && p == Policy::Emulate);
    CHECK(ParsePolicy("todo", &p) && p == Policy::Todo);
    CHECK(!ParsePolicy("sometimes", &p));
    CHECK(!ParsePolicy(nullptr, &p));
    CHECK(std::string(PolicyName(Policy::Translate)) == "Translate");
}

TEST(dispatch_set_policy) {
    Initialize();
    CHECK_EQ(SetPolicy("NtYieldExecution", Policy::Todo), 0);
    CHECK(LookupByName("NtYieldExecution")->policy == Policy::Todo);
    CHECK_EQ(SetPolicy("NtYieldExecution", Policy::PassThrough), 0);
    CHECK(LookupByName("NtYieldExecution")->policy == Policy::PassThrough);
    CHECK(SetPolicy("NtNoSuchSyscall", Policy::Todo) != 0);
    CHECK_EQ(SetPolicy("NtYieldExecution", Policy::Emulate), kNotImplemented);   // no implementation
}

TEST(dispatch_relay_format) {
    Initialize();
    const Entry* e = LookupByName("NtClose");
    std::uint64_t a[1] = {0x1234};
    CHECK_EQ(FormatRelay(*e, a, 0), std::string("NtClose(00001234) ret=00000000"));
    const Entry* q = LookupByName("NtQueryPerformanceCounter");
    std::uint64_t b[2] = {0x00007ff612345678ull, 0};
    CHECK_EQ(FormatRelay(*q, b, kNotImplemented),
             std::string("NtQueryPerformanceCounter(00007ff612345678,00000000) ret=c0000002"));
    std::string t = FormatRelay(*e, a, 0, 12.25);
    CHECK(t.find(" (12.2us)") != std::string::npos || t.find(" (12.3us)") != std::string::npos);
}

TEST(dispatch_emulated_handles) {
    HANDLE h = RegisterEmulatedObject(ObjectKind::Device, 77, reinterpret_cast<void*>(0x1234));
    CHECK(h != nullptr);
    EmulatedObject o{};
    CHECK(LookupEmulatedObject(h, &o));
    CHECK(o.kind == ObjectKind::Device && o.id == 77 && o.extra == reinterpret_cast<void*>(0x1234));
    // Guest handle values are real host event handles, so waits behave.
    CHECK_EQ(WaitForSingleObject(h, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    UnregisterEmulatedObject(h);
    CHECK(!LookupEmulatedObject(h, &o));
}

TEST(dispatch_call_with_args) {
    std::uint64_t a[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    CHECK_EQ(CallWithArgs(reinterpret_cast<void*>(&Sum12), a, 12), 78);
    CHECK_EQ(CallWithArgs(reinterpret_cast<void*>(&Sum12), a, 4), 10);   // missing args are 0
}

TEST(dispatch_asm_entry_passthrough_qpc) {
    Initialize();
    CHECK_EQ(SetPolicy("NtQueryPerformanceCounter", Policy::PassThrough), 0);
    void* thunk = MakeThunk(IdOf("NtQueryPerformanceCounter"));
    CHECK(thunk != nullptr);
    if (!thunk) return;
    using Fn = NTSTATUS(NTAPI*)(PLARGE_INTEGER, PLARGE_INTEGER);
    LARGE_INTEGER c1{}, c2{}, f{}, hf{};
    CHECK_EQ(reinterpret_cast<Fn>(thunk)(&c1, &f), 0);
    CHECK_EQ(reinterpret_cast<Fn>(thunk)(&c2, nullptr), 0);
    QueryPerformanceFrequency(&hf);
    CHECK(c1.QuadPart > 0 && c2.QuadPart >= c1.QuadPart);
    CHECK_EQ(f.QuadPart, hf.QuadPart);
    CHECK(LookupByName("NtQueryPerformanceCounter")->calls >= 2);
    FreeThunk(thunk);
}

TEST(dispatch_asm_entry_stack_args) {
    // NtQueryVirtualMemory has 6 args: two of them come from the caller's stack.
    Initialize();
    void* thunk = MakeThunk(IdOf("NtQueryVirtualMemory"));
    if (!thunk) { CHECK(false); return; }
    using Fn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T ret = 0;
    NTSTATUS s = reinterpret_cast<Fn>(thunk)(GetCurrentProcess(), thunk, 0, &mbi, sizeof mbi, &ret);
    CHECK_EQ(s, 0);
    CHECK_EQ(ret, sizeof mbi);
    CHECK(mbi.BaseAddress == thunk);
    CHECK(mbi.State == MEM_COMMIT && mbi.Protect == PAGE_EXECUTE_READ);
    FreeThunk(thunk);
}

TEST(dispatch_asm_entry_todo_and_translate) {
    Initialize();
    void* yield = MakeThunk(IdOf("NtYieldExecution"));
    void* opendir = MakeThunk(IdOf("NtOpenDirectoryObject"));
    if (!yield || !opendir) { CHECK(false); return; }
    CHECK_EQ(SetPolicy("NtYieldExecution", Policy::Todo), 0);
    CHECK_EQ(reinterpret_cast<NTSTATUS(NTAPI*)()>(yield)(), kNotImplemented);
    SetPolicy("NtYieldExecution", Policy::PassThrough);
    NTSTATUS y = reinterpret_cast<NTSTATUS(NTAPI*)()>(yield)();
    CHECK(y == 0 || y == 0x40000024 /*STATUS_NO_YIELD_PERFORMED*/);

    wchar_t name[] = L"\\KnownDlls";
    UNICODE_STRING us{static_cast<USHORT>(sizeof(name) - 2), static_cast<USHORT>(sizeof(name)), name};
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    HANDLE h = nullptr;
    using OpenFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    CHECK_EQ(reinterpret_cast<OpenFn>(opendir)(&h, 0x0003 /*DIRECTORY_QUERY|TRAVERSE*/, &oa), kObjectNameNotFound);
    CHECK(h == nullptr);
    FreeThunk(yield);
    FreeThunk(opendir);
}

TEST(dispatch_unknown_id) {
    Initialize();
    void* t = MakeThunk(0x0fff);
    if (!t) { CHECK(false); return; }
    CHECK_EQ(reinterpret_cast<NTSTATUS(NTAPI*)(void*)>(t)(nullptr), kNotImplemented);
    FreeThunk(t);
}

TEST(dispatch_host_mode_scope_noop) {
    Initialize();
    void* tls = reinterpret_cast<void*>(__readgsqword(0x58));
    {
        HostModeScope a;
        HostModeScope b;
    }
    CHECK(reinterpret_cast<void*>(__readgsqword(0x58)) == tls);
    CHECK(!process::IsGuestThread());
}

TEST(dispatch_image_information) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE);
    if (f == INVALID_HANDLE_VALUE) return;
    std::shared_ptr<detail::ImageSection> sec;
    CHECK_EQ(detail::CreateImageSectionFromFile(f, &sec), 0);
    CloseHandle(f);
    if (!sec) return;
    auto* self = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(self + reinterpret_cast<IMAGE_DOS_HEADER*>(self)->e_lfanew);
    std::uint64_t transfer;
    std::uint16_t machine, chars;
    std::memcpy(&transfer, sec->info, 8);
    std::memcpy(&machine, sec->info + 0x30, 2);
    std::memcpy(&chars, sec->info + 0x2C, 2);
    CHECK_EQ(machine, static_cast<std::uint16_t>(IMAGE_FILE_MACHINE_AMD64));
    CHECK_EQ(chars, nt->FileHeader.Characteristics);
    // TransferAddress uses the file's preferred base (the section is not mapped).
    std::uint64_t file_base;
    {
        auto* b = sec->bytes.data();
        auto* fnt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        file_base = fnt->OptionalHeader.ImageBase;
    }
    CHECK_EQ(transfer, file_base + nt->OptionalHeader.AddressOfEntryPoint);
    CHECK_EQ(sec->info[0x32], 1);   // ImageContainsCode
    CHECK_EQ(sec->size_of_image, nt->OptionalHeader.SizeOfImage);

    std::uint8_t info[0x40];
    const std::uint8_t junk[64] = {'X', 'Y'};
    CHECK(detail::BuildImageInformation(junk, sizeof junk, info, nullptr) != 0);
}

TEST(dispatch_patch_jump) {
    // A 6-byte function `mov eax,1 ; ret` far from the target, redirected.
    auto* p = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!p) { CHECK(false); return; }
    const std::uint8_t code[] = {0xB8, 0x01, 0, 0, 0, 0xC3};
    std::memcpy(p, code, sizeof code);
    DWORD old;
    VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    using Fn = int (*)();
    CHECK_EQ(reinterpret_cast<Fn>(p)(), 1);
    CHECK(detail::PatchJump(p, reinterpret_cast<const void*>(&ReturnsTwo)));
    CHECK_EQ(reinterpret_cast<Fn>(p)(), 2);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof mbi);
    CHECK_EQ(mbi.Protect, static_cast<DWORD>(PAGE_EXECUTE_READ));   // protection restored
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST(dispatch_cpuid_policy) {
    CpuPolicy saved = GetCpuPolicy();
    CpuPolicy p;
    p.vendor = "AuthenticAMD";
    p.hide_hypervisor = true;
    SetCpuPolicy(p);
    loader::ScanRegs r{};
    r.rax = 0;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    char v[13] = {};
    std::uint32_t ebx = static_cast<std::uint32_t>(r.rbx), edx = static_cast<std::uint32_t>(r.rdx),
                  ecx = static_cast<std::uint32_t>(r.rcx);
    std::memcpy(v, &ebx, 4); std::memcpy(v + 4, &edx, 4); std::memcpy(v + 8, &ecx, 4);
    CHECK_EQ(std::string(v), std::string("AuthenticAMD"));
    r = {};
    r.rax = 1;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    CHECK((r.rcx & (1u << 31)) == 0);
    r = {};
    r.rax = 0x40000000;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    CHECK(r.rax == 0 && r.rbx == 0 && r.rcx == 0 && r.rdx == 0);
    r = {};
    CHECK(CpuSiteHandler(loader::SiteKind::Rdtsc, &r, nullptr));
    std::uint64_t t1 = (r.rdx << 32) | r.rax;
    CHECK(CpuSiteHandler(loader::SiteKind::Rdtsc, &r, nullptr));
    std::uint64_t t2 = (r.rdx << 32) | r.rax;
    CHECK(t2 >= t1);
    SetCpuPolicy(saved);
}

// D18: the CPUID patch serves the machine identity: brand string, family/model/stepping, RDRAND, no hypervisor.
TEST(dispatch_cpuid_identity) {
    CpuPolicy saved = GetCpuPolicy();
    CpuPolicy p;
    p.vendor = "AuthenticAMD";
    p.hide_hypervisor = true;
    std::strcpy(p.brand, "AMD 4700S 8-Core Processor");
    p.family = 23; p.model = 136; p.stepping = 1;
    p.advertise_rdrand = true;
    SetCpuPolicy(p);
    loader::ScanRegs r{};
    r.rax = 1;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    CHECK_EQ(static_cast<std::uint32_t>(r.rax), 0x00880F81u);
    CHECK((r.rcx & (1u << 30)) != 0);
    CHECK((r.rcx & (1u << 31)) == 0);
    r = {};
    r.rax = 0x80000001u;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    CHECK_EQ(static_cast<std::uint32_t>(r.rax), 0x00880F81u);
    char brand[49] = {};
    for (std::uint32_t i = 0; i < 3; ++i) {
        r = {};
        r.rax = 0x80000002u + i;
        CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
        const std::uint32_t w[4] = {static_cast<std::uint32_t>(r.rax), static_cast<std::uint32_t>(r.rbx),
                                    static_cast<std::uint32_t>(r.rcx), static_cast<std::uint32_t>(r.rdx)};
        std::memcpy(brand + 16 * i, w, 16);
    }
    CHECK_EQ(std::string(brand), std::string("AMD 4700S 8-Core Processor"));
    r = {};
    r.rax = 0x40000000;
    CHECK(CpuSiteHandler(loader::SiteKind::Cpuid, &r, nullptr));
    CHECK(r.rax == 0 && r.rbx == 0 && r.rcx == 0 && r.rdx == 0);
    SetCpuPolicy(saved);
}

// D18: RDRAND/RDSEED are advertised; on a host without them the #UD handler stands in (random bytes, CF=1, RIP past it).
TEST(dispatch_rdrand_emulation) {
    InstallRandomInstructionEmulation();
    const std::uint8_t code[] = {0x0F, 0xC7, 0xF0,                // rdrand eax
                                 0x48, 0x0F, 0xC7, 0xF1,          // rdrand rcx
                                 0x0F, 0xC7, 0xF8,                // rdseed eax
                                 0x90, 0, 0, 0, 0, 0, 0, 0};      // padding so the decoder may read 15 bytes
    CONTEXT c{};
    c.Rax = 0xFFFFFFFFFFFFFFFFull;
    c.EFlags = 0x246;   // ZF, PF set
    c.Rip = reinterpret_cast<DWORD64>(code);
    CHECK(EmulateRandomInstruction(&c));
    CHECK_EQ(c.Rip, reinterpret_cast<DWORD64>(code) + 3);
    CHECK((c.Rax >> 32) == 0);                  // a 32-bit destination clears the upper half
    CHECK((c.EFlags & 1u) == 1u && (c.EFlags & 0x44u) == 0);
    CHECK(EmulateRandomInstruction(&c));
    CHECK_EQ(c.Rip, reinterpret_cast<DWORD64>(code) + 7);
    bool varied = false;   // 64 random bits are not all zero twice in a row
    DWORD64 first = c.Rcx;
    for (int i = 0; i < 4 && !varied; ++i) {
        c.Rip = reinterpret_cast<DWORD64>(code) + 3;
        CHECK(EmulateRandomInstruction(&c));
        varied = c.Rcx != first;
    }
    CHECK(varied);
    CHECK(EmulateRandomInstruction(&c));        // rdseed eax
    c.Rip = reinterpret_cast<DWORD64>(code) + 10;   // nop: not ours
    CHECK(!EmulateRandomInstruction(&c));
}

// D18: the synthetic SMBIOS table parses the way wbemprox and Windows tools parse it, and the NtQuerySystemInformation
// answer follows the SYSTEM_FIRMWARE_TABLE_INFORMATION protocol (too small -> the needed size, then the table).
TEST(dispatch_smbios_table) {
    config::Config::MachineIdentity id;
    const std::vector<std::uint8_t> t = detail::MakeSmbios(id, "AuthenticAMD", 8, 16, 3600, 0x178BFBFFu);
    CHECK(t.size() > 8);
    std::uint32_t len = 0;
    std::memcpy(&len, t.data() + 4, 4);
    CHECK_EQ(static_cast<std::size_t>(len) + 8, t.size());
    CHECK_EQ(t[1], 3);
    const std::uint8_t* p = t.data() + 8;
    const std::uint8_t* end = t.data() + t.size();
    int types[4] = {};
    std::string version, vendor;
    std::uint64_t proc_id = 0;
    while (p + 4 <= end) {
        const std::uint8_t type = p[0], flen = p[1];
        CHECK(flen >= 4 && p + flen <= end);
        const char* strs = reinterpret_cast<const char*>(p) + flen;
        auto str = [&](unsigned idx) {
            const char* s = strs;
            for (unsigned i = 1; i < idx; ++i) s += std::strlen(s) + 1;
            return std::string(s);
        };
        if (type == 4) {
            version = str(p[0x10]);
            vendor = str(p[7]);
            std::memcpy(&proc_id, p + 8, 8);
            CHECK_EQ(flen, 0x30);
            CHECK_EQ(p[0x2A], 8);   // core count 2
            CHECK_EQ(p[0x2E], 16);  // thread count 2
        }
        if (type == 0) ++types[0];
        if (type == 1) ++types[1];
        if (type == 4) ++types[2];
        if (type == 127) { ++types[3]; break; }
        const char* q = strs;
        while (q + 1 < reinterpret_cast<const char*>(end) && (q[0] || q[1])) ++q;
        p = reinterpret_cast<const std::uint8_t*>(q) + 2;
    }
    CHECK(types[0] == 1 && types[1] == 1 && types[2] == 1 && types[3] == 1);
    CHECK_EQ(version, std::string("AMD 4700S 8-Core Processor"));
    CHECK_EQ(vendor, std::string("AuthenticAMD"));
    CHECK_EQ(proc_id, 0x178BFBFF00880F81ull);

    detail::BuildFirmwareTables("AuthenticAMD");
    std::vector<std::uint32_t> buf(4 + 4);
    buf[0] = 0x52534D42; buf[1] = 1; buf[2] = 0;   // 'RSMB', get
    ULONG ret = 0;
    NTSTATUS status = 0;
    CHECK(detail::QuerySystemFirmwareTable(buf.data(), 16, &ret, &status));
    CHECK_EQ(status, static_cast<NTSTATUS>(0xC0000023));   // STATUS_BUFFER_TOO_SMALL
    CHECK(ret > 16);
    CHECK_EQ(buf[3], ret - 16);
    std::vector<std::uint32_t> full((ret + 3) / 4);
    full[0] = 0x52534D42; full[1] = 1;
    CHECK(detail::QuerySystemFirmwareTable(full.data(), ret, &ret, &status));
    CHECK_EQ(status, 0);
    CHECK_EQ(reinterpret_cast<const std::uint8_t*>(full.data())[16 + 1], 3);   // SMBIOS major version
    full[0] = 0x4D524946;   // 'FIRM': not ours
    CHECK(!detail::QuerySystemFirmwareTable(full.data(), ret, &ret, &status));
}

TEST(dispatch_unix_calls) {
    Initialize();
    detail::RegisterNtdllUnixLib();
    std::uint64_t h = detail::NtdllUnixLibHandle();
    CHECK(h != 0);
    CHECK_EQ(detail::UnixLibHandleByName(L"ntdll.dll"), h);
    CHECK_EQ(detail::UnixLibHandleByName(L"C:\\x\\ntdll.so"), h);
    CHECK_EQ(detail::UnixLibHandleByName(L"win32u.dll"), 0ull);
    LONGLONG t = 0;
    CHECK_EQ(UnixCallDispatcher(h, 7 /*unix_system_time_precise*/, &t), 0);
    CHECK(t > 116444736000000000LL);   // after 1970
    int req[3] = {42, 0, 0};
    CHECK_EQ(UnixCallDispatcher(h, 3 /*unix_wine_server_call*/, req), kNotImplemented);
    CHECK_EQ(UnixCallDispatcher(h, 0 /*unix_load_so_dll*/, nullptr), static_cast<NTSTATUS>(0xC0000135));
    CHECK_EQ(UnixCallDispatcher(h, 99, nullptr), kNotImplemented);
    CHECK_EQ(UnixCallDispatcher(12345, 0, nullptr), kNotImplemented);
}

TEST(dispatch_create_heap_growable) {
    Initialize();
    // Wine's HeapCreate(0, 0, 0) reaches RtlCreateHeap with flags 0 and no maximum; the heap must still grow past the
    // host's default 1 MB reservation (git's malloc ran dry in one).
    void* heap = detail::w10x_RtlCreateHeap(0, nullptr, 0, 0, nullptr, nullptr);
    CHECK(heap != nullptr);
    if (!heap) return;
    bool all = true;
    void* blocks[48] = {};
    for (auto& b : blocks) {
        b = HeapAlloc(heap, HEAP_ZERO_MEMORY, 100 * 1024);
        all = all && b != nullptr;
    }
    CHECK(all);
    HeapDestroy(heap);
}

TEST(dispatch_wine_debug_channels) {
    // A Wine channel struct with the lazy-init bit set, as Wine declares them.
    detail::WineDebugChannel ch{0xff, "loaddll"};
    unsigned char f = detail::w10x_wine_dbg_get_channel_flags(&ch);
    CHECK((f & 0x80) == 0);       // init bit cleared, result cached
    CHECK_EQ(ch.flags, f);
    CHECK((f & 0x8) == 0);        // trace off unless WIN10XBOX_WINEDEBUG enables it
    CHECK_EQ(detail::w10x_wine_dbg_header(3, &ch, "f"), -1);
    const char* a = detail::w10x_wine_dbg_strdup("abc");
    const char* b = detail::w10x_wine_dbg_strdup("defg");
    CHECK(std::string(a) == "abc" && std::string(b) == "defg");
}

TEST(dispatch_preserves_last_error) {
    // A syscall never touches TEB.LastErrorValue; the translators' host Win32
    // calls (here GetProcessId on a bogus handle) must not leak into it.
    Initialize();
    void* t = MakeThunk(IdOf("NtQueryInformationProcess"));
    if (!t) { CHECK(false); return; }
    using Fn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    ULONG_PTR buf[8] = {};
    SetLastError(1234);
    NTSTATUS s = reinterpret_cast<Fn>(t)(reinterpret_cast<HANDLE>(0x123456), 0, buf, sizeof buf, nullptr);
    DWORD after = GetLastError();
    CHECK(s != 0);
    CHECK_EQ(after, 1234ul);
    FreeThunk(t);
}
