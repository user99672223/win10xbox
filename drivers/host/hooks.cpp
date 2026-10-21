// Dispatcher interposition: chains the Nt* entries that reach devices so the
// driver host sees opens/ioctls on driver-created devices without edits to
// core/dispatch. The orchestrator may instead call OwnsPath/OwnsHandle and the
// Drv_* functions from the dispatcher's own translators and turn this off
// (drivers::Options::install_dispatcher_hooks = false).
#include "../ntoskrnl/internal.h"
#include "win10xbox/dispatch.h"

namespace win10xbox::drivers {
using namespace detail;
namespace dsp = win10xbox::dispatch;

namespace {

enum Slot {
    kCreateFile, kOpenFile, kClose, kDeviceIoControl, kFsControl, kReadFile, kWriteFile, kFlushBuffers,
    kQueryVolumeInfo, kOpenSymLink, kQuerySymLink, kSlotCount
};

struct Saved {
    dsp::Entry* entry = nullptr;
    void* prev = nullptr;          // what to call for paths/handles that are not ours
    void* old_impl = nullptr;
    dsp::Policy old_policy = dsp::Policy::Todo;
};
Saved g_saved[kSlotCount];
bool g_installed = false;

template <class Fn>
Fn Prev(Slot s) { return reinterpret_cast<Fn>(g_saved[s].prev); }

constexpr NTSTATUS kNotImpl = static_cast<NTSTATUS>(0xC0000002);

NTSTATUS NTAPI W_NtCreateFile(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io, PLARGE_INTEGER alloc,
                              ULONG attrs, ULONG share, ULONG disp, ULONG opts, PVOID ea, ULONG ea_len) {
    if (OwnsPath(oa)) return Drv_NtCreateFile(h, a, oa, io, alloc, attrs, share, disp, opts, ea, ea_len);
    using Fn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                                ULONG, ULONG, PVOID, ULONG);
    auto fn = Prev<Fn>(kCreateFile);
    return fn ? fn(h, a, oa, io, alloc, attrs, share, disp, opts, ea, ea_len) : kNotImpl;
}
NTSTATUS NTAPI W_NtOpenFile(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io, ULONG share, ULONG opts) {
    if (OwnsPath(oa)) return Drv_NtOpenFile(h, a, oa, io, share, opts);
    using Fn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
    auto fn = Prev<Fn>(kOpenFile);
    return fn ? fn(h, a, oa, io, share, opts) : kNotImpl;
}
NTSTATUS NTAPI W_NtClose(HANDLE h) {
    if (OwnsHandle(h)) return Drv_NtClose(h);
    using Fn = NTSTATUS(NTAPI*)(HANDLE);
    auto fn = Prev<Fn>(kClose);
    return fn ? fn(h) : kNotImpl;
}
NTSTATUS NTAPI W_NtDeviceIoControlFile(HANDLE f, HANDLE ev, PIO_APC_ROUTINE apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG code,
                                       PVOID in, ULONG in_len, PVOID out, ULONG out_len) {
    if (OwnsHandle(f)) return Drv_NtDeviceIoControlFile(f, ev, apc, ctx, io, code, in, in_len, out, out_len);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
    auto fn = Prev<Fn>(kDeviceIoControl);
    return fn ? fn(f, ev, apc, ctx, io, code, in, in_len, out, out_len) : kNotImpl;
}
NTSTATUS NTAPI W_NtFsControlFile(HANDLE f, HANDLE ev, PIO_APC_ROUTINE apc, PVOID ctx, PIO_STATUS_BLOCK io, ULONG code,
                                 PVOID in, ULONG in_len, PVOID out, ULONG out_len) {
    if (OwnsHandle(f)) return Drv_NtFsControlFile(f, ev, apc, ctx, io, code, in, in_len, out, out_len);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
    auto fn = Prev<Fn>(kFsControl);
    return fn ? fn(f, ev, apc, ctx, io, code, in, in_len, out, out_len) : kNotImpl;
}
NTSTATUS NTAPI W_NtReadFile(HANDLE f, HANDLE ev, PIO_APC_ROUTINE apc, PVOID ctx, PIO_STATUS_BLOCK io, PVOID buf, ULONG len,
                            PLARGE_INTEGER off, PULONG key) {
    if (OwnsHandle(f)) return Drv_NtReadFile(f, ev, apc, ctx, io, buf, len, off, key);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    auto fn = Prev<Fn>(kReadFile);
    return fn ? fn(f, ev, apc, ctx, io, buf, len, off, key) : kNotImpl;
}
NTSTATUS NTAPI W_NtWriteFile(HANDLE f, HANDLE ev, PIO_APC_ROUTINE apc, PVOID ctx, PIO_STATUS_BLOCK io, PVOID buf, ULONG len,
                             PLARGE_INTEGER off, PULONG key) {
    if (OwnsHandle(f)) return Drv_NtWriteFile(f, ev, apc, ctx, io, buf, len, off, key);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    auto fn = Prev<Fn>(kWriteFile);
    return fn ? fn(f, ev, apc, ctx, io, buf, len, off, key) : kNotImpl;
}
NTSTATUS NTAPI W_NtFlushBuffersFile(HANDLE f, PIO_STATUS_BLOCK io) {
    if (OwnsHandle(f)) return Drv_NtFlushBuffersFile(f, io);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK);
    auto fn = Prev<Fn>(kFlushBuffers);
    return fn ? fn(f, io) : kNotImpl;
}
NTSTATUS NTAPI W_NtQueryVolumeInformationFile(HANDLE f, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, ULONG cls) {
    if (OwnsHandle(f)) return Drv_NtQueryVolumeInformationFile(f, io, buf, len, cls);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    auto fn = Prev<Fn>(kQueryVolumeInfo);
    return fn ? fn(f, io, buf, len, cls) : kNotImpl;
}
NTSTATUS NTAPI W_NtOpenSymbolicLinkObject(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa) {
    if (OwnsSymbolicLinkPath(oa)) return Drv_NtOpenSymbolicLinkObject(h, a, oa);
    using Fn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    auto fn = Prev<Fn>(kOpenSymLink);
    return fn ? fn(h, a, oa) : kNotImpl;
}
NTSTATUS NTAPI W_NtQuerySymbolicLinkObject(HANDLE h, PUNICODE_STRING target, PULONG len) {
    if (OwnsHandle(h)) return Drv_NtQuerySymbolicLinkObject(h, target, len);
    using Fn = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, PULONG);
    auto fn = Prev<Fn>(kQuerySymLink);
    return fn ? fn(h, target, len) : kNotImpl;
}

struct Wrapper { Slot slot; const char* name; void* fn; };
const Wrapper kWrappers[] = {
    {kCreateFile, "NtCreateFile", reinterpret_cast<void*>(&W_NtCreateFile)},
    {kOpenFile, "NtOpenFile", reinterpret_cast<void*>(&W_NtOpenFile)},
    {kClose, "NtClose", reinterpret_cast<void*>(&W_NtClose)},
    {kDeviceIoControl, "NtDeviceIoControlFile", reinterpret_cast<void*>(&W_NtDeviceIoControlFile)},
    {kFsControl, "NtFsControlFile", reinterpret_cast<void*>(&W_NtFsControlFile)},
    {kReadFile, "NtReadFile", reinterpret_cast<void*>(&W_NtReadFile)},
    {kWriteFile, "NtWriteFile", reinterpret_cast<void*>(&W_NtWriteFile)},
    {kFlushBuffers, "NtFlushBuffersFile", reinterpret_cast<void*>(&W_NtFlushBuffersFile)},
    {kQueryVolumeInfo, "NtQueryVolumeInformationFile", reinterpret_cast<void*>(&W_NtQueryVolumeInformationFile)},
    {kOpenSymLink, "NtOpenSymbolicLinkObject", reinterpret_cast<void*>(&W_NtOpenSymbolicLinkObject)},
    {kQuerySymLink, "NtQuerySymbolicLinkObject", reinterpret_cast<void*>(&W_NtQuerySymbolicLinkObject)},
};

}  // namespace

NTSTATUS InstallDispatcherHooks() {
    if (g_installed) return STATUS_SUCCESS;
    NTSTATUS st = dsp::Initialize();   // idempotent; process::Launch repeats it harmlessly
    if (!NT_SUCCESS(st)) return st;
    std::size_t hooked = 0;
    for (const Wrapper& w : kWrappers) {
        auto* e = const_cast<dsp::Entry*>(dsp::LookupByName(w.name));
        if (!e) { WARN(driver, "dispatcher has no entry %s, not hooked", w.name); continue; }
        Saved& sv = g_saved[w.slot];
        sv.entry = e;
        sv.old_impl = e->impl;
        sv.old_policy = e->policy;
        switch (e->policy) {
        case dsp::Policy::PassThrough: sv.prev = e->host_fn; break;
        case dsp::Policy::Translate:
        case dsp::Policy::Emulate: sv.prev = e->impl ? e->impl : e->host_fn; break;
        case dsp::Policy::Todo: sv.prev = nullptr; break;
        }
        e->impl = w.fn;
        e->policy = dsp::Policy::Translate;
        ++hooked;
    }
    g_installed = hooked != 0;
    TRACE(driver, "dispatcher hooks installed on %zu syscalls", hooked);
    return hooked ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

void RemoveDispatcherHooks() {
    if (!g_installed) return;
    for (Saved& sv : g_saved) {
        if (!sv.entry) continue;
        sv.entry->impl = sv.old_impl;
        sv.entry->policy = sv.old_policy;
        sv = Saved{};
    }
    g_installed = false;
}

}  // namespace win10xbox::drivers
