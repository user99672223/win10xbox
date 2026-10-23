// User-pointer probing (core/dispatch/probe.cpp, core/include/win10xbox/probe.h): a bad pointer handed to an
// Emulate/Translate syscall comes back as the status Windows gives, instead of faulting; a NULL that Windows accepts
// stays accepted. The syscalls are called directly, through the same entry points the dispatcher's tables hold:
// the clock (NtQueryPerformanceCounter), the registry (NtOpenKey) and the token module (NtAdjustPrivilegesToken),
// plus the file translator's attribute capture (TranslateOA, which every Nt*File call goes through).
#include "check.h"
#include "win10xbox/clock.h"
#include "win10xbox/probe.h"
#include "win10xbox/registry.h"
#include "win10xbox/token.h"
#include "../../core/dispatch/internal.h"

#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace probe = win10xbox::probe;
namespace dsp = win10xbox::dispatch::detail;

namespace {

constexpr NTSTATUS kAccessViolation = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kMisaligned = static_cast<NTSTATUS>(0x80000002);
constexpr NTSTATUS kInvalidParameter = static_cast<NTSTATUS>(0xC000000D);

// A pointer that no mapping can satisfy: the shape the Wine tests use (0xdeadbee0), and NULL.
template <class T> T* Wild() { return reinterpret_cast<T*>(static_cast<std::uintptr_t>(0xdeadbee0)); }

// Two committed pages followed by a no-access one (and, optionally, a read-only first page).
struct Pages {
    std::uint8_t* base = nullptr;
    Pages() {
        base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        if (base) VirtualProtect(base + 0x2000, 0x1000, PAGE_NOACCESS, &old);
    }
    ~Pages() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
};

}  // namespace

TEST(probe_read_write_primitives) {
    Pages p;
    CHECK(p.base != nullptr);
    std::memset(p.base, 0x5A, 0x2000);

    CHECK_EQ(probe::ProbeRead(p.base, 0x2000), 0);
    CHECK_EQ(probe::ProbeWrite(p.base, 0x2000), 0);
    CHECK_EQ(p.base[0], 0x5A);              // ProbeWrite writes nothing
    CHECK_EQ(p.base[0x1FFF], 0x5A);
    CHECK_EQ(probe::ProbeRead(nullptr, 4), kAccessViolation);
    CHECK_EQ(probe::ProbeWrite(Wild<void>(), 4), kAccessViolation);
    CHECK_EQ(probe::ProbeRead(p.base + 0x1FF0, 0x20), kAccessViolation);   // runs into the no-access page
    CHECK_EQ(probe::ProbeWrite(p.base + 0x2000, 1), kAccessViolation);
    CHECK_EQ(probe::ProbeRead(reinterpret_cast<void*>(probe::kUserProbeAddress), 1), kAccessViolation);
    CHECK_EQ(probe::ProbeRead(p.base + 0x1FFF, ~std::size_t{0}), kAccessViolation);                       // wraps

    // A zero length accepts any pointer without looking at it; alignment is only checked for a real range.
    CHECK_EQ(probe::ProbeRead(nullptr, 0), 0);
    CHECK_EQ(probe::ProbeWrite(Wild<void>(), 0, 8), 0);
    CHECK_EQ(probe::ProbeRead(p.base + 1, 4, 4), kMisaligned);
    CHECK_EQ(probe::ProbeWrite(p.base + 2, 8, 8), kMisaligned);
    CHECK_EQ(probe::ProbeRead(p.base + 4, 4, 4), 0);

    // A read-only page can be read and probed for reading but not for writing.
    DWORD old;
    VirtualProtect(p.base, 0x1000, PAGE_READONLY, &old);
    CHECK_EQ(probe::ProbeRead(p.base, 0x1000), 0);
    CHECK_EQ(probe::ProbeWrite(p.base, 0x1000), kAccessViolation);
    CHECK_EQ(probe::ProbeWrite(p.base + 0x1000, 0x1000), 0);
}

TEST(probe_copy_reports_the_fault_and_nothing_else) {
    Pages p;
    CHECK(p.base != nullptr);
    for (int i = 0; i < 0x2000; ++i) p.base[i] = static_cast<std::uint8_t>(i);
    std::uint8_t local[0x40] = {};

    CHECK_EQ(probe::CopyFromUser(local, p.base + 3, sizeof local), 0);
    CHECK_EQ(local[0], 3);
    CHECK_EQ(local[0x3F], 0x42);
    CHECK_EQ(probe::CopyFromUser(local, p.base + 0x1FF0, sizeof local), kAccessViolation);   // crosses into the hole
    CHECK_EQ(probe::CopyFromUser(local, nullptr, 1), kAccessViolation);
    CHECK_EQ(probe::CopyToUser(Wild<void>(), local, 1), kAccessViolation);
    std::uint8_t pattern[0x40];
    std::memset(pattern, 0xC3, sizeof pattern);
    CHECK_EQ(probe::CopyToUser(p.base + 0x1000, pattern, sizeof pattern), 0);
    CHECK_EQ(p.base[0x1000], 0xC3);
    CHECK_EQ(p.base[0x103F], 0xC3);
    CHECK_EQ(p.base[0x1040], static_cast<std::uint8_t>(0x1040));
    CHECK_EQ(probe::CopyFromUser(nullptr, nullptr, 0), 0);

    // The handler is the same one for every thread and does not swallow a fault outside the guarded routines:
    // many faults in a row, from several threads, each reported to its own caller.
    struct Arg { int bad = 0; };
    auto thread_fn = [](void* a) -> DWORD {
        auto* arg = static_cast<Arg*>(a);
        std::uint8_t b[8];
        for (int i = 0; i < 2000; ++i)
            if (probe::CopyFromUser(b, Wild<void>(), sizeof b) != kAccessViolation) ++arg->bad;
        return 0;
    };
    Arg args[4];
    HANDLE threads[4];
    for (int i = 0; i < 4; ++i)
        threads[i] = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(+thread_fn), &args[i], 0, nullptr);
    WaitForMultipleObjects(4, threads, TRUE, 30000);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(args[i].bad, 0);
        CloseHandle(threads[i]);
    }
}

TEST(probe_capture_object_attributes) {
    std::wstring text = L"\\Registry\\Machine";
    UNICODE_STRING name;
    name.Buffer = text.data();
    name.Length = static_cast<USHORT>(text.size() * 2);
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.ObjectName = &name;

    probe::ObjectAttributes cap;
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &cap), 0);
    CHECK(cap.oa.ObjectName == &cap.name.us);
    CHECK(cap.name.view() == text);

    probe::ObjectAttributes bad;
    CHECK_EQ(probe::CaptureObjectAttributes(Wild<OBJECT_ATTRIBUTES>(), &bad), kAccessViolation);
    oa.Length = sizeof oa * 2;
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), kInvalidParameter);
    oa.Length = sizeof oa;
    oa.ObjectName = Wild<UNICODE_STRING>();
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), kAccessViolation);
    oa.ObjectName = &name;
    name.Buffer = Wild<wchar_t>();
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), kAccessViolation);
    name.Buffer = reinterpret_cast<wchar_t*>(reinterpret_cast<std::uint8_t*>(text.data()) + 1);
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), kMisaligned);   // a name must be WCHAR-aligned
    name.Length = 0;
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), 0);             // an empty name is not read at all
    oa.ObjectName = nullptr;
    CHECK_EQ(probe::CaptureObjectAttributes(&oa, &bad), 0);             // an unnamed object is fine
    CHECK(bad.oa.ObjectName == nullptr);
}

TEST(probe_clock_syscalls) {
    namespace clk = win10xbox::clock;
    clk::Initialize(clk::Settings{});
    LARGE_INTEGER counter{}, freq{};
    // Windows: the counter is required, the frequency is optional but, when given, must be writable.
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(nullptr, nullptr), kAccessViolation);
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(nullptr, &freq), kAccessViolation);
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(&counter, Wild<LARGE_INTEGER>()), kAccessViolation);
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(Wild<LARGE_INTEGER>(), &freq), kAccessViolation);
    CHECK_EQ(freq.QuadPart, 0);       // nothing was written on the way to the failure
    CHECK_EQ(counter.QuadPart, 0);
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(&counter, nullptr), 0);          // NULL frequency stays accepted
    CHECK(counter.QuadPart >= 0);
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(&counter, &freq), 0);
    CHECK_EQ(freq.QuadPart, 10'000'000);

    CHECK_EQ(clk::Clk_NtQuerySystemTime(nullptr), kAccessViolation);
    CHECK_EQ(clk::Clk_NtSetSystemTime(Wild<LARGE_INTEGER>(), nullptr), kAccessViolation);
    LARGE_INTEGER bad_old_target; bad_old_target.QuadPart = 0x01D0000000000000;
    CHECK_EQ(clk::Clk_NtSetSystemTime(&bad_old_target, Wild<LARGE_INTEGER>()), kAccessViolation);
    CHECK_EQ(clk::Clk_NtDelayExecution(FALSE, Wild<LARGE_INTEGER>()), kAccessViolation);
    CHECK_EQ(clk::Clk_NtQueryTimerResolution(nullptr, nullptr, nullptr), kAccessViolation);
    ULONG min = 0, max = 0, cur = 0;
    CHECK_EQ(clk::Clk_NtQueryTimerResolution(&min, &max, &cur), 0);
    CHECK_EQ(clk::Clk_NtQueryTimerResolution(&min, &max, nullptr), kAccessViolation);
    CHECK_EQ(clk::Clk_NtSetTimerResolution(0, FALSE, nullptr), kAccessViolation);
    CHECK_EQ(clk::Clk_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(FALSE, nullptr, nullptr, nullptr),
             kAccessViolation);
    ULONGLONG v = 1, q = 2;
    CHECK_EQ(clk::Clk_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(FALSE, &v, &q, nullptr),
             static_cast<NTSTATUS>(0xC00000BB));   // STATUS_NOT_SUPPORTED, error output optional
    CHECK_EQ(q, 2);
    clk::Initialize(clk::Settings{});
}

TEST(probe_registry_syscalls) {
    namespace reg = win10xbox::registry;
    HANDLE key = reinterpret_cast<HANDLE>(0x77);
    std::wstring text = L"\\Registry\\Machine";
    UNICODE_STRING name;
    name.Buffer = text.data();
    name.Length = static_cast<USHORT>(text.size() * 2);
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.ObjectName = &name;
    constexpr ACCESS_MASK kRead = 0x20019;

    CHECK_EQ(reg::Reg_NtOpenKey(nullptr, kRead, nullptr), kAccessViolation);
    CHECK_EQ(reg::Reg_NtOpenKey(&key, kRead, nullptr), kAccessViolation);
    CHECK_EQ(reg::Reg_NtOpenKey(nullptr, kRead, &oa), kAccessViolation);
    CHECK_EQ(reg::Reg_NtOpenKey(Wild<HANDLE>(), kRead, &oa), kAccessViolation);
    CHECK_EQ(key, reinterpret_cast<HANDLE>(0x77));          // an argument error leaves the handle alone
    CHECK_EQ(reg::Reg_NtOpenKey(&key, kRead, Wild<OBJECT_ATTRIBUTES>()), kAccessViolation);
    oa.ObjectName = Wild<UNICODE_STRING>();
    CHECK_EQ(reg::Reg_NtOpenKey(&key, kRead, &oa), kAccessViolation);
    oa.ObjectName = &name;
    name.Buffer = Wild<wchar_t>();
    CHECK_EQ(reg::Reg_NtOpenKey(&key, kRead, &oa), kAccessViolation);
    name.Buffer = text.data();
    oa.Length *= 2;
    CHECK_EQ(reg::Reg_NtOpenKey(&key, kRead, &oa), kInvalidParameter);

    ULONG disposition = 0;
    HANDLE created = nullptr;
    CHECK_EQ(reg::Reg_NtCreateKey(&created, kRead, nullptr, 0, nullptr, 0, &disposition), kAccessViolation);
    CHECK_EQ(reg::Reg_NtCreateKey(&created, kRead, nullptr, 0, nullptr, 0, nullptr), kAccessViolation);   // NULL Disposition is accepted, NULL attributes not
    oa.Length = sizeof oa;
    CHECK_EQ(reg::Reg_NtCreateKey(&created, kRead, &oa, 0, Wild<UNICODE_STRING>(), 0, nullptr), kAccessViolation);
    CHECK_EQ(reg::Reg_NtCreateKey(&created, kRead, &oa, 0, nullptr, 0, Wild<ULONG>()), kAccessViolation);

    ULONG len = 0;
    CHECK_EQ(reg::Reg_NtQueryValueKey(nullptr, nullptr, 2, nullptr, 0, &len), kAccessViolation);
    CHECK_EQ(reg::Reg_NtSetValueKey(nullptr, Wild<UNICODE_STRING>(), 0, REG_DWORD, nullptr, 0), kAccessViolation);
    UNICODE_STRING value;
    std::wstring value_text = L"v";
    value.Buffer = value_text.data();
    value.Length = 2;
    value.MaximumLength = 2;
    CHECK_EQ(reg::Reg_NtSetValueKey(nullptr, &value, 0, REG_BINARY, Wild<void>(), 4), kAccessViolation);
}

TEST(probe_token_syscalls) {
    namespace tok = win10xbox::token;
    tok::Initialize(tok::Identity{});
    HANDLE h = nullptr;
    CHECK_EQ(tok::Tok_NtOpenProcessToken(reinterpret_cast<HANDLE>(-1), TOKEN_ALL_ACCESS, nullptr), kAccessViolation);
    CHECK_EQ(tok::Tok_NtOpenProcessToken(reinterpret_cast<HANDLE>(-1), TOKEN_ALL_ACCESS, Wild<HANDLE>()), kAccessViolation);
    CHECK_EQ(tok::Tok_NtOpenProcessToken(reinterpret_cast<HANDLE>(-1), TOKEN_ALL_ACCESS, &h), 0);
    CHECK(h != nullptr);

    ULONG need = 0;
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, static_cast<TOKEN_INFORMATION_CLASS>(1), nullptr, 0, &need),
             static_cast<NTSTATUS>(0xC0000023));   // STATUS_BUFFER_TOO_SMALL: NULL with length 0 is a size query
    CHECK(need > 0);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, static_cast<TOKEN_INFORMATION_CLASS>(1), nullptr, need, &need),
             kAccessViolation);
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, static_cast<TOKEN_INFORMATION_CLASS>(1), Wild<void>(), need, &need),
             kAccessViolation);
    std::uint8_t buf[512] = {};
    CHECK_EQ(tok::Tok_NtQueryInformationToken(h, static_cast<TOKEN_INFORMATION_CLASS>(1), buf, sizeof buf, Wild<ULONG>()),
             kAccessViolation);

    // PreviousState and ReturnLength are optional; a NewState that is not readable is not.
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(h, TRUE, nullptr, 0, nullptr, nullptr), 0);
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(h, FALSE, Wild<TOKEN_PRIVILEGES>(), 0, nullptr, nullptr), kAccessViolation);
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(h, FALSE, nullptr, 0, nullptr, nullptr), kInvalidParameter);
    CHECK_EQ(tok::Tok_NtAdjustPrivilegesToken(h, TRUE, nullptr, 0, nullptr, Wild<ULONG>()), kAccessViolation);

    BOOLEAN equal = 7;
    CHECK_EQ(tok::Tok_NtCompareTokens(h, h, nullptr), kAccessViolation);
    CHECK_EQ(tok::Tok_NtCompareTokens(h, h, Wild<BOOLEAN>()), kAccessViolation);
    CHECK_EQ(tok::Tok_NtCompareTokens(h, h, &equal), 0);
    CHECK_EQ(equal, TRUE);

    ACCESS_MASK granted = 0;
    NTSTATUS access = 0;
    GENERIC_MAPPING gm{1, 2, 4, 7};
    CHECK_EQ(tok::Tok_NtAccessCheck(Wild<void>(), h, 1, &gm, nullptr, nullptr, &granted, &access), kAccessViolation);
    CHECK_EQ(tok::Tok_NtAccessCheck(Wild<void>(), h, 1, Wild<GENERIC_MAPPING>(), nullptr, nullptr, &granted, &access),
             kAccessViolation);
    CHECK_EQ(tok::Close(h), 0);
}

TEST(probe_file_translator_attribute_capture) {
    // Every Nt*File call reads the caller's OBJECT_ATTRIBUTES through TranslateOA: no attributes is the host's to
    // answer, unreadable ones are an access violation, a misaligned name is a misalignment.
    dsp::TranslatedOA t;
    CHECK_EQ(dsp::TranslateOA(nullptr, &t), 0);
    CHECK_EQ(dsp::TranslateOA(Wild<OBJECT_ATTRIBUTES>(), &t), kAccessViolation);
    std::wstring text = L"\\??\\C:\\x";
    UNICODE_STRING name;
    name.Buffer = text.data();
    name.Length = static_cast<USHORT>(text.size() * 2);
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof oa;
    oa.ObjectName = Wild<UNICODE_STRING>();
    CHECK_EQ(dsp::TranslateOA(&oa, &t), kAccessViolation);
    oa.ObjectName = &name;
    name.Buffer = Wild<wchar_t>();
    CHECK_EQ(dsp::TranslateOA(&oa, &t), kAccessViolation);
    name.Buffer = reinterpret_cast<wchar_t*>(reinterpret_cast<std::uint8_t*>(text.data()) + 1);
    CHECK_EQ(dsp::TranslateOA(&oa, &t), kMisaligned);
    oa.ObjectName = nullptr;
    CHECK_EQ(dsp::TranslateOA(&oa, &t), 0);   // unnamed: nothing to translate
}
