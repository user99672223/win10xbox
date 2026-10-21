// Shared internals of the driver host (drivers/ntoskrnl + drivers/host).
// Not a public interface: see drivers/include/win10xbox/drivers.h.
#pragma once
#include <windows.h>
#include <winternl.h>
#define NTK_NO_API_DECLS
#include "ntk/ntk.h"
#include "win10xbox/drivers.h"
#include "win10xbox/trace.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Pool tags are written by humans as 'Tag1' (little-endian in memory).
#ifndef NTK_TAG
#define NTK_TAG(a, b, c, d) (((ULONG)(unsigned char)(a)) | ((ULONG)(unsigned char)(b) << 8) | ((ULONG)(unsigned char)(c) << 16) | ((ULONG)(unsigned char)(d) << 24))
#endif

namespace win10xbox::drivers::detail {

// ---- export table ----------------------------------------------------------------
struct Export { const char* name; void* addr; };
struct ExportGroup { const Export* data; std::size_t size; };

// One group per implementation file (defined next to the functions).
ExportGroup KeExports();
ExportGroup ExExports();
ExportGroup IoExports();
ExportGroup ObPsMmExports();
ExportGroup RtlExports();
ExportGroup DataExports();
ExportGroup RegistryExports();
void ObPsMmInit();
void ObPsMmShutdown();
void NotifyImageLoaded(const std::wstring& full_name, PVOID base, SIZE_T size);
PFILE_OBJECT HostFileObjectForHandle(HANDLE h);
DRIVER_OBJECT* DriverByName(std::wstring_view name);
std::vector<PDEVICE_OBJECT> ShutdownDevices();
ExportGroup WdfLdrExports();   // drivers/kmdf: WdfVersionBind & friends

// Implemented export (not a stub), or nullptr. Case-sensitive, exact.
void* FindExport(const char* name);
// Address a driver's import `name` is bound to: the export, a Zw*/Nt* entry
// resolved through the dispatcher, a forward to the host ntdll, or a freshly
// generated loud stub (counted in *stubbed). Never nullptr.
class StubArena;
void* BindImport(const char* module, const char* name, StubArena* arena, bool* is_stub);

// Executable thunks for unimplemented imports: each calls StubHandler(name),
// which traces FIXME and returns STATUS_NOT_IMPLEMENTED.
class StubArena {
public:
    ~StubArena();
    // Reserves a thunk for `name`; thunks are only executable after Seal().
    void* Add(const char* name);
    bool Seal();
    std::size_t count() const { return count_; }
private:
    struct Page { std::uint8_t* mem; std::size_t used; };
    std::vector<Page> pages_;
    std::size_t count_ = 0;
};
std::int64_t StubHandler(const char* name);

// ---- virtual IRQL ---------------------------------------------------------------------
KIRQL CurrentIrql();
KIRQL SwapIrql(KIRQL n);                 // returns the previous level
KPROCESSOR_MODE PreviousMode();          // ExGetPreviousMode
KPROCESSOR_MODE SwapPreviousMode(KPROCESSOR_MODE m);
bool InDriverCall();
bool SwapInDriverCall(bool v);
bool StrictIrql();
void SetStrictIrql(bool strict);
bool IrqlInit();
void IrqlShutdown();
KIRQL SpinAcquireRaise(PKSPIN_LOCK lock, KIRQL raise_to);   // raises the virtual IRQL, then spins
void SpinReleaseLower(PKSPIN_LOCK lock, KIRQL old);
void IrqlViolation(const char* what, KIRQL current, KIRQL wanted);

// ---- pseudo kernel threads/processes -------------------------------------------------------
struct PseudoThread {
    DISPATCHER_HEADER hdr;     // ThreadObject; signalled when a system thread ends
    DWORD tid;
    HANDLE host_handle;        // for system threads we created
    PVOID start_context;
    PKSTART_ROUTINE start_routine;
};
PseudoThread* CurrentPseudoThread();
PseudoThread* PseudoThreadForTid(DWORD tid, bool create);
PVOID CurrentPseudoProcess();   // PEPROCESS of the guest process
PVOID SystemPseudoProcess();

// ---- dispatcher-object waits (ke.cpp) ------------------------------------------------------------
extern SRWLOCK g_disp_lock;
extern CONDITION_VARIABLE g_disp_cv;
void DispSignalAll();                                  // wake all waiters; lock must NOT be held
LONG DispSetEventLocked(PKEVENT ev);                   // lock held; returns previous state
NTSTATUS WaitForObjects(ULONG count, PVOID* objects, WAIT_TYPE wait_type, PLARGE_INTEGER timeout);
std::int64_t GuestSystemTime100ns();
std::uint64_t GuestInterruptTime100ns();

// ---- worker thread (worker.cpp) ---------------------------------------------------------------------
bool WorkerStart();
void WorkerStop();
bool WorkerQueueDpc(PKDPC dpc, PVOID a1, PVOID a2);
bool WorkerRemoveDpc(PKDPC dpc);
void WorkerFlushDpcs();
void WorkerPost(std::function<void()> fn);          // runs at PASSIVE_LEVEL on the worker
void WorkerPostDispatch(std::function<void()> fn);  // runs at DISPATCH_LEVEL
bool WorkerOnWorkerThread();
// Timers (KeSetTimer/KeCancelTimer): due in 100ns units relative to now.
bool WorkerSetTimer(PKTIMER t, std::int64_t due_100ns_relative, ULONG period_ms, PKDPC dpc);
bool WorkerCancelTimer(PKTIMER t);
// Interrupts
struct InterruptRec;
InterruptRec* InterruptConnect(PKSERVICE_ROUTINE routine, PVOID ctx, PKSPIN_LOCK lock, ULONG vector, KIRQL irql,
                               KIRQL sync_irql);
void InterruptDisconnect(InterruptRec* r);
bool InterruptInject(ULONG vector);
BOOLEAN InterruptSynchronize(InterruptRec* r, PKSYNCHRONIZE_ROUTINE routine, PVOID ctx);

// ---- pool (pool_ex.cpp) -------------------------------------------------------------------------------------
void* PoolAlloc(SIZE_T bytes, ULONG tag);
void  PoolFree(void* p, ULONG tag, bool check_tag);
bool  PoolInit();
void  PoolShutdown();

// ---- object registry (objects.cpp) -----------------------------------------------------------------------------
enum class ObjType : std::uint8_t { Device, Driver, File, Event, Thread, Process, Interrupt, Other };
void ObjRegister(void* p, ObjType t, void (*destroy)(void*), LONG refs = 1);
bool ObjLookup(void* p, ObjType* t);
LONG ObjRef(void* p);              // new count, -1 if unknown
LONG ObjDeref(void* p);            // new count, -1 if unknown; runs destroy at 0
void ObjForget(void* p);
void ObjClearAll();

// ---- namespace (namespace.cpp) -----------------------------------------------------------------------------------
std::wstring NsCanonical(std::wstring_view name);                 // lower-case; DosDevices and GLOBAL?? prefixes become ??
DEVICE_OBJECT* NsFindDevice(std::wstring_view name);              // exact device name (canonical compare)
// Resolves device names and symbolic links (up to 8 hops). `remainder` gets
// the unmatched tail ("\foo" in "\Device\X\foo"). Returns the device or nullptr.
DEVICE_OBJECT* NsResolve(std::wstring_view name, std::wstring* remainder, std::wstring* resolved_name = nullptr);
NTSTATUS NsAddDevice(DEVICE_OBJECT* dev, std::wstring_view name);
void NsRemoveDevice(DEVICE_OBJECT* dev);
std::wstring NsDeviceName(DEVICE_OBJECT* dev);                    // "" if unnamed
NTSTATUS NsAddLink(std::wstring_view link, std::wstring_view target);
NTSTATUS NsRemoveLink(std::wstring_view link);
bool NsQueryLink(std::wstring_view link, std::wstring* target);
void NsRemoveLinksTargeting(std::wstring_view device_name);
std::wstring ToWide(const UNICODE_STRING& s);
std::wstring_view View(const UNICODE_STRING& s);
void NsClear();
std::vector<std::pair<std::wstring, DEVICE_OBJECT*>> NsSnapshotDevices();
std::vector<std::pair<std::wstring, std::wstring>> NsSnapshotLinks();

// ---- drivers (driver_loader.cpp) ---------------------------------------------------------------------------------------
struct DriverRec;
DriverRec* DriverFromObject(DRIVER_OBJECT* d);
void DriversUnloadAll();
std::wstring DriverObjectName(DRIVER_OBJECT* d);
NTSTATUS CreateDriverObject(const std::wstring& name, const std::wstring& registry_path, DRIVER_OBJECT** out);
void DestroyDriverObject(DRIVER_OBJECT* d);
extern LIST_ENTRY g_ps_loaded_module_list;

// ---- IRPs (io.cpp) -----------------------------------------------------------------------------------------------------------
enum class IrpKind : std::uint32_t { DriverOwned = 1, IoMgrOwned = 2, TopLevel = 3 };
struct TopContext;   // top-level (user-originated) request bookkeeping
PIRP IrpAllocate(CCHAR stack_count, IrpKind kind);
void IrpInit(PIRP irp, USHORT packet_size, CCHAR stack_count);   // IoInitializeIrp
void IrpSetUserOutLen(PIRP irp, ULONG n);
void IrpRelease(PIRP irp);                    // frees IRP (+ system buffer / MDL per flags)
IrpKind IrpKindOf(PIRP irp);
void IrpSetTop(PIRP irp, TopContext* t);
TopContext* IrpTop(PIRP irp);
// Called by IofCompleteRequest once every stack location completed. Defined
// in the host (irp_top.cpp) for TopLevel IRPs.
void TopFinalComplete(PIRP irp, TopContext* top);
NTSTATUS NTAPI CallDriver(PDEVICE_OBJECT dev, PIRP irp);          // IofCallDriver
VOID NTAPI CompleteRequest(PIRP irp, CCHAR boost);                // IofCompleteRequest
PDEVICE_OBJECT NTAPI AttachedDevice(PDEVICE_OBJECT dev);          // IoGetAttachedDevice
NTSTATUS NTAPI InvalidDeviceRequest(PDEVICE_OBJECT dev, PIRP irp);
void DeviceFree(PDEVICE_OBJECT dev);
PMDL MdlCreate(PVOID va, ULONG length);
void MdlFree(PMDL mdl);

bool NsHasObjects();
void SendShutdownIrps();
void CloseAllHandles();
void CloseHandlesOfDriver(DRIVER_OBJECT* drv);
NTSTATUS OpenInternalPublic(ACCESS_MASK access, const std::wstring& name, ULONG share, ULONG disposition, ULONG options,
                            ULONG file_attributes, KPROCESSOR_MODE mode, PFILE_OBJECT* out_fo);

// ---- misc helpers -----------------------------------------------------------------------------------------------------------------
// Looks up an Nt* syscall implementation: the dispatcher's impl/host_fn if the
// dispatcher is initialised, else the host ntdll export.
void* ResolveNtFunction(const char* nt_name);
bool ProbeRange(const void* p, std::size_t len, bool write);

// Pool tags used by the host itself.
constexpr ULONG kTagDev = NTK_TAG('N', 'k', 'D', 'v'), kTagDrv = NTK_TAG('N', 'k', 'D', 'r'),
                kTagIrp = NTK_TAG('N', 'I', 'r', 'p'), kTagFile = NTK_TAG('N', 'k', 'F', 'l'),
                kTagMdl = NTK_TAG('N', 'M', 'd', 'l'), kTagStr = NTK_TAG('N', 'k', 'S', 'r'),
                kTagMisc = NTK_TAG('N', 'M', 's', 'c'), kTagBuf = NTK_TAG('N', 'k', 'B', 'f');

}  // namespace win10xbox::drivers::detail



// Cross-file calls between the ntk:: implementations.
namespace ntk {
KIRQL NTAPI KfRaiseIrql(KIRQL);
VOID NTAPI KfLowerIrql(KIRQL);
VOID NTAPI KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
LONG NTAPI KeSetEvent(PKEVENT, LONG, BOOLEAN);
VOID NTAPI KeInitializeDpc(PKDPC, PKDEFERRED_ROUTINE, PVOID);
BOOLEAN NTAPI KeInsertQueueDpc(PKDPC, PVOID, PVOID);
VOID NTAPI KeInitializeTimerEx(PKTIMER, TIMER_TYPE);
BOOLEAN NTAPI KeSetTimerEx(PKTIMER, LARGE_INTEGER, LONG, PKDPC);
BOOLEAN NTAPI KeCancelTimer(PKTIMER);
PVOID NTAPI ExAllocatePoolWithTag(POOL_TYPE, SIZE_T, ULONG);
VOID NTAPI ExFreePool(PVOID);
BOOLEAN NTAPI IoCancelIrp(PIRP);
NTSTATUS NTAPI KeWaitForSingleObject(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG, PVOID[], WAIT_TYPE, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER, PVOID);
KIRQL NTAPI KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK);
VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK, KIRQL);
BOOLEAN NTAPI KeTryToAcquireSpinLockAtDpcLevel(PKSPIN_LOCK);
VOID NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK);
VOID NTAPI KeFlushQueuedDpcs(void);
BOOLEAN NTAPI KeSetTimer(PKTIMER, LARGE_INTEGER, PKDPC);
VOID NTAPI IoDeleteDevice(PDEVICE_OBJECT);
NTSTATUS NTAPI IoGetDeviceObjectPointer(PUNICODE_STRING, ACCESS_MASK, PFILE_OBJECT*, PDEVICE_OBJECT*);
NTSTATUS NTAPI IoCreateDriver(PUNICODE_STRING, PDRIVER_INITIALIZE);
VOID NTAPI IoDeleteDriver(PDRIVER_OBJECT);
}
