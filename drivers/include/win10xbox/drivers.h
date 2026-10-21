// win10xbox driver host: loads kernel-mode driver images (.sys) in user mode
// and gives them an object manager, an IRP engine and the ntoskrnl API they
// import. Owned by drivers/. MIT.
//
// Model (see drivers/README.md):
//   * A driver image is mapped by core/loader (ReserveOnly), its imports
//     (ntoskrnl.exe, hal.dll, wdfldr.sys ...) are bound to the user-mode
//     ntoskrnl implementation in drivers/ntoskrnl, then DriverEntry runs on
//     the calling thread at a virtual PASSIVE_LEVEL.
//   * The driver builds DEVICE_OBJECTs (IoCreateDevice) and symbolic links
//     (IoCreateSymbolicLink) in a private namespace: \Device\X, \??\X, \DosDevices\X.
//   * Guest NtCreateFile / NtDeviceIoControlFile / NtReadFile / NtWriteFile /
//     NtClose on such a path or the handle it produced become IRP_MJ_CREATE /
//     DEVICE_CONTROL / READ / WRITE / CLEANUP+CLOSE sent through IoCallDriver
//     to the driver's MajorFunction table; the result is copied back.
//   * DPCs, timers, work items and interrupt service routines run on one
//     dedicated worker thread at DISPATCH_LEVEL / DIRQL. IRQL is a virtual
//     per-thread value, not hardware state.
//
// Privileged instructions executed by driver code (rdmsr, in/out, cli ...)
// are not handled here: that is the privileged-instruction handler (a
// separate module). It may use GetVirtualIrql/SetVirtualIrql to back
// `mov rax, cr8` / `mov cr8, rax`.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "win10xbox/loader.h"

namespace win10xbox::drivers {

struct Options {
    // Chain the Nt* entries of the dispatcher table (core/dispatch) that
    // reach devices: NtCreateFile, NtOpenFile, NtClose, NtDeviceIoControlFile,
    // NtFsControlFile, NtReadFile, NtWriteFile, NtFlushBuffersFile,
    // NtQueryVolumeInformationFile, NtOpenSymbolicLinkObject,
    // NtQuerySymbolicLinkObject. Needs dispatch::Initialize() to have run.
    // When the dispatcher is wired to call the Drv_* functions directly
    // (OwnsPath / OwnsHandle, below) turn this off.
    bool install_dispatcher_hooks = true;
    // IRQL rule violations (lowering below the current level, waiting at
    // DISPATCH_LEVEL ...): false = ERR trace and continue, true = bug check.
    bool strict_irql = false;
};

// Idempotent. Creates the pool heap, the namespace and the worker thread.
NTSTATUS Initialize(const Options* opts = nullptr);
// Unloads every driver (DriverUnload, in reverse load order), stops the
// worker, removes dispatcher hooks. Idempotent.
void Shutdown();
bool IsInitialized();

// ---- Drivers ----------------------------------------------------------------
struct LoadOptions {
    std::wstring driver_name;     // "\Driver\Name"; default "\Driver\<file stem>"
    std::wstring registry_path;   // default "\Registry\Machine\System\CurrentControlSet\Services\<file stem>"
};
struct DriverInfo {
    std::uint32_t id = 0;
    std::wstring  name;           // "\Driver\OurTestDevice"
    void*         driver_object = nullptr;   // DRIVER_OBJECT*
    void*         image_base = nullptr;
    std::size_t   image_size = 0;
    std::size_t   stubbed_imports = 0;       // imports bound to a loud STATUS_NOT_IMPLEMENTED stub
};
// Maps `host_path` (Win32 host path of a .sys), binds imports, runs
// DriverEntry(DRIVER_OBJECT*, RegistryPath). On failure the image is
// unmapped and DriverEntry's status (or the loader's) is returned.
NTSTATUS LoadDriver(const wchar_t* host_path, const LoadOptions* opts = nullptr, DriverInfo* info = nullptr);
// Calls DriverUnload (if any), deletes the driver's remaining devices and
// symbolic links, unmaps the image.
NTSTATUS UnloadDriver(std::uint32_t id);

// Called after imports are bound and before the image gets its final page
// protections (so the pages are still writable) and before DriverEntry.
// The privileged-instruction handler uses this to patch/arm the image.
// Return a failure status to abort the load.
using ImageLoadHook = NTSTATUS (*)(const loader::MappedImage& image, void* ctx);
void SetImageLoadHook(ImageLoadHook hook, void* ctx);

// ---- Namespace introspection (tests, tools, trace) -------------------------------------
struct DeviceInfo {
    std::wstring  name;           // "\Device\X" (empty for unnamed devices)
    std::wstring  driver;         // "\Driver\Y"
    std::uint32_t device_type = 0, characteristics = 0, flags = 0;
    void*         device_object = nullptr;
};
std::vector<DeviceInfo> EnumerateDevices();
std::vector<std::pair<std::wstring, std::wstring>> EnumerateSymbolicLinks();   // {link, target}
// "\??\X" / "\DosDevices\X" / "\Device\X" -> final "\Device\X" (follows links).
bool ResolveName(const wchar_t* name, std::wstring* device_name);

// ---- Virtual IRQL (per thread) -------------------------------------------------------------
std::uint8_t GetVirtualIrql();
void         SetVirtualIrql(std::uint8_t irql);

// ---- Interrupts ------------------------------------------------------------------------------
// Runs the service routine connected with IoConnectInterrupt(Ex) for `vector`
// on the worker thread at the connected DIRQL. Returns false if none is connected.
bool InjectInterrupt(std::uint32_t vector);

// Bug check handler (KeBugCheck/KeBugCheckEx). Default: ERR trace then
// terminate the process. Tests install a handler that records and returns
// (the calling driver code then continues, which real Windows never does).
using BugCheckHandler = void (*)(std::uint32_t code, const std::uint64_t params[4], void* ctx);
void SetBugCheckHandler(BugCheckHandler h, void* ctx);

// ---- Diagnostics -----------------------------------------------------------------------------------
struct PoolStats { std::uint64_t allocations, frees, blocks_outstanding, bytes_outstanding; };
PoolStats GetPoolStats();
std::uint64_t PoolBlocksOutstandingForTag(std::uint32_t tag);
// {tag, blocks} for every tag with live blocks (tag in memory order: 'Tag1' prints as such).
std::vector<std::pair<std::uint32_t, std::int64_t>> PoolLiveTags();
// Times a loud stub (an ntoskrnl import with no implementation) was called.
std::uint64_t StubCallCount(const char* api_name);
// True if `name` is an implemented ntoskrnl/hal export (not a stub).
bool IsExportImplemented(const char* name);
std::size_t ExportCount();

// ---- Dispatcher hooks ----------------------------------------------------------------------------------
// The dispatcher (core/dispatch) can either call these from its own Nt*
// translators or rely on InstallDispatcherHooks(). All have exactly the
// signature of the Nt* syscall of the same name and are safe to call from
// guest threads in host mode (the dispatcher already switches).
//
//   if (drivers::OwnsPath(attr))   return drivers::Drv_NtCreateFile(...);
//   if (drivers::OwnsHandle(h))    return drivers::Drv_NtDeviceIoControlFile(...);
bool OwnsPath(const OBJECT_ATTRIBUTES* attr);   // resolves to a driver-created device
bool OwnsHandle(HANDLE h);                      // handle produced by Drv_NtCreateFile/Drv_NtOpenFile

NTSTATUS NTAPI Drv_NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                                PLARGE_INTEGER alloc_size, ULONG attributes, ULONG sharing, ULONG disposition,
                                ULONG options, PVOID ea, ULONG ea_length);
NTSTATUS NTAPI Drv_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                              ULONG sharing, ULONG options);
NTSTATUS NTAPI Drv_NtClose(HANDLE handle);
NTSTATUS NTAPI Drv_NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                         PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len);
NTSTATUS NTAPI Drv_NtFsControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                   PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                   ULONG out_len);
NTSTATUS NTAPI Drv_NtReadFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                              PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);
NTSTATUS NTAPI Drv_NtWriteFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                               PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);
NTSTATUS NTAPI Drv_NtFlushBuffersFile(HANDLE file, PIO_STATUS_BLOCK io);
NTSTATUS NTAPI Drv_NtQueryVolumeInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer, ULONG length,
                                                ULONG fs_information_class);
// \??\X symbolic links created by drivers (kernel32 QueryDosDevice, GetFinalPathNameByHandle ...).
NTSTATUS NTAPI Drv_NtOpenSymbolicLinkObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr);
NTSTATUS NTAPI Drv_NtQuerySymbolicLinkObject(HANDLE handle, PUNICODE_STRING target, PULONG returned_length);

// Interposes the functions above in the dispatcher table: each Entry's impl
// becomes a wrapper that calls the Drv_* function for owned paths/handles and
// otherwise the previous implementation (impl, or the host export). Needs
// dispatch::Initialize() first. Idempotent; RemoveDispatcherHooks restores the table.
NTSTATUS InstallDispatcherHooks();
void RemoveDispatcherHooks();

}  // namespace win10xbox::drivers
