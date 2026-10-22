// Process-creation syscalls and the refusals around them (core/dispatch/nt_spawn.cpp, Tok_NtCreateLowBoxToken):
// policies, the STATUS_NOT_SUPPORTED group, NtSetSystemInformation's privilege split, NtMakePermanentObject's
// pass-through, the child-process helpers and a real NtCreateUserProcess through the asm entry.
#include "check.h"
#include "win10xbox/config.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/token.h"
#include "win10xbox/vfs.h"
#include "../../core/dispatch/nt_spawn.h"
#include <windows.h>
#include <cstring>
#include <type_traits>
#include <string>
#include <vector>

using namespace win10xbox;
using namespace win10xbox::dispatch;
using namespace win10xbox::dispatch::detail;

namespace {

constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kInvalidHandle = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS kInvalidParameter = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kAccessViolation = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS kNotSupported = static_cast<NTSTATUS>(0xC00000BB);
constexpr NTSTATUS kPrivilegeNotHeld = static_cast<NTSTATUS>(0xC0000061);
constexpr NTSTATUS kInvalidImageNotMz = static_cast<NTSTATUS>(0xC000012F);
constexpr NTSTATUS kInvalidImageFormat = static_cast<NTSTATUS>(0xC000007B);

// mov r10,rcx ; mov eax,id ; mov r11,entry ; jmp r11 -- what a rewritten guest thunk amounts to.
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

// The syscall `name` called through the real asm entry, with up to 11 arguments.
struct Syscall {
    void* thunk;
    explicit Syscall(const char* name) {
        Initialize();
        const Entry* e = LookupByName(name);
        thunk = e ? MakeThunk(e->id) : nullptr;
        CHECK_MSG(thunk != nullptr, "%s is not in the syscall table", name);
    }
    ~Syscall() { if (thunk) VirtualFree(thunk, 0, MEM_RELEASE); }
    template <class... A> NTSTATUS operator()(A... a) const {
        using Fn = NTSTATUS(NTAPI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                    std::uint64_t);
        std::uint64_t v[11] = {};
        std::uint64_t in[] = {Arg(a)..., 0};
        for (std::size_t i = 0; i < sizeof...(A); ++i) v[i] = in[i];
        return reinterpret_cast<Fn>(thunk)(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]);
    }
    template <class T> static std::uint64_t Arg(T x) {
        if constexpr (std::is_null_pointer_v<T>) return 0;
        else if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::uint64_t>(x);
        else return static_cast<std::uint64_t>(x);
    }
};

Policy PolicyOf(const char* name) {
    Initialize();
    const Entry* e = LookupByName(name);
    return e ? e->policy : Policy::Todo;
}
bool HasImpl(const char* name) {
    const Entry* e = LookupByName(name);
    return e && e->impl != nullptr;
}

std::wstring TestBinDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    p.resize(p.find_last_of(L'\\'));
    return p;
}

}  // namespace

// ---- policies ----------------------------------------------------------------------------------------------------

TEST(syscalls_process_policies) {
    struct { const char* name; Policy policy; } want[] = {
        {"NtCreateUserProcess", Policy::Translate}, {"NtCreateProcessEx", Policy::Emulate},
        {"NtCreateThread", Policy::Emulate},        {"NtCreateDebugObject", Policy::Emulate},
        {"NtDebugActiveProcess", Policy::Emulate},  {"NtCreateLowBoxToken", Policy::Emulate},
        {"NtMakePermanentObject", Policy::PassThrough}, {"NtSetSystemInformation", Policy::Translate},
    };
    for (auto& w : want) {
        CHECK_MSG(PolicyOf(w.name) == w.policy, "%s: %s", w.name, PolicyName(PolicyOf(w.name)));
        if (w.policy != Policy::PassThrough) CHECK_MSG(HasImpl(w.name), "%s has no implementation", w.name);
    }
    // Wine lists NtCreateProcessEx with 0 arguments; the dispatcher knows it takes 9.
    CHECK_EQ(LookupByName("NtCreateProcessEx")->nargs, 9u);
    CHECK_EQ(LookupByName("NtCreateUserProcess")->nargs, 11u);
}

// ---- the refusals -------------------------------------------------------------------------------------------------

TEST(syscalls_process_refusals_are_not_supported) {
    HANDLE h = reinterpret_cast<HANDLE>(0x1234);
    // No debugger support: nothing is created, the output handle is left alone.
    CHECK_EQ(Syscall("NtCreateDebugObject")(&h, 0x1F000F, nullptr, 0), kNotSupported);
    CHECK(h == reinterpret_cast<HANDLE>(0x1234));
    CHECK_EQ(Syscall("NtDebugActiveProcess")(reinterpret_cast<HANDLE>(-1), reinterpret_cast<HANDLE>(0x4)), kNotSupported);
    // Legacy thread / empty-process creation.
    CHECK_EQ(Syscall("NtCreateThread")(&h, 0x1FFFFF, nullptr, reinterpret_cast<HANDLE>(-1), nullptr, nullptr, nullptr, 1),
             kNotSupported);
    CHECK_EQ(Syscall("NtCreateProcessEx")(&h, 0x1FFFFF, nullptr, reinterpret_cast<HANDLE>(-1), 0, nullptr, nullptr,
                                          nullptr, 0), kNotSupported);
    CHECK(h == reinterpret_cast<HANDLE>(0x1234));
}

TEST(syscalls_process_lowbox_token) {
    token::Initialize(token::Identity{});
    Syscall create("NtCreateLowBoxToken");
    HANDLE out = reinterpret_cast<HANDLE>(0x77);
    // S-1-15-2-1-1-1-1-1-1-1: an AppContainer package SID.
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    SID_IDENTIFIER_AUTHORITY app_pkg = {{0, 0, 0, 0, 0, 15}};
    PSID sid = sid_buf;
    InitializeSid(sid, &app_pkg, 8);
    for (DWORD i = 0; i < 8; ++i) *GetSidSubAuthority(sid, i) = i == 0 ? 2 : 1;
    const HANDLE process_token = reinterpret_cast<HANDLE>(-4);

    // A valid token and package: the failure is the decision (no AppContainer tokens), not an argument error.
    CHECK_EQ(create(&out, process_token, TOKEN_ALL_ACCESS, nullptr, sid, 0, nullptr, 0, nullptr), kNotSupported);
    CHECK(out == nullptr);   // never a half-made handle
    out = reinterpret_cast<HANDLE>(0x77);
    CHECK_EQ(create(&out, reinterpret_cast<HANDLE>(0x1234), TOKEN_ALL_ACCESS, nullptr, sid, 0, nullptr, 0, nullptr),
             kInvalidHandle);
    CHECK_EQ(create(&out, process_token, TOKEN_ALL_ACCESS, nullptr, nullptr, 0, nullptr, 0, nullptr), kInvalidParameter);
    CHECK_EQ(create(static_cast<PHANDLE>(nullptr), process_token, TOKEN_ALL_ACCESS, nullptr, sid, 0, nullptr, 0, nullptr),
             kAccessViolation);
}

// ---- NtMakePermanentObject ------------------------------------------------------------------------------------------

TEST(syscalls_process_make_permanent_passes_through) {
    Initialize();
    // Whatever the host answers for a handle that is not an object, the guest gets unchanged. A host ntdll without
    // the export (Wine 9 and older do not have it) gives the dispatcher's own STATUS_NOT_IMPLEMENTED.
    using Fn = NTSTATUS(NTAPI*)(HANDLE);
    auto host = reinterpret_cast<Fn>(HostExport("NtMakePermanentObject"));
    for (HANDLE bad : {static_cast<HANDLE>(nullptr), reinterpret_cast<HANDLE>(0x7FFFFFF0)}) {
        const NTSTATUS want = host ? host(bad) : static_cast<NTSTATUS>(0xC0000002);
        CHECK_EQ(Syscall("NtMakePermanentObject")(bad), want);
    }
}

// ---- NtSetSystemInformation -------------------------------------------------------------------------------------------

TEST(syscalls_process_set_system_information_classification) {
    const char* name = nullptr;
    const char* privilege = nullptr;
    CHECK(ClassifySetSystemInformation(28, &name, &privilege) == SetSystemInfoAction::PrivilegeNotHeld);
    CHECK(name && std::strcmp(name, "SystemTimeAdjustmentInformation") == 0);
    CHECK(privilege && std::strcmp(privilege, "SeSystemtimePrivilege") == 0);
    CHECK(ClassifySetSystemInformation(9, &name, nullptr) == SetSystemInfoAction::PrivilegeNotHeld);
    CHECK(ClassifySetSystemInformation(44, &name, nullptr) == SetSystemInfoAction::PrivilegeNotHeld);   // time zone
    CHECK(ClassifySetSystemInformation(59, &name, &privilege) == SetSystemInfoAction::PassThrough);
    CHECK(name && std::strcmp(name, "SystemComPlusPackage") == 0);
    // Unlisted classes are refused too, unnamed.
    CHECK(ClassifySetSystemInformation(0xFFFF, &name, nullptr) == SetSystemInfoAction::PrivilegeNotHeld);
    CHECK(name == nullptr);
    // Query-only classes have nothing to set on behalf of the guest.
    CHECK(ClassifySetSystemInformation(5, &name, nullptr) == SetSystemInfoAction::PrivilegeNotHeld);
}

TEST(syscalls_process_set_system_information_through_the_entry) {
    Syscall set("NtSetSystemInformation");
    std::uint32_t value = 0;
    // The privileged ones never reach the host, whatever its token could do.
    for (ULONG klass : {9u, 21u, 28u, 31u, 38u, 44u, 56u, 80u, 93u, 200u})
        CHECK_MSG(set(klass, &value, sizeof value) == kPrivilegeNotHeld, "class %lu", klass);
    // SystemComPlusPackage reaches the host. A bad length makes the host's answer independent of its token.
    using Fn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG);
    auto host = reinterpret_cast<Fn>(HostExport("NtSetSystemInformation"));
    CHECK(host != nullptr);
    if (host) CHECK_EQ(set(59u, nullptr, 0u), host(59, nullptr, 0));
}

// ---- helpers of NtCreateUserProcess --------------------------------------------------------------------------------------

TEST(syscalls_process_quoting_and_command_line) {
    auto q = [](std::wstring_view a) { std::wstring s; AppendQuotedArgument(&s, a); return s; };
    CHECK(q(L"plain") == L"plain");
    CHECK(q(L"") == L"\"\"");
    CHECK(q(L"a b") == L"\"a b\"");
    CHECK(q(L"C:\\Program Files\\x") == L"\"C:\\Program Files\\x\"");
    CHECK(q(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(q(L"dir \\") == L"\"dir \\\\\"");          // backslashes before the closing quote are doubled
    CHECK(q(L"C:\\x\\") == L"C:\\x\\");

    const std::wstring cl = BuildChildCommandLine(L"C:\\host dir\\win10xbox.exe", L"C:\\machine", L"C:\\guest\\app.exe",
                                                  L"\"C:\\guest\\app.exe\" /x \"y z\"", L"C:\\work dir\\", true);
    CHECK(cl == L"\"C:\\host dir\\win10xbox.exe\" --machine C:\\machine --cwd \"C:\\work dir\\\\\" --guest-env "
                L"--cmdline \"\\\"C:\\guest\\app.exe\\\" /x \\\"y z\\\"\" C:\\guest\\app.exe");
    const std::wstring bare = BuildChildCommandLine(L"h.exe", L"M", L"C:\\a.exe", L"", L"", false);
    CHECK(bare == L"h.exe --machine M C:\\a.exe");
}

TEST(syscalls_process_environment_block) {
    CHECK(IsHostOwnedVariable(L"WIN10XBOX_DEBUG=+all"));
    CHECK(IsHostOwnedVariable(L"winedebug=-all"));
    CHECK(IsHostOwnedVariable(L"WINEPREFIX=/x"));
    CHECK(!IsHostOwnedVariable(L"windir=C:\\Windows"));
    CHECK(!IsHostOwnedVariable(L"Path=C:\\Windows"));
    const std::vector<std::wstring> guest = {L"Path=C:\\Windows", L"WINEDEBUG=guest-value", L"A=1", L""};
    const std::vector<std::wstring> host = {L"Path=Z:\\host", L"WIN10XBOX_DEBUG=+all", L"WINEDEBUG=-all", L"HOME=/h"};
    std::vector<wchar_t> block = BuildChildEnvironmentBlock(guest, host);
    std::vector<std::wstring> got;
    for (const wchar_t* p = block.data(); *p; p += wcslen(p) + 1) got.push_back(p);
    const std::vector<std::wstring> want = {L"Path=C:\\Windows", L"A=1", L"WIN10XBOX_DEBUG=+all", L"WINEDEBUG=-all"};
    CHECK(got == want);
    CHECK(block.size() >= 2 && block[block.size() - 1] == 0 && block[block.size() - 2] == 0);
    // An empty environment is still a valid block.
    block = BuildChildEnvironmentBlock({}, {});
    CHECK_EQ(block.size(), 2u);
}

TEST(syscalls_process_host_environment_entries) {
    SetEnvironmentVariableW(L"W10X_ENV_PROBE", L"present");
    bool found = false;
    for (const std::wstring& e : HostEnvironmentEntries()) found = found || e == L"W10X_ENV_PROBE=present";
    CHECK(found);
    SetEnvironmentVariableW(L"W10X_ENV_PROBE", nullptr);
}

// ---- NtCreateUserProcess -------------------------------------------------------------------------------------------------

namespace {

struct PsAttr { ULONG_PTR Attribute; SIZE_T Size; ULONG_PTR Value; SIZE_T* ReturnLength; };
constexpr ULONG_PTR kInput = 0x20000, kThread = 0x10000;
constexpr ULONG_PTR kImageName = 5 | kInput, kClientId = 3 | kThread, kImageInfo = 6, kTokenAttr = 2 | kInput | 0x40000;

// A normalized RTL_USER_PROCESS_PARAMETERS from the host ntdll (what kernelbase builds with the same call).
void* MakeParams(const std::wstring& image, const std::wstring& cmdline, const std::wstring& cwd) {
    using Fn = NTSTATUS(NTAPI*)(PVOID*, PUNICODE_STRING, PUNICODE_STRING, PUNICODE_STRING, PUNICODE_STRING, PVOID,
                                PUNICODE_STRING, PUNICODE_STRING, PUNICODE_STRING, PUNICODE_STRING, ULONG);
    auto fn = reinterpret_cast<Fn>(HostExport("RtlCreateProcessParametersEx"));
    if (!fn) return nullptr;
    auto us = [](const std::wstring& s) {
        return UNICODE_STRING{static_cast<USHORT>(s.size() * 2), static_cast<USHORT>(s.size() * 2 + 2),
                              const_cast<PWSTR>(s.c_str())};
    };
    UNICODE_STRING i = us(image), c = us(cmdline), d = us(cwd), title = us(image), desktop = us(L"WinSta0\\Default"),
                   empty = us(L"");
    static const wchar_t env[] = L"A=1\0B=2\0";   // double-NUL terminated by the literal's own NUL
    void* params = nullptr;
    NTSTATUS s = fn(&params, &i, nullptr, &d, &c, const_cast<wchar_t*>(env), &title, &desktop, &empty, &empty, 1);
    return NtSuccess(s) ? params : nullptr;
}

void FreeParams(void* params) {
    using Fn = PVOID(NTAPI*)(PVOID);
    if (auto fn = reinterpret_cast<Fn>(HostExport("RtlDestroyProcessParameters"))) fn(params);
}

struct Machine {
    std::wstring root;
    Machine() {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        root = std::wstring(tmp) + L"w10x-spawn-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW((root + L"\\guest").c_str(), nullptr);
        CreateDirectoryW((root + L"\\Windows").c_str(), nullptr);
        CopyFileW((TestBinDir() + L"\\guest\\guest_hello.exe").c_str(), (root + L"\\guest\\guest_hello.exe").c_str(), FALSE);
        CopyFileW((TestBinDir() + L"\\guest\\guest_dll.dll").c_str(), (root + L"\\guest\\guest_dll.dll").c_str(), FALSE);
        HANDLE f = CreateFileW((root + L"\\guest\\notanexe.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, "just text, no MZ header", 23, &w, nullptr);
            CloseHandle(f);
        }
        CHECK_EQ(vfs::Initialize(root), kSuccess);
    }
    ~Machine() {
        DeleteFileW((root + L"\\guest\\guest_hello.exe").c_str());
        DeleteFileW((root + L"\\guest\\notanexe.txt").c_str());
        DeleteFileW((root + L"\\guest\\guest_dll.dll").c_str());
        RemoveDirectoryW((root + L"\\guest").c_str());
        RemoveDirectoryW((root + L"\\Windows").c_str());
        RemoveDirectoryW(root.c_str());
    }
};

// Restores the config and the spawn seam when a test ends.
struct ChildConfig {
    bool saved_flag;
    std::wstring saved_root;
    explicit ChildConfig(const std::wstring& root) {
        saved_flag = config::Current().child_processes;
        saved_root = config::Current().machine_root;
        config::Current().child_processes = true;
        config::Current().machine_root = root;
        // The spawned "host" is a plain guest program: it ignores the command line, prints, exits 42.
        SetChildHostExecutableForTest(TestBinDir() + L"\\guest\\guest_hello.exe");
    }
    ~ChildConfig() {
        config::Current().child_processes = saved_flag;
        config::Current().machine_root = saved_root;
        SetChildHostExecutableForTest(L"");
    }
};

NTSTATUS CreateChild(const std::wstring& image_nt, void* params, std::vector<PsAttr> extra, ULONG thread_flags,
                     HANDLE* process, HANDLE* thread, CLIENT_ID* id, std::uint8_t* image_info, std::uint8_t* create_info) {
    std::vector<PsAttr> attrs;
    SIZE_T return_length = 0;
    if (!image_nt.empty())
        attrs.push_back({kImageName, image_nt.size() * 2, reinterpret_cast<ULONG_PTR>(image_nt.c_str()), nullptr});
    attrs.push_back({kClientId, sizeof(CLIENT_ID), reinterpret_cast<ULONG_PTR>(id), nullptr});
    attrs.push_back({kImageInfo, 0x40, reinterpret_cast<ULONG_PTR>(image_info), &return_length});
    for (const PsAttr& a : extra) attrs.push_back(a);
    // PS_ATTRIBUTE_LIST: TotalLength, then the attributes
    std::vector<std::uint8_t> list(sizeof(SIZE_T) + attrs.size() * sizeof(PsAttr));
    const SIZE_T total = list.size();
    std::memcpy(list.data(), &total, sizeof total);
    std::memcpy(list.data() + sizeof total, attrs.data(), attrs.size() * sizeof(PsAttr));
    Syscall create("NtCreateUserProcess");
    NTSTATUS s = create(process, thread, 0x1FFFFF, 0x1FFFFF, nullptr, nullptr, 0, thread_flags, params, create_info, list.data());
    if (NtSuccess(s)) CHECK_EQ(return_length, static_cast<SIZE_T>(0x40));
    return s;
}

}  // namespace

TEST(syscalls_process_create_user_process_refused_when_off) {
    Initialize();
    const bool saved = config::Current().child_processes;
    config::Current().child_processes = false;   // the UWP host's setting
    HANDLE p = reinterpret_cast<HANDLE>(0x11), t = reinterpret_cast<HANDLE>(0x22);
    std::uint8_t info[0x58];
    // Nothing is parsed, nothing started, nothing written.
    CHECK_EQ(Syscall("NtCreateUserProcess")(&p, &t, 0x1FFFFF, 0x1FFFFF, nullptr, nullptr, 0, 1, nullptr, info, nullptr),
             kNotSupported);
    CHECK(p == reinterpret_cast<HANDLE>(0x11) && t == reinterpret_cast<HANDLE>(0x22));
    config::Current().child_processes = saved;
}

TEST(syscalls_process_create_user_process_argument_errors) {
    Machine machine;
    ChildConfig cfg(machine.root);
    HANDLE p = nullptr, t = nullptr;
    CLIENT_ID id{};
    std::uint8_t image_info[0x40], create_info[0x58];
    void* params = MakeParams(L"C:\\guest\\guest_hello.exe", L"C:\\guest\\guest_hello.exe", L"C:\\");
    CHECK(params != nullptr);
    if (!params) return;
    const std::wstring hello = L"\\??\\C:\\guest\\guest_hello.exe";

    // No parameter block, no image name.
    CHECK_EQ(CreateChild(hello, nullptr, {}, 0, &p, &t, &id, image_info, create_info), kInvalidParameter);
    CHECK_EQ(CreateChild(L"", params, {}, 0, &p, &t, &id, image_info, create_info), kInvalidParameter);
    // A file that is not there, a file that is not a program.
    CHECK_EQ(CreateChild(L"\\??\\C:\\guest\\nope.exe", params, {}, 0, &p, &t, &id, image_info, create_info),
             kObjectNameNotFound);
    CHECK_EQ(CreateChild(L"\\??\\C:\\guest\\notanexe.txt", params, {}, 0, &p, &t, &id, image_info, create_info),
             kInvalidImageNotMz);
    // A DLL is not something a process can be started from.
    CHECK_EQ(CreateChild(L"\\??\\C:\\guest\\guest_dll.dll", params, {}, 0, &p, &t, &id, image_info, create_info),
             kInvalidImageFormat);
    CHECK_EQ(CreateChild(L"\\??\\C:\\guest\\notanexe.txt", params, {}, 0, &p, &t, &id, image_info, create_info),
             kInvalidImageNotMz);
    // PS_CREATE_INFO says which stage failed (PsCreateFailExeFormat = 3).
    std::uint32_t state = 0;
    std::memcpy(&state, create_info + 8, 4);
    CHECK_EQ(state, 3u);
    // Another token: the guest's tokens are emulated, a host child cannot run under one.
    HANDLE token = reinterpret_cast<HANDLE>(-4);
    CHECK_EQ(CreateChild(hello, params, {{kTokenAttr, sizeof(HANDLE), reinterpret_cast<ULONG_PTR>(token), nullptr}}, 0, &p,
                         &t, &id, image_info, create_info), kNotSupported);
    // Hide-from-debugger is not a thing the layer can honour: Windows and Wine refuse it as well.
    CHECK_EQ(CreateChild(hello, params, {}, 4, &p, &t, &id, image_info, create_info), kInvalidParameter);
    CHECK(p == nullptr && t == nullptr);   // nothing was started for any of those
    FreeParams(params);
}

TEST(syscalls_process_create_user_process_runs_a_child) {
    Machine machine;
    ChildConfig cfg(machine.root);
    void* params = MakeParams(L"C:\\guest\\guest_hello.exe", L"C:\\guest\\guest_hello.exe", L"C:\\");
    CHECK(params != nullptr);
    if (!params) return;
    // The child's stdout is a pipe whose write end is NOT inheritable: the layer has to make it so for the spawn.
    HANDLE read_end = nullptr, write_end = nullptr;
    CHECK(CreatePipe(&read_end, &write_end, nullptr, 0));
    constexpr std::uint32_t kStdOutOffset = 0x28;
    std::memcpy(static_cast<std::uint8_t*>(params) + kStdOutOffset, &write_end, sizeof write_end);

    HANDLE p = nullptr, t = nullptr;
    CLIENT_ID id{};
    std::uint8_t image_info[0x40] = {}, create_info[0x58];
    std::memset(create_info, 0xCC, sizeof create_info);
    NTSTATUS s = CreateChild(L"\\??\\C:\\guest\\guest_hello.exe", params, {}, 1 /*suspended*/, &p, &t, &id, image_info,
                             create_info);
    CHECK_EQ(s, kSuccess);
    if (s == kSuccess) {
        CHECK(p != nullptr && t != nullptr);
        CHECK(id.UniqueProcess != nullptr && id.UniqueThread != nullptr);
        CHECK_EQ(GetProcessId(p), static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(id.UniqueProcess)));
        CHECK_EQ(GetThreadId(t), static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(id.UniqueThread)));
        // SECTION_IMAGE_INFORMATION of the child's image: x64, not a DLL, with a real file size.
        USHORT machine_type, characteristics;
        std::uint32_t file_size;
        std::memcpy(&machine_type, image_info + 0x30, 2);
        std::memcpy(&characteristics, image_info + 0x2C, 2);
        std::memcpy(&file_size, image_info + 0x38, 4);
        CHECK_EQ(machine_type, static_cast<USHORT>(0x8664));
        CHECK(!(characteristics & IMAGE_FILE_DLL));
        CHECK(file_size > 1000);
        std::uint32_t state, size_lo;
        std::memcpy(&size_lo, create_info, 4);
        std::memcpy(&state, create_info + 8, 4);
        CHECK_EQ(size_lo, 0x58u);
        CHECK_EQ(state, 6u);   // PsCreateSuccess

        // Created suspended: it has not run.
        CHECK_EQ(WaitForSingleObject(p, 300), static_cast<DWORD>(WAIT_TIMEOUT));
        CHECK_EQ(ResumeThread(t), 1u);
        CHECK_EQ(WaitForSingleObject(p, 60000), static_cast<DWORD>(WAIT_OBJECT_0));
        DWORD code = 0;
        CHECK(GetExitCodeProcess(p, &code));
        CHECK_EQ(code, 42u);   // the exit code of the host child comes back through the handle
        CloseHandle(t);
        CloseHandle(p);
    }
    CloseHandle(write_end);
    char buf[128] = {};
    DWORD got = 0;
    ReadFile(read_end, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(read_end);
    CHECK_MSG(std::strstr(buf, "hello from guest") != nullptr, "child output: [%s]", buf);
    FreeParams(params);
}
