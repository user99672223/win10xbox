// Unit tests of the driver host: load the real test drivers (PE images built from
// tests/drivers/*.c), drive them through the Drv_* entry points the dispatcher uses,
// and check the ntoskrnl surface from the host side. No guest process involved, so
// these run wherever the host does (the guest end-to-end test is integration.driver_ioctl).
//   test_drivers <directory containing OurTestDevice.sys, OurStubProbe.sys, OurFailDriver.sys>
#include "check.h"
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/drivers.h"
#include "ntoskrnl/internal.h"
#include "our_test_device.h"
#include "our_kmdf_device.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CHECK_ST(a, b) CHECK_EQ(static_cast<NTSTATUS>(a), static_cast<NTSTATUS>(b))

using namespace win10xbox::drivers;
namespace dt = win10xbox::drivers::detail;

namespace {
std::wstring g_dir;

std::wstring DriverPath(const wchar_t* file) { return g_dir + L"\\" + file; }

std::wstring ToHostPath(std::string p) {
    std::wstring w(p.begin(), p.end());
    if (!w.empty() && w[0] == L'/') w = L"Z:" + w;
    for (auto& c : w) if (c == L'/') c = L'\\';
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return w;
}

UNICODE_STRING Us(const wchar_t* s) {
    UNICODE_STRING u;
    u.Buffer = const_cast<wchar_t*>(s);
    u.Length = static_cast<USHORT>(std::wcslen(s) * 2);
    u.MaximumLength = u.Length + 2;
    return u;
}

NTSTATUS Open(const wchar_t* name, HANDLE* h, ULONG options = FILE_SYNCHRONOUS_IO_NONALERT) {
    UNICODE_STRING us = Us(name);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK iosb{};
    *h = nullptr;
    return Drv_NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, nullptr, 0, 0, FILE_OPEN, options, nullptr, 0);
}

NTSTATUS Ioctl(HANDLE h, ULONG code, void* in, ULONG in_len, void* out, ULONG out_len, ULONG_PTR* info = nullptr,
               HANDLE event = nullptr, IO_STATUS_BLOCK* iosb_out = nullptr) {
    IO_STATUS_BLOCK iosb{};
    NTSTATUS st = Drv_NtDeviceIoControlFile(h, event, nullptr, nullptr, iosb_out ? iosb_out : &iosb, code, in, in_len, out, out_len);
    if (info) *info = (iosb_out ? iosb_out : &iosb)->Information;
    return st;
}

struct Loaded {
    DriverInfo info;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    explicit Loaded(const wchar_t* file) { st = LoadDriver(DriverPath(file).c_str(), nullptr, &info); }
    ~Loaded() { if (NT_SUCCESS(st)) UnloadDriver(info.id); }
};

using SnprintfFn = int(__cdecl*)(char*, size_t, const char*, ...);
using SprintfFn = int(__cdecl*)(char*, const char*, ...);
using SnwprintfFn = int(__cdecl*)(wchar_t*, size_t, const wchar_t*, ...);

}  // namespace

TEST(load_creates_device_and_link) {
    Loaded d(L"OurTestDevice.sys");
    CHECK_MSG(NT_SUCCESS(d.st), "LoadDriver status %08lx", static_cast<unsigned long>(d.st));
    if (!NT_SUCCESS(d.st)) return;
    CHECK(d.info.name == L"\\Driver\\OurTestDevice");
    CHECK(d.info.driver_object != nullptr);
    bool found = false;
    for (auto& di : EnumerateDevices())
        if (di.name == L"\\Device\\OurTestDevice") {
            found = true;
            CHECK_EQ(di.device_type, 0x22u);
            CHECK(di.flags & DO_BUFFERED_IO);
            CHECK(!(di.flags & DO_DEVICE_INITIALIZING));   // cleared after DriverEntry for legacy drivers
            CHECK(di.driver == L"\\Driver\\OurTestDevice");
        }
    CHECK(found);
    bool link = false;
    for (auto& l : EnumerateSymbolicLinks())
        if (l.second == L"\\Device\\OurTestDevice") link = true;
    CHECK(link);
    std::wstring dev;
    CHECK(ResolveName(L"\\??\\OurTestDevice", &dev));
    CHECK(dev == L"\\Device\\OurTestDevice");
    CHECK(ResolveName(L"\\DosDevices\\ourtestdevice", &dev));
    CHECK(ResolveName(L"\\GLOBAL??\\OURTESTDEVICE", &dev));
    CHECK(ResolveName(L"\\Device\\OurTestDevice\\sub\\path", &dev));
    CHECK(dev == L"\\Device\\OurTestDevice\\sub\\path");
    CHECK(!ResolveName(L"\\??\\NoSuchDevice", &dev));
    CHECK(!ResolveName(L"\\Device\\Other", &dev));
}

TEST(buffered_ioctl_echo_transform) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    CHECK(OwnsHandle(h));
    unsigned char in[] = "driver host says hello", out[sizeof(in)] = {};
    ULONG_PTR info = 0;
    NTSTATUS st = Ioctl(h, IOCTL_OTD_XOR, in, sizeof in, out, sizeof out, &info);
    CHECK_ST(st, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof in);
    bool same = true;
    for (size_t i = 0; i < sizeof in; ++i) same = same && out[i] == (in[i] ^ OTD_XOR_KEY);
    CHECK(same);
    // too small an output buffer: the driver's status is returned as is
    CHECK_ST(Ioctl(h, IOCTL_OTD_XOR, in, sizeof in, out, 3), STATUS_BUFFER_TOO_SMALL);
    // unknown code
    CHECK_ST(Ioctl(h, CTL_CODE(0x22, 0x999, METHOD_BUFFERED, FILE_ANY_ACCESS), nullptr, 0, nullptr, 0), STATUS_INVALID_DEVICE_REQUEST);
    CHECK_ST(Drv_NtClose(h), STATUS_SUCCESS);
    CHECK(!OwnsHandle(h));
}

TEST(direct_and_neither_ioctls) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    static unsigned char big[10000];
    unsigned char pat = 0x3C;
    ULONG_PTR info = 0;
    CHECK_ST(Ioctl(h, IOCTL_OTD_FILL, &pat, 1, big, sizeof big, &info), STATUS_SUCCESS);
    CHECK_EQ(info, sizeof big);
    bool all = true;
    for (unsigned char c : big) all = all && c == 0x3C;
    CHECK(all);
    ULONG sum = 0;
    for (size_t i = 0; i < 700; ++i) { big[i] = static_cast<unsigned char>(i); sum += big[i]; }
    CHECK_ST(Ioctl(h, IOCTL_OTD_SUM, &pat, 1, big, 700, &info), STATUS_SUCCESS);
    CHECK_EQ(info, static_cast<ULONG_PTR>(sum));
    ULONG v = 99, r = 0;
    CHECK_ST(Ioctl(h, IOCTL_OTD_NEITHER, &v, sizeof v, &r, sizeof r), STATUS_SUCCESS);
    CHECK_EQ(r, 100u);
    Drv_NtClose(h);
}

TEST(pending_irp_completes_from_dpc_on_worker) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    OTD_DEFERRED_OUT out{};
    ULONG_PTR info = 0;
    CHECK_ST(Ioctl(h, IOCTL_OTD_DEFERRED, nullptr, 0, &out, sizeof out, &info), STATUS_SUCCESS);
    CHECK_EQ(out.Magic, OTD_DEFERRED_MAGIC);
    CHECK_EQ(out.DispatchIrql, 0ul);
    CHECK_EQ(out.DpcIrql, 2ul);                       // DISPATCH_LEVEL on the worker
    CHECK_EQ(out.CallerThreadId, GetCurrentThreadId());
    CHECK(out.DpcThreadId != GetCurrentThreadId());
    CHECK_EQ(GetVirtualIrql(), 0);                    // the caller thread never left PASSIVE_LEVEL
    Drv_NtClose(h);
}

TEST(async_handle_pending_event_and_iosb) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h, 0), STATUS_SUCCESS);   // no FILE_SYNCHRONOUS_IO_*: overlapped semantics
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OTD_DEFERRED_OUT out{};
    IO_STATUS_BLOCK iosb{};
    iosb.Status = static_cast<NTSTATUS>(0x103);
    NTSTATUS st = Ioctl(h, IOCTL_OTD_DEFERRED, nullptr, 0, &out, sizeof out, nullptr, ev, &iosb);
    CHECK_ST(st, STATUS_PENDING);
    CHECK_EQ(WaitForSingleObject(ev, 5000), WAIT_OBJECT_0);
    CHECK_ST(iosb.Status, STATUS_SUCCESS);
    CHECK_EQ(iosb.Information, sizeof out);
    CHECK_EQ(out.Magic, OTD_DEFERRED_MAGIC);
    CloseHandle(ev);
    Drv_NtClose(h);
}

TEST(timer_dpc_completes_request) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    ULONG ms = 150, elapsed = 0;
    DWORD t0 = GetTickCount();
    CHECK_ST(Ioctl(h, IOCTL_OTD_TIMER, &ms, sizeof ms, &elapsed, sizeof elapsed), STATUS_SUCCESS);
    DWORD wall = GetTickCount() - t0;
    CHECK_MSG(wall >= 120 && wall < 5000, "wall=%lu", wall);
    CHECK_MSG(elapsed >= 120 && elapsed < 5000, "driver elapsed=%lu", elapsed);
    Drv_NtClose(h);
}

TEST(close_cancels_pending_request) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h, 0), STATUS_SUCCESS);
    ULONG ms = 60000, elapsed = 0;
    IO_STATUS_BLOCK iosb{};
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_ST(Ioctl(h, IOCTL_OTD_TIMER, &ms, sizeof ms, &elapsed, sizeof elapsed, nullptr, ev, &iosb), STATUS_PENDING);
    DWORD t0 = GetTickCount();
    CHECK_ST(Drv_NtClose(h), STATUS_SUCCESS);     // cancels through the driver's cancel routine, then cleanup + close
    CHECK_MSG(GetTickCount() - t0 < 5000, "close took %lu ms", GetTickCount() - t0);
    CHECK_EQ(WaitForSingleObject(ev, 1000), WAIT_OBJECT_0);
    CHECK_ST(iosb.Status, STATUS_CANCELLED);
    CloseHandle(ev);
}

TEST(concurrent_requests_from_many_threads) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    HANDLE shared;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &shared), STATUS_SUCCESS);   // one handle used by every thread, plus one per thread
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            HANDLE h;
            if (!NT_SUCCESS(Open(L"\\Device\\OurTestDevice", &h))) { ++failures; return; }
            for (int i = 0; i < 250; ++i) {
                unsigned char in[32], out[32];
                for (int k = 0; k < 32; ++k) in[k] = static_cast<unsigned char>(t * 31 + i + k);
                HANDLE use = (i & 1) ? shared : h;
                if (Ioctl(use, IOCTL_OTD_XOR, in, 32, out, 32) != STATUS_SUCCESS) { ++failures; continue; }
                for (int k = 0; k < 32; ++k)
                    if (out[k] != (in[k] ^ OTD_XOR_KEY)) { ++failures; break; }
            }
            Drv_NtClose(h);
        });
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
    OTD_INFO info{};
    CHECK_ST(Ioctl(shared, IOCTL_OTD_INFO, nullptr, 0, &info, sizeof info), STATUS_SUCCESS);
    CHECK_EQ(info.OpenCount, 1ul);
    Drv_NtClose(shared);
}

TEST(interrupt_service_routine_runs_on_worker_at_dirql) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    CHECK(!InjectInterrupt(0x99));          // nothing connected there
    for (int i = 0; i < 5; ++i) CHECK(InjectInterrupt(OTD_INTERRUPT_VECTOR));
    OTD_INFO info{};
    for (int i = 0; i < 200; ++i) {
        CHECK_ST(Ioctl(h, IOCTL_OTD_INFO, nullptr, 0, &info, sizeof info), STATUS_SUCCESS);
        if (info.IsrCount == 5) break;
        Sleep(10);
    }
    CHECK_EQ(info.IsrCount, 5ul);
    CHECK_EQ(info.IsrIrql, static_cast<unsigned long>(OTD_ISR_IRQL));
    CHECK_EQ(GetVirtualIrql(), 0);
    Drv_NtClose(h);
}

TEST(structured_exception_handling_inside_a_driver) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    OTD_SEH_OUT r{};
    CHECK_ST(Ioctl(h, IOCTL_OTD_SEH, nullptr, 0, &r, sizeof r), STATUS_SUCCESS);
    CHECK_EQ(r.CaughtRaise, 1ul);
    CHECK_MSG(r.RaiseCode == 0xC000009Aul, "RaiseCode=%08lx", r.RaiseCode);     // ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES) caught by __except
    CHECK_EQ(r.FinallyRan, 1ul);             // the __finally between them ran during unwinding
    CHECK_EQ(r.CaughtFault, 1ul);
    CHECK_MSG(r.FaultCode == 0xC0000005ul, "FaultCode=%08lx", r.FaultCode);     // access violation from a NULL store
    Drv_NtClose(h);
}

TEST(kernel_selftest_all_checks_pass) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    OTD_SELFTEST_OUT r{};
    CHECK_ST(Ioctl(h, IOCTL_OTD_SELFTEST, nullptr, 0, &r, sizeof r), STATUS_SUCCESS);
    CHECK_MSG(r.Failed == 0, "%lu of %lu in-kernel assertions failed", r.Failed, r.Executed);
    CHECK_MSG(r.Executed >= OTD_SELFTEST_MIN_ASSERTIONS, "only %lu assertions ran", r.Executed);
    Drv_NtClose(h);
}

TEST(read_write_and_info) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);
    const char text[] = "persisted in the device extension";
    IO_STATUS_BLOCK iosb{};
    CHECK_ST(Drv_NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, const_cast<char*>(text), sizeof text, nullptr, nullptr), STATUS_SUCCESS);
    CHECK_EQ(iosb.Information, sizeof text);
    char back[64] = {};
    LARGE_INTEGER off{};
    CHECK_ST(Drv_NtReadFile(h, nullptr, nullptr, nullptr, &iosb, back, sizeof back, &off, nullptr), STATUS_SUCCESS);
    CHECK_EQ(iosb.Information, sizeof text);
    CHECK(std::strcmp(back, text) == 0);

    OTD_INFO info{};
    CHECK_ST(Ioctl(h, IOCTL_OTD_INFO, nullptr, 0, &info, sizeof info), STATUS_SUCCESS);
    CHECK_EQ(info.Irql, 0ul);
    CHECK_EQ(info.PreviousMode, 0ul);                 // host thread, not a guest thread: kernel caller
    CHECK_EQ(info.ProcessId, GetCurrentProcessId());
    CHECK_EQ(info.ThreadId, GetCurrentThreadId());
    CHECK_EQ(info.OpenCount, 1ul);
    std::wstring reg(reinterpret_cast<wchar_t*>(info.RegistryPath), info.RegistryPathChars);
    CHECK(reg == L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\OurTestDevice");

    // FileFsDeviceInformation answered by the I/O manager from the device object
    ULONG fsdi[2] = {};
    CHECK_ST(Drv_NtQueryVolumeInformationFile(h, &iosb, fsdi, sizeof fsdi, 4), STATUS_SUCCESS);
    CHECK_EQ(fsdi[0], 0x22u);
    CHECK_EQ(fsdi[1], static_cast<ULONG>(FILE_DEVICE_SECURE_OPEN));
    CHECK_ST(Drv_NtFlushBuffersFile(h, &iosb), STATUS_INVALID_DEVICE_REQUEST);   // the driver has no IRP_MJ_FLUSH_BUFFERS
    Drv_NtClose(h);
}

TEST(open_semantics) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    HANDLE h = nullptr, h2 = nullptr;
    CHECK_ST(Open(L"\\??\\NoSuchDevice", &h), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_ST(Open(L"\\Device\\OURTESTDEVICE", &h), STATUS_SUCCESS);          // object names are case-insensitive
    CHECK_ST(Open(L"\\DosDevices\\OurTestDevice", &h2), STATUS_SUCCESS);     // a second handle
    CHECK(h != h2);
    OTD_INFO info{};
    CHECK_ST(Ioctl(h, IOCTL_OTD_INFO, nullptr, 0, &info, sizeof info), STATUS_SUCCESS);
    CHECK_EQ(info.OpenCount, 2ul);
    Drv_NtClose(h2);
    CHECK_ST(Ioctl(h, IOCTL_OTD_INFO, nullptr, 0, &info, sizeof info), STATUS_SUCCESS);
    CHECK_EQ(info.OpenCount, 1ul);
    CHECK_ST(Drv_NtClose(h), STATUS_SUCCESS);
    CHECK_ST(Drv_NtClose(h), STATUS_INVALID_HANDLE);                          // already closed
    // OwnsPath is what the dispatcher asks first
    UNICODE_STRING us = Us(L"\\??\\OurTestDevice");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, 0, nullptr, nullptr);
    CHECK(OwnsPath(&oa));
    us = Us(L"\\??\\C:\\Windows\\notepad.exe");
    CHECK(!OwnsPath(&oa));
}

TEST(symbolic_link_objects) {
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    UNICODE_STRING us = Us(L"\\??\\OurTestDevice");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    HANDLE h = nullptr;
    CHECK_ST(Drv_NtOpenSymbolicLinkObject(&h, 1, &oa), STATUS_SUCCESS);
    wchar_t buf[64] = {};
    UNICODE_STRING t = {0, sizeof buf, buf};
    ULONG need = 0;
    CHECK_ST(Drv_NtQuerySymbolicLinkObject(h, &t, &need), STATUS_SUCCESS);
    CHECK(std::wstring(t.Buffer, t.Length / 2) == L"\\Device\\OurTestDevice");
    UNICODE_STRING small = {0, 4, buf};
    CHECK_ST(Drv_NtQuerySymbolicLinkObject(h, &small, &need), STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ(need, 42u);
    CHECK_ST(Drv_NtClose(h), STATUS_SUCCESS);
    us = Us(L"\\??\\C:");
    CHECK_ST(Drv_NtOpenSymbolicLinkObject(&h, 1, &oa), STATUS_OBJECT_NAME_NOT_FOUND);
}

TEST(unload_removes_everything) {
    std::uint32_t id = 0;
    {
        DriverInfo info;
        NTSTATUS st = LoadDriver(DriverPath(L"OurTestDevice.sys").c_str(), nullptr, &info);
        CHECK_ST(st, STATUS_SUCCESS);
        id = info.id;
        // a duplicate load of the same driver name is refused
        CHECK_ST(LoadDriver(DriverPath(L"OurTestDevice.sys").c_str()), STATUS_IMAGE_ALREADY_LOADED);
    }
    HANDLE h;
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_SUCCESS);   // left open on purpose: unload closes it
    CHECK_ST(UnloadDriver(id), STATUS_SUCCESS);
    CHECK(!OwnsHandle(h));
    std::wstring dev;
    CHECK(!ResolveName(L"\\??\\OurTestDevice", &dev));
    CHECK(!ResolveName(L"\\Device\\OurTestDevice", &dev));
    CHECK_ST(Open(L"\\??\\OurTestDevice", &h), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_ST(UnloadDriver(id), STATUS_NOT_FOUND);
    // everything the driver allocated with its tag is back
    CHECK_EQ(PoolBlocksOutstandingForTag(NTK_TAG('O', 'T', 's', 't')), 0ull);
    // and a fresh load works again
    Loaded again(L"OurTestDevice.sys");
    CHECK_ST(again.st, STATUS_SUCCESS);
}

TEST(unimplemented_import_is_a_loud_stub) {
    std::uint64_t before = StubCallCount("IoRegisterPlugPlayNotification");
    DriverInfo info;
    NTSTATUS st = LoadDriver(DriverPath(L"OurStubProbe.sys").c_str(), nullptr, &info);
    // DriverEntry only succeeds when the call returned STATUS_NOT_IMPLEMENTED
    CHECK_ST(st, STATUS_SUCCESS);
    CHECK(info.stubbed_imports >= 1);
    CHECK_EQ(StubCallCount("IoRegisterPlugPlayNotification"), before + 1);
    CHECK(!IsExportImplemented("IoRegisterPlugPlayNotification"));
    if (NT_SUCCESS(st)) UnloadDriver(info.id);
}

TEST(failed_driver_entry_is_rolled_back) {
    DriverInfo info;
    NTSTATUS st = LoadDriver(DriverPath(L"OurFailDriver.sys").c_str(), nullptr, &info);
    CHECK_ST(st, STATUS_UNSUCCESSFUL);
    std::wstring dev;
    CHECK(!ResolveName(L"\\Device\\OurFailDriver", &dev));
    CHECK(!ResolveName(L"\\??\\OurFailDriver", &dev));
    CHECK(!NT_SUCCESS(LoadDriver(DriverPath(L"DoesNotExist.sys").c_str())));
}

TEST(import_definitions_are_all_implemented) {
    // Every name the test drivers import, except the one stubbed on purpose.
    const char* names[] = {
        "DbgPrint", "ExAcquireFastMutex", "ExAcquireResourceExclusiveLite", "ExAcquireResourceSharedLite",
        "ExAllocatePoolWithTag", "ExDeleteResourceLite", "ExFreePool", "ExFreePoolWithTag", "KeResetEvent", "ExGetPreviousMode",
        "ExInitializeResourceLite", "ExReleaseFastMutex", "ExReleaseResourceLite", "IoCreateDevice",
        "IoCreateSymbolicLink", "IoDeleteDevice", "IoDeleteSymbolicLink", "IoGetCurrentProcess", "IofCompleteRequest",
        "KeCancelTimer", "KeDelayExecutionThread", "KeGetCurrentIrql", "KeInitializeDpc", "KeInitializeEvent",
        "KeInitializeMutex", "KeInitializeSemaphore", "KeInitializeTimerEx", "KeInsertQueueDpc", "KeQuerySystemTime",
        "KeReleaseMutex", "KeReleaseSemaphore", "KeReleaseSpinLock", "KeAcquireSpinLockRaiseToDpc", "KeSetEvent",
        "KeSetTimer", "KeWaitForSingleObject", "KfLowerIrql", "KfRaiseIrql", "MmIsAddressValid",
        "MmMapLockedPagesSpecifyCache", "ObReferenceObjectByHandle", "ObfDereferenceObject", "PsGetCurrentProcessId",
        "PsGetCurrentThreadId", "RtlCopyUnicodeString", "RtlInitUnicodeString", "memcpy", "memset",
        // the Zw* surface comes from the dispatcher / host ntdll
        "ZwClose", "ZwCreateFile", "ZwOpenKey", "ZwQueryValueKey", "ZwDeviceIoControlFile",
        // forwarded Rtl* and unwinding
        "RtlCompareUnicodeString", "RtlTimeToTimeFields", "__C_specific_handler", "RtlLookupFunctionEntry",
    };
    for (const char* n : names) CHECK_MSG(IsExportImplemented(n), "%s", n);
    CHECK(ExportCount() > 300);
}

TEST(virtual_irql_is_per_thread) {
    CHECK_EQ(GetVirtualIrql(), 0);
    SetVirtualIrql(2);
    CHECK_EQ(GetVirtualIrql(), 2);
    std::uint8_t other = 99;
    std::thread t([&] { other = GetVirtualIrql(); SetVirtualIrql(5); });
    t.join();
    CHECK_EQ(other, 0);                 // a new thread starts at PASSIVE_LEVEL
    CHECK_EQ(GetVirtualIrql(), 2);      // and did not disturb this one
    SetVirtualIrql(0);
}

TEST(kernel_waits_spinlocks_and_dpc_flush) {
    using namespace ntk;
    KEVENT ev;
    KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
    LARGE_INTEGER zero{};
    CHECK_ST(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    std::thread setter([&] { Sleep(50); KeSetEvent(&ev, 0, FALSE); });
    DWORD t0 = GetTickCount();
    LARGE_INTEGER limit;
    limit.QuadPart = -50000000;   // 5 s
    CHECK_ST(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &limit), STATUS_SUCCESS);
    CHECK_MSG(GetTickCount() - t0 < 4000, "wait took %lu", GetTickCount() - t0);
    setter.join();
    // timeout path
    limit.QuadPart = -1000000;    // 100 ms
    t0 = GetTickCount();
    CHECK_ST(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &limit), STATUS_TIMEOUT);
    CHECK_MSG(GetTickCount() - t0 >= 80, "timeout after %lu", GetTickCount() - t0);
    // spin lock raises the virtual IRQL and excludes
    KSPIN_LOCK lock = 0;
    KIRQL old = KeAcquireSpinLockRaiseToDpc(&lock);
    CHECK_EQ(old, 0);
    CHECK_EQ(GetVirtualIrql(), 2);
    CHECK_EQ(KeTryToAcquireSpinLockAtDpcLevel(&lock), FALSE);
    KeReleaseSpinLock(&lock, old);
    CHECK_EQ(GetVirtualIrql(), 0);
    CHECK_EQ(KeTryToAcquireSpinLockAtDpcLevel(&lock), TRUE);
    KeReleaseSpinLockFromDpcLevel(&lock);
    // wait-all over two objects
    KEVENT a, b;
    KeInitializeEvent(&a, NotificationEvent, TRUE);
    KeInitializeEvent(&b, NotificationEvent, FALSE);
    PVOID objs[2] = {&a, &b};
    CHECK_ST(KeWaitForMultipleObjects(2, objs, WaitAll, Executive, KernelMode, FALSE, &zero, nullptr), STATUS_TIMEOUT);
    KeSetEvent(&b, 0, FALSE);
    CHECK_ST(KeWaitForMultipleObjects(2, objs, WaitAll, Executive, KernelMode, FALSE, &zero, nullptr), STATUS_WAIT_0);
    CHECK_ST(KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &zero, nullptr), STATUS_WAIT_0);
    // DPC runs on the worker at DISPATCH_LEVEL; flush waits for it
    struct Ctx { std::atomic<int> irql{-1}; std::atomic<DWORD> tid{0}; } ctx;
    KDPC dpc;
    KeInitializeDpc(&dpc, [](PKDPC, PVOID c, PVOID, PVOID) {
        auto* x = static_cast<Ctx*>(c);
        x->irql = GetVirtualIrql();
        x->tid = GetCurrentThreadId();
    }, &ctx);
    CHECK(KeInsertQueueDpc(&dpc, nullptr, nullptr));
    KeFlushQueuedDpcs();
    CHECK_EQ(ctx.irql.load(), 2);
    CHECK(ctx.tid.load() != GetCurrentThreadId());
    // timer fires its DPC and signals
    KTIMER timer;
    KeInitializeTimerEx(&timer, NotificationTimer);
    ctx.irql = -1;
    LARGE_INTEGER due;
    due.QuadPart = -300000;   // 30 ms
    KeSetTimer(&timer, due, &dpc);
    limit.QuadPart = -30000000;
    CHECK_ST(KeWaitForSingleObject(&timer, Executive, KernelMode, FALSE, &limit), STATUS_SUCCESS);
    KeFlushQueuedDpcs();
    CHECK_EQ(ctx.irql.load(), 2);
    CHECK(!KeCancelTimer(&timer));
}

namespace {
struct QEntry { PVOID routine; ULONG flags; PWSTR name; PVOID context; ULONG def_type; PVOID def_data; ULONG def_len; };
struct Seen { int calls = 0; std::wstring names; ULONG blob_len = 0; ULONG blob_type = 0; std::wstring multi; };
NTSTATUS NTAPI QueryCb(PWSTR name, ULONG type, PVOID data, ULONG len, PVOID ctx, PVOID entry_ctx) {
    auto* seen = static_cast<Seen*>(ctx);
    ++seen->calls;
    seen->names += std::wstring(name ? name : L"?") + L";";
    if (entry_ctx == reinterpret_cast<PVOID>(1)) { seen->blob_len = len; seen->blob_type = type; }
    if (entry_ctx == reinterpret_cast<PVOID>(2)) seen->multi += std::wstring(static_cast<wchar_t*>(data), len / 2 - 1) + L"|";
    return STATUS_SUCCESS;
}
}  // namespace

TEST(registry_query_and_write_helpers) {
    using Create = NTSTATUS(NTAPI*)(ULONG, PWSTR);
    using Write = NTSTATUS(NTAPI*)(ULONG, PCWSTR, PCWSTR, ULONG, PVOID, ULONG);
    using Query = NTSTATUS(NTAPI*)(ULONG, PCWSTR, PVOID, PVOID, PVOID);
    using Del = NTSTATUS(NTAPI*)(ULONG, PCWSTR, PCWSTR);
    using Check = NTSTATUS(NTAPI*)(ULONG, PCWSTR);
    auto create = reinterpret_cast<Create>(dt::FindExport("RtlCreateRegistryKey"));
    auto write = reinterpret_cast<Write>(dt::FindExport("RtlWriteRegistryValue"));
    auto query = reinterpret_cast<Query>(dt::FindExport("RtlQueryRegistryValues"));
    auto del = reinterpret_cast<Del>(dt::FindExport("RtlDeleteRegistryValue"));
    auto check = reinterpret_cast<Check>(dt::FindExport("RtlCheckRegistryKey"));
    CHECK(create && write && query && del && check);
    if (!create) return;
    const wchar_t* key = L"\\Registry\\Machine\\Software\\Win10xboxDrvTest";
    NTSTATUS st = create(0, const_cast<PWSTR>(key));
    if (st == STATUS_ACCESS_DENIED) { std::printf("  (skipped: cannot create HKLM\\Software keys here)\n"); return; }
    CHECK_ST(st, STATUS_SUCCESS);
    ULONG dword = 7;
    unsigned char blob[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    wchar_t sz[] = L"driver";
    wchar_t multi[] = L"alpha\0beta\0";
    CHECK_ST(write(0, key, L"Count", 4, &dword, sizeof dword), STATUS_SUCCESS);
    CHECK_ST(write(0, key, L"Name", 1, sz, sizeof sz), STATUS_SUCCESS);
    CHECK_ST(write(0, key, L"Blob", 3, blob, sizeof blob), STATUS_SUCCESS);
    CHECK_ST(write(0, key, L"List", 7, multi, sizeof multi + 2), STATUS_SUCCESS);
    CHECK_ST(check(0, key), STATUS_SUCCESS);
    CHECK_ST(check(0, L"\\Registry\\Machine\\Software\\Win10xboxDrvTest\\Nope"), STATUS_OBJECT_NAME_NOT_FOUND);

    ULONG out_dword = 0, out_default = 0;
    UNICODE_STRING out_name = {};
    ULONG missing_default = 42;
    Seen seen;
    QEntry table[] = {
        {nullptr, 0x20, const_cast<PWSTR>(L"Count"), &out_dword, 0, nullptr, 0},
        {nullptr, 0x20, const_cast<PWSTR>(L"Name"), &out_name, 0, nullptr, 0},
        {reinterpret_cast<PVOID>(&QueryCb), 0, const_cast<PWSTR>(L"Blob"), reinterpret_cast<PVOID>(1), 0, nullptr, 0},
        {reinterpret_cast<PVOID>(&QueryCb), 0, const_cast<PWSTR>(L"List"), reinterpret_cast<PVOID>(2), 0, nullptr, 0},
        {nullptr, 0x20, const_cast<PWSTR>(L"Absent"), &out_default, 4, &missing_default, 4},
        {nullptr, 0, nullptr, nullptr, 0, nullptr, 0},
    };
    CHECK_ST(query(0, key, table, &seen, nullptr), STATUS_SUCCESS);
    CHECK_EQ(out_dword, 7ul);
    CHECK(out_name.Buffer && std::wstring(out_name.Buffer, out_name.Length / 2) == L"driver");
    CHECK_EQ(seen.blob_len, 9ul);
    CHECK_EQ(seen.blob_type, 3ul);
    CHECK(seen.multi == L"alpha|beta|");
    CHECK_EQ(out_default, 42ul);
    if (out_name.Buffer) reinterpret_cast<void(NTAPI*)(PVOID)>(dt::FindExport("ExFreePool"))(out_name.Buffer);

    QEntry required[] = {{nullptr, 0x20 | 0x4, const_cast<PWSTR>(L"Absent"), &out_default, 0, nullptr, 0}, {nullptr, 0, nullptr, nullptr, 0, nullptr, 0}};
    CHECK_ST(query(0, key, required, nullptr, nullptr), STATUS_OBJECT_NAME_NOT_FOUND);

    Seen all;
    QEntry enumerate[] = {{reinterpret_cast<PVOID>(&QueryCb), 0, nullptr, nullptr, 0, nullptr, 0}, {nullptr, 0, nullptr, nullptr, 0, nullptr, 0}};
    CHECK_ST(query(0, key, enumerate, &all, nullptr), STATUS_SUCCESS);
    CHECK_MSG(all.calls >= 5, "enumerated %d values: %ls", all.calls, all.names.c_str());   // Count Name Blob List(x2)

    CHECK_ST(del(0, key, L"Count"), STATUS_SUCCESS);
    out_dword = 0;
    QEntry gone[] = {{nullptr, 0x20 | 0x4, const_cast<PWSTR>(L"Count"), &out_dword, 0, nullptr, 0}, {nullptr, 0, nullptr, nullptr, 0, nullptr, 0}};
    CHECK_ST(query(0, key, gone, nullptr, nullptr), STATUS_OBJECT_NAME_NOT_FOUND);

    // clean up the key through the Nt interface
    using FnOpen = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    using FnDelKey = NTSTATUS(NTAPI*)(HANDLE);
    UNICODE_STRING kn = Us(key);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    HANDLE hk = nullptr;
    auto open = reinterpret_cast<FnOpen>(dt::ResolveNtFunction("NtOpenKey"));
    auto delkey = reinterpret_cast<FnDelKey>(dt::ResolveNtFunction("NtDeleteKey"));
    if (open && delkey && NT_SUCCESS(open(&hk, 0x10000 | 0x20019, &oa))) { delkey(hk); CloseHandle(hk); }
}

TEST(printf_family) {
    auto* snprintf_ = reinterpret_cast<SnprintfFn>(dt::FindExport("_snprintf"));
    auto* sprintf_ = reinterpret_cast<SprintfFn>(dt::FindExport("sprintf"));
    auto* snw = reinterpret_cast<SnwprintfFn>(dt::FindExport("_snwprintf"));
    CHECK(snprintf_ && sprintf_ && snw);
    if (!snprintf_) return;
    char b[128];
    sprintf_(b, "%d %u %x %X %08x %-5d| %5s %c %%", -42, 42u, 255, 255, 0xBEEF, 7, "ab", 'z');
    CHECK(std::string(b) == "-42 42 ff FF 0000beef 7    |    ab z %");
    UNICODE_STRING us = Us(L"Hi");
    ANSI_STRING as;
    as.Buffer = const_cast<char*>("Ansi");
    as.Length = 4;
    as.MaximumLength = 5;
    sprintf_(b, "[%wZ][%Z][%ws][%S][%ls]", &us, &as, L"wide", L"w2", L"w3");
    CHECK_MSG(std::string(b) == "[Hi][Ansi][wide][w2][w3]", "%s", b);
    sprintf_(b, "%I64d %lld %llx %I64u %lu %hd", -5000000000ll, 5000000000ll, 0xABCDEF012345ull, 18446744073709551615ull, 4000000000ul, static_cast<short>(-3));
    CHECK_MSG(std::string(b) == "-5000000000 5000000000 abcdef012345 18446744073709551615 4000000000 -3", "%s", b);
    sprintf_(b, "%p|%.3s|%*d|%-*d|%+d|% d|%#x|%05d", reinterpret_cast<void*>(0x1234), "abcdef", 6, 12, 4, 7, 5, 6, 255, -12);
    CHECK_MSG(std::string(b) == "0000000000001234|abc|    12|7   |+5| 6|0xff|-0012", "%s", b);
    CHECK_EQ(snprintf_(b, 8, "%s", "123456789"), -1);   // truncation
    CHECK_EQ(snprintf_(b, 8, "%s", "1234567"), 7);
    wchar_t w[32];
    CHECK_EQ(snw(w, 32, L"%s-%S-%d-%c", L"w", "n", 5, L'x'), 7);
    CHECK(std::wstring(w) == L"w-n-5-x");
}

TEST(pool_tags_and_double_free_detection) {
    auto alloc = reinterpret_cast<PVOID(NTAPI*)(POOL_TYPE, SIZE_T, ULONG)>(dt::FindExport("ExAllocatePoolWithTag"));
    auto free_tag = reinterpret_cast<void(NTAPI*)(PVOID, ULONG)>(dt::FindExport("ExFreePoolWithTag"));
    auto free_ = reinterpret_cast<void(NTAPI*)(PVOID)>(dt::FindExport("ExFreePool"));
    const ULONG tag = NTK_TAG('U', 'T', 's', 't');
    PoolStats s0 = GetPoolStats();
    void* p = alloc(NonPagedPool, 100, tag);
    CHECK(p != nullptr);
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u);
    for (int i = 0; i < 100; ++i) CHECK(static_cast<unsigned char*>(p)[i] == 0);   // zeroed
    CHECK_EQ(PoolBlocksOutstandingForTag(tag), 1ull);
    CHECK_EQ(GetPoolStats().bytes_outstanding, s0.bytes_outstanding + 100);
    free_tag(p, tag);
    CHECK_EQ(PoolBlocksOutstandingForTag(tag), 0ull);
    void* q = alloc(PagedPool, 10, tag);
    free_(q);
    CHECK_EQ(PoolBlocksOutstandingForTag(tag), 0ull);
    CHECK_EQ(GetPoolStats().blocks_outstanding, s0.blocks_outstanding);
    free_(nullptr);   // tolerated
}

TEST(dispatcher_hooks_chain_to_previous_implementation) {
    using namespace win10xbox;
    Loaded d(L"OurTestDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    NTSTATUS st = InstallDispatcherHooks();
    CHECK_ST(st, STATUS_SUCCESS);
    const dispatch::Entry* create = dispatch::LookupByName("NtCreateFile");
    const dispatch::Entry* ioctl = dispatch::LookupByName("NtDeviceIoControlFile");
    const dispatch::Entry* close = dispatch::LookupByName("NtClose");
    CHECK(create && ioctl && close);
    if (!create || !ioctl || !close) return;
    CHECK(create->policy != dispatch::Policy::PassThrough);
    // Call through the table exactly like win10xbox_dispatch does.
    UNICODE_STRING us = Us(L"\\??\\OurTestDevice");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    HANDLE h = nullptr;
    IO_STATUS_BLOCK iosb{};
    std::uint64_t a[11] = {reinterpret_cast<std::uint64_t>(&h), GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                           reinterpret_cast<std::uint64_t>(&oa), reinterpret_cast<std::uint64_t>(&iosb), 0, 0, 0, FILE_OPEN,
                           FILE_SYNCHRONOUS_IO_NONALERT, 0, 0};
    CHECK_ST(dispatch::CallWithArgs(create->impl, a, 11), STATUS_SUCCESS);
    CHECK(OwnsHandle(h));
    unsigned char in[4] = {1, 2, 3, 4}, out[4] = {};
    std::uint64_t b[10] = {reinterpret_cast<std::uint64_t>(h), 0, 0, 0, reinterpret_cast<std::uint64_t>(&iosb), IOCTL_OTD_XOR,
                           reinterpret_cast<std::uint64_t>(in), 4, reinterpret_cast<std::uint64_t>(out), 4};
    CHECK_ST(dispatch::CallWithArgs(ioctl->impl, b, 10), STATUS_SUCCESS);
    CHECK_EQ(out[0], 1 ^ OTD_XOR_KEY);
    std::uint64_t c[1] = {reinterpret_cast<std::uint64_t>(h)};
    CHECK_ST(dispatch::CallWithArgs(close->impl, c, 1), STATUS_SUCCESS);
    CHECK(!OwnsHandle(h));
    // A host event handle is not ours: NtClose falls through to the previous implementation.
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::uint64_t e[1] = {reinterpret_cast<std::uint64_t>(ev)};
    CHECK_ST(dispatch::CallWithArgs(close->impl, e, 1), STATUS_SUCCESS);
    SetLastError(0);
    CHECK(!CloseHandle(ev));   // already closed by the chain
    // A path that is not a driver device goes to the previous NtCreateFile (which fails for a missing file).
    UNICODE_STRING nofile = Us(L"\\??\\Z:\\definitely\\not\\here.bin");
    OBJECT_ATTRIBUTES oa2;
    InitializeObjectAttributes(&oa2, &nofile, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    a[2] = reinterpret_cast<std::uint64_t>(&oa2);
    h = nullptr;
    NTSTATUS miss = dispatch::CallWithArgs(create->impl, a, 11);
    CHECK(!NT_SUCCESS(miss));
    CHECK(h == nullptr);
    RemoveDispatcherHooks();
    CHECK(dispatch::LookupByName("NtCreateFile")->policy != dispatch::Policy::Todo);
}

#ifdef WIN10XBOX_HAVE_KMDF_DRIVER
TEST(kmdf_driver_binds_to_the_wdf_shim_and_serves_requests) {
    Loaded d(L"OurKmdfDevice.sys");
    CHECK_MSG(NT_SUCCESS(d.st), "LoadDriver status %08lx", static_cast<unsigned long>(d.st));
    if (!NT_SUCCESS(d.st)) return;
    CHECK_EQ(d.info.stubbed_imports, 0u);   // WdfVersionBind/Unbind come from the shim, the rest from ntoskrnl
    std::wstring dev;
    CHECK(ResolveName(L"\\??\\OurKmdfDevice", &dev));
    CHECK(dev == L"\\Device\\OurKmdfDevice");

    HANDLE h1, h2;
    CHECK_ST(Open(L"\\??\\OurKmdfDevice", &h1), STATUS_SUCCESS);   // EvtDeviceFileCreate allocates a file context
    CHECK_ST(Open(L"\\??\\OurKmdfDevice", &h2), STATUS_SUCCESS);
    unsigned char in[] = {1, 2, 3, 250}, out[4] = {};
    ULONG_PTR info = 0;
    CHECK_ST(Ioctl(h1, IOCTL_OKD_XOR, in, 4, out, 4, &info), STATUS_SUCCESS);
    CHECK_EQ(info, 4u);
    CHECK(out[0] == (1 ^ OKD_XOR_KEY) && out[3] == (250 ^ OKD_XOR_KEY));
    CHECK_ST(Ioctl(h1, IOCTL_OKD_XOR, in, 4, out, 2), STATUS_BUFFER_TOO_SMALL);   // WdfRequestRetrieveOutputBuffer minimum
    static unsigned char big[5000];
    unsigned char pat = 0x6B;
    CHECK_ST(Ioctl(h1, IOCTL_OKD_FILL, &pat, 1, big, sizeof big, &info), STATUS_SUCCESS);
    CHECK_EQ(info, sizeof big);
    bool all = true;
    for (unsigned char c : big) all = all && c == 0x6B;
    CHECK(all);
    OKD_STATS st1{}, st2{};
    CHECK_ST(Ioctl(h1, IOCTL_OKD_STATS, nullptr, 0, &st1, sizeof st1), STATUS_SUCCESS);
    CHECK_ST(Ioctl(h2, IOCTL_OKD_STATS, nullptr, 0, &st2, sizeof st2), STATUS_SUCCESS);
    CHECK_EQ(st1.Opens, 2ul);
    CHECK_EQ(st1.ParamIoctlCode, static_cast<unsigned long>(IOCTL_OKD_STATS));
    CHECK_EQ(st1.WdmIoctl, static_cast<unsigned long>(IOCTL_OKD_STATS));
    CHECK_EQ(st1.ParamInputLength, 0ul);
    CHECK_EQ(st1.ParamOutputLength, static_cast<unsigned long>(sizeof st1));
    CHECK_EQ(st1.RequestorMode, 0ul);
    CHECK_EQ(st1.FileCtx, 1ul);   // per-file-object contexts are distinct
    CHECK_EQ(st2.FileCtx, 2ul);
    CHECK(st2.Ioctls > st1.Ioctls);
    // write goes to EvtIoWrite, read has no handler and no default: the shim answers like the framework would
    IO_STATUS_BLOCK iosb{};
    CHECK_ST(Drv_NtWriteFile(h1, nullptr, nullptr, nullptr, &iosb, in, 4, nullptr, nullptr), STATUS_SUCCESS);
    CHECK_EQ(iosb.Information, 4u);
    CHECK_ST(Drv_NtReadFile(h1, nullptr, nullptr, nullptr, &iosb, out, 4, nullptr, nullptr), STATUS_INVALID_DEVICE_REQUEST);
    CHECK_ST(Ioctl(h1, CTL_CODE(0x22, 0xFFF, METHOD_BUFFERED, FILE_ANY_ACCESS), nullptr, 0, nullptr, 0), STATUS_INVALID_DEVICE_REQUEST);
    Drv_NtClose(h2);
    CHECK_ST(Ioctl(h1, IOCTL_OKD_STATS, nullptr, 0, &st2, sizeof st2), STATUS_SUCCESS);
    CHECK_EQ(st2.Closes, 1ul);    // EvtFileClose ran for the closed handle
    Drv_NtClose(h1);
    std::uint32_t id = d.info.id;
    (void)id;
}

TEST(kmdf_unload_runs_evt_unload_and_frees_framework_objects) {
    {
        Loaded d(L"OurKmdfDevice.sys");
        CHECK(NT_SUCCESS(d.st));
    }   // ~Loaded unloads: WdfDriver unload -> EvtDriverUnload, device + link deleted, globals unbound
    std::wstring dev;
    CHECK(!ResolveName(L"\\??\\OurKmdfDevice", &dev));
    CHECK_EQ(PoolBlocksOutstandingForTag(NTK_TAG('W', 'd', 'f', 'C')), 0ull);   // object contexts
    CHECK_EQ(PoolBlocksOutstandingForTag(NTK_TAG('W', 'd', 'f', 'G')), 0ull);   // WDF_DRIVER_GLOBALS
}

namespace {
ULONG WdfIndex(const char* name) {
    struct E { const char* n; ULONG i; };
    static const E e[] = {
#define WDF_FUNC(n, i) {#n, i},
#include "wdf_func_index.inc"
#undef WDF_FUNC
        {nullptr, 0}};
    for (const E* p = e; p->n; ++p)
        if (!std::strcmp(p->n, name)) return p->i;
    return 0xFFFFFFFF;
}
}  // namespace

TEST(wdf_function_table_stubs_are_named_and_loud) {
    // WdfTimerCreate is not implemented by the shim: calling its table slot must hit a stub that names it.
    Loaded d(L"OurKmdfDevice.sys");
    if (!NT_SUCCESS(d.st)) { CHECK(false); return; }
    struct BindInfo { ULONG size; PWCHAR comp; ULONG maj, min, build; ULONG count; const void** table; PVOID module; } bi{};
    using BindFn = NTSTATUS(NTAPI*)(PDRIVER_OBJECT, PUNICODE_STRING, BindInfo*, void**);
    using UnbindFn = NTSTATUS(NTAPI*)(PUNICODE_STRING, BindInfo*, void*);
    auto bind = reinterpret_cast<BindFn>(dt::FindExport("WdfVersionBind"));
    auto unbind = reinterpret_cast<UnbindFn>(dt::FindExport("WdfVersionUnbind"));
    CHECK(bind && unbind);
    if (!bind) return;
    const void* table = nullptr;
    wchar_t comp[] = L"KmdfLibrary";
    bi.size = sizeof bi;
    bi.comp = comp;
    bi.maj = 1; bi.min = 33;
    bi.count = 458;
    bi.table = &table;
    UNICODE_STRING reg = Us(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Probe");
    void* globals = nullptr;
    CHECK_ST(bind(static_cast<PDRIVER_OBJECT>(d.info.driver_object), &reg, &bi, &globals), STATUS_SUCCESS);
    CHECK(table != nullptr);
    ULONG idx = WdfIndex("WdfTimerCreate");
    CHECK(idx < 458);
    if (table && idx < 458) {
        const std::uint64_t before = StubCallCount("WdfTimerCreate");
        auto fn = reinterpret_cast<NTSTATUS(NTAPI*)(void*, void*, void*, void*)>(const_cast<void*>(static_cast<const void* const*>(table)[idx]));
        CHECK_ST(fn(globals, nullptr, nullptr, nullptr), STATUS_NOT_IMPLEMENTED);
        CHECK_EQ(StubCallCount("WdfTimerCreate"), before + 1);
        // an implemented slot is not a stub
        ULONG ci = WdfIndex("WdfDriverCreate");
        CHECK(ci < 458 && static_cast<const void* const*>(table)[ci] != static_cast<const void* const*>(table)[idx]);
    }
    unbind(&reg, &bi, globals);
}
#endif

int main(int argc, char** argv) {
    if (argc < 2) { std::fputs("usage: test_drivers <driver directory>\n", stderr); return 64; }
    g_dir = ToHostPath(argv[1]);
    win10xbox::config::Config cfg;
    cfg.trace_default_on = false;
    win10xbox::Initialize(cfg);
    Options opts;
    opts.install_dispatcher_hooks = false;
    NTSTATUS st = Initialize(&opts);
    if (!NT_SUCCESS(st)) { std::printf("drivers::Initialize failed: %08lx\n", static_cast<unsigned long>(st)); return 2; }
    const char* filter = argc > 2 ? argv[2] : nullptr;
    int run = 0;
    for (auto& c : w10x_test::Cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = w10x_test::Failures();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        std::printf("[ %s ] %s\n", w10x_test::Failures() == before ? " OK " : "FAIL", c.name);
        ++run;
    }
    PoolStats ps = GetPoolStats();
    std::printf("%d tests, %d failures (pool: %llu blocks live)\n", run, w10x_test::Failures(),
                static_cast<unsigned long long>(ps.blocks_outstanding));
    Shutdown();
    win10xbox::Shutdown();
    return w10x_test::Failures() ? 1 : 0;
}
