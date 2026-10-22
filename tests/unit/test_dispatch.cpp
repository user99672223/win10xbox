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
    CHECK(pol("NtCreateUserProcess") == Policy::Todo);
    CHECK(pol("NtShutdownSystem") == Policy::Todo);
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

// Dispatch by name: a thunk from another build's DLL (a native win32u/ntdll) carries that build's id in its
// `mov eax, imm32`; RenumberSyscallThunks rewrites it to the table's id for the thunk's NAME. A name with no
// entry gets an id that no entry has, so it can never land in whatever entry its foreign id collides with.
TEST(dispatch_renumber_thunks_by_name) {
    CHECK_EQ(Initialize(), 0);
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(page != nullptr);
    if (!page) return;
    // Windows 24-byte thunk: mov r10,rcx; mov eax,id; test [7FFE0308h],1; jne; syscall; ret; int 2Eh; ret
    auto make = [&](std::uint8_t* t, std::uint32_t id) {
        const std::uint8_t bytes[24] = {0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0, 0xF6, 0x04, 0x25, 0x08,
                                        0x03, 0xFE, 0x7F, 0x01, 0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};
        std::memcpy(t, bytes, sizeof bytes);
        std::memcpy(t + 4, &id, 4);
    };
    auto id_at = [](const std::uint8_t* t) { std::uint32_t v; std::memcpy(&v, t + 4, 4); return v; };
    const std::uint32_t close_id = IdOf("NtClose"), user_id = IdOf("NtUserCreateWindowEx");
    CHECK(close_id < 0x1000);
    CHECK(user_id >= 0x1000 && user_id < 0x1800);
    make(page, 0x106F);          // the native 26100 win32u's NtUserCreateWindowEx id, here on NtClose's thunk
    make(page + 32, 0x106F);     // the native NtUserCreateWindowEx thunk itself
    make(page + 64, 0x0123);     // a name the table does not have
    make(page + 96, close_id);   // already right: left alone
    std::vector<loader::StubPatch> patched = {
        {"NtClose", 0x106F, page},
        {"NtUserCreateWindowEx", 0x106F, page + 32},
        {"NtNoSuchSyscallAnywhere", 0x0123, page + 64},
        {"NtClose", close_id, page + 96},
    };
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);   // like a finalized image: protected around the write
    CHECK_EQ(detail::RenumberSyscallThunks(patched, L"test"), static_cast<std::size_t>(3));
    CHECK_EQ(id_at(page), close_id);
    CHECK_EQ(id_at(page + 32), user_id);
    CHECK_EQ(id_at(page + 64), 0x1FFFu);
    CHECK(Lookup(0x1FFF) == nullptr);
    CHECK_EQ(id_at(page + 96), close_id);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(page, &mbi, sizeof mbi);
    CHECK(mbi.Protect == PAGE_EXECUTE_READ);
    VirtualFree(page, 0, MEM_RELEASE);
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
