// A minimal KMDF framework: the WDF function table plus the WDFLDR bind protocol.
// MIT, written against the public KMDF 1.33 headers (third_party/Windows-Driver-Frameworks).
//
// Decision (see drivers/README.md "KMDF"): Microsoft's framework (WDF01000.sys, about 250 C++
// files) needs the WDK km headers, the kernel CRT and a PnP manager; it is not built here.
// Instead a KMDF driver built by the WDK binds to this shim exactly like it binds to WDFLDR.SYS:
// its stub calls WdfVersionBind, receives a table of 458 function pointers and calls
// WdfFunctions_01033[index](WdfDriverGlobals, ...). Implemented: the control-device model
//   WdfDriverCreate, WdfControlDeviceInitAllocate, WdfDeviceInitSet*/AssignName/SetFileObjectConfig,
//   WdfDeviceCreate, WdfDeviceCreateSymbolicLink, WdfControlFinishInitializing, WdfIoQueueCreate
//   (sequential / parallel), request dispatch for create/close/cleanup/read/write/device control,
//   WdfRequestRetrieve*Buffer, WdfRequestComplete*, WdfRequestGetParameters, object contexts and
//   WdfObjectDelete with cleanup/destroy callbacks, spin locks, wait locks.
// Every other table slot is a loud STATUS_NOT_IMPLEMENTED stub named after its WDF function.
// Not implemented: PnP/power (no PnP manager: EvtDriverDeviceAdd is never called), manual
// queues, I/O targets, timers, DPCs, work items, DMA, USB, interrupts.
#include "../ntoskrnl/internal.h"
#include "wdf_types.h"
#include <deque>
#include <map>
#include <memory>

namespace ntk {
NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT*);
NTSTATUS NTAPI IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
NTSTATUS NTAPI IoDeleteSymbolicLink(PUNICODE_STRING);
VOID NTAPI IofCompleteRequest(PIRP, CCHAR);
}

namespace win10xbox::drivers::detail {
namespace {

constexpr ULONG kFuncTableEntries = 458;   // WdfFunctionTableNumEntries of 1.33
constexpr ULONG kFuncEntriesV1_13 = 438;   // clients up to 1.13 get the table copied instead of referenced
constexpr std::uint32_t kMagic = 0x57444631;   // 'WDF1'

struct FuncName { const char* name; ULONG index; };
const FuncName kFuncNames[] = {
#if __has_include("wdf_func_index.inc")
#define WDF_FUNC(n, i) {#n, i},
#include "wdf_func_index.inc"
#undef WDF_FUNC
#endif
    {nullptr, 0},
};

enum class WType : std::uint32_t { Driver = 1, Device, Queue, Request, File, Lock };

struct ContextRec { PCWDF_OBJECT_CONTEXT_TYPE_INFO type; void* mem; };

struct WObj {
    std::uint32_t magic = kMagic;
    WType type;
    WObj* parent = nullptr;
    std::vector<WObj*> children;
    std::vector<ContextRec> contexts;
    PVOID cleanup = nullptr, destroy = nullptr;
    explicit WObj(WType t) : type(t) {}
    virtual ~WObj() = default;
};

struct WDevice;
struct WQueue;
struct WRequest;

struct WDriver : WObj {
    PDRIVER_OBJECT drv = nullptr;
    WDF_DRIVER_CONFIG config{};
    std::wstring registry_path;
    PWDF_DRIVER_GLOBALS globals = nullptr;
    WDriver() : WObj(WType::Driver) {}
};

struct WDeviceInit {
    WDriver* driver = nullptr;
    bool control = true;
    bool has_name = false;
    std::wstring name;
    ULONG type = FILE_DEVICE_UNKNOWN;
    ULONG characteristics = 0;
    bool exclusive = false;
    int io_type = 1;   // WdfDeviceIoBuffered
    bool has_file_config = false;
    WDF_FILEOBJECT_CONFIG file_config{};
};

struct WDevice : WObj {
    WDriver* driver = nullptr;
    PDEVICE_OBJECT dev = nullptr;
    std::wstring name;
    std::vector<std::wstring> links;
    bool has_file_config = false;
    WDF_FILEOBJECT_CONFIG file_config{};
    WQueue* default_queue = nullptr;
    WDevice() : WObj(WType::Device) {}
};

struct WQueue : WObj {
    WDevice* device = nullptr;
    WDF_IO_QUEUE_CONFIG config{};
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<WRequest*> pending;
    WRequest* current = nullptr;
    WQueue() : WObj(WType::Queue) {}
};

struct WFile : WObj {
    WDevice* device = nullptr;
    PFILE_OBJECT fo = nullptr;
    WFile() : WObj(WType::File) {}
};

struct WRequest : WObj {
    PIRP irp = nullptr;
    WQueue* queue = nullptr;
    WFile* file = nullptr;
    WRequest() : WObj(WType::Request) {}
};

struct WLock : WObj {
    KSPIN_LOCK spin = 0;
    KIRQL old = 0;
    CRITICAL_SECTION cs;
    bool wait_lock = false;
    WLock() : WObj(WType::Lock) { InitializeCriticalSection(&cs); }
    ~WLock() override { DeleteCriticalSection(&cs); }
};

SRWLOCK g_lock = SRWLOCK_INIT;
void* g_table[kFuncTableEntries];
bool g_table_ready = false;
StubArena* g_arena = nullptr;
std::map<PDRIVER_OBJECT, WDriver*>& Drivers() { static std::map<PDRIVER_OBJECT, WDriver*> m; return m; }
std::map<PDRIVER_OBJECT, PWDF_DRIVER_GLOBALS>& Bound() { static std::map<PDRIVER_OBJECT, PWDF_DRIVER_GLOBALS> m; return m; }

template <class Fn> Fn Call(PVOID p) { return reinterpret_cast<Fn>(p); }

bool Valid(const void* h, WType t) {
    if (!h) return false;
    const auto* o = static_cast<const WObj*>(h);
    return o->magic == kMagic && o->type == t;
}

PCWDF_OBJECT_CONTEXT_TYPE_INFO Unique(PCWDF_OBJECT_CONTEXT_TYPE_INFO t) { return t && t->UniqueType ? t->UniqueType : t; }

void AddContext(WObj* o, const WDF_OBJECT_ATTRIBUTES* a) {
    if (!a || a->Size < sizeof(WDF_OBJECT_ATTRIBUTES)) return;
    o->cleanup = a->EvtCleanupCallback;
    o->destroy = a->EvtDestroyCallback;
    if (a->ContextTypeInfo) {
        size_t size = a->ContextSizeOverride ? a->ContextSizeOverride : a->ContextTypeInfo->ContextSize;
        void* mem = PoolAlloc(size ? size : 1, NTK_TAG('W', 'd', 'f', 'C'));
        o->contexts.push_back({Unique(a->ContextTypeInfo), mem});
    }
}

void Link(WObj* o, WObj* parent) {
    o->parent = parent;
    if (parent) parent->children.push_back(o);
}

void DeleteObject(WObj* o);

void DeleteChildren(WObj* o) {
    while (!o->children.empty()) {
        WObj* c = o->children.back();
        o->children.pop_back();
        c->parent = nullptr;
        DeleteObject(c);
    }
}

void DeleteDeviceObjects(WDevice* d) {
    for (const std::wstring& l : d->links) {
        UNICODE_STRING us;
        us.Buffer = const_cast<PWSTR>(l.c_str());
        us.Length = static_cast<USHORT>(l.size() * 2);
        us.MaximumLength = us.Length + 2;
        ntk::IoDeleteSymbolicLink(&us);
    }
    d->links.clear();
    if (d->dev) {
        PDEVICE_OBJECT dev = d->dev;
        d->dev = nullptr;
        ntk::IoDeleteDevice(dev);
    }
}

void DeleteObject(WObj* o) {
    if (!o || o->magic != kMagic) return;
    DeleteChildren(o);
    if (o->cleanup) Call<VOID(NTAPI*)(WDFOBJECT)>(o->cleanup)(o);
    if (o->destroy) Call<VOID(NTAPI*)(WDFOBJECT)>(o->destroy)(o);
    if (o->type == WType::Device) DeleteDeviceObjects(static_cast<WDevice*>(o));
    if (o->parent) {
        auto& v = o->parent->children;
        for (auto it = v.begin(); it != v.end(); ++it)
            if (*it == o) { v.erase(it); break; }
    }
    for (ContextRec& c : o->contexts) PoolFree(c.mem, 0, false);
    o->magic = 0;
    delete o;
}

// ---- request plumbing ---------------------------------------------------------------------------------------------

WDevice* DeviceOf(PDEVICE_OBJECT dev) {
    return dev && dev->DeviceExtension ? *static_cast<WDevice**>(dev->DeviceExtension) : nullptr;
}

NTSTATUS CompleteIrp(PIRP irp, NTSTATUS st, ULONG_PTR info) {
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    ntk::IofCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

void Present(WQueue* q, WRequest* r);

void CompleteRequest(WRequest* r, NTSTATUS st, ULONG_PTR info, bool use_info) {
    PIRP irp = r->irp;
    WQueue* q = r->queue;
    if (!use_info) info = irp->IoStatus.Information;
    // WDF keeps the information set by WdfRequestSetInformation in the IRP.
    r->irp = nullptr;
    DeleteObject(r);
    CompleteIrp(irp, st, info);
    if (!q) return;
    // Sequential queue: the completed request was the current one, present the next.
    WRequest* next = nullptr;
    AcquireSRWLockExclusive(&q->lock);
    if (q->config.DispatchType == WDF_IO_QUEUE_DISPATCH_SEQUENTIAL) {
        q->current = nullptr;
        if (!q->pending.empty()) {
            next = q->pending.front();
            q->pending.pop_front();
            q->current = next;
        }
    }
    ReleaseSRWLockExclusive(&q->lock);
    if (next) Present(q, next);
}

void Present(WQueue* q, WRequest* r) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(r->irp);
    const WDF_IO_QUEUE_CONFIG& c = q->config;
    switch (s->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL:
        if (c.EvtIoDeviceControl) {
            Call<VOID(NTAPI*)(WDFQUEUE, WDFREQUEST, size_t, size_t, ULONG)>(c.EvtIoDeviceControl)(
                q, r, s->Parameters.DeviceIoControl.OutputBufferLength, s->Parameters.DeviceIoControl.InputBufferLength,
                s->Parameters.DeviceIoControl.IoControlCode);
            return;
        }
        break;
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        if (c.EvtIoInternalDeviceControl) {
            Call<VOID(NTAPI*)(WDFQUEUE, WDFREQUEST, size_t, size_t, ULONG)>(c.EvtIoInternalDeviceControl)(
                q, r, s->Parameters.DeviceIoControl.OutputBufferLength, s->Parameters.DeviceIoControl.InputBufferLength,
                s->Parameters.DeviceIoControl.IoControlCode);
            return;
        }
        break;
    case IRP_MJ_READ:
        if (c.EvtIoRead) {
            Call<VOID(NTAPI*)(WDFQUEUE, WDFREQUEST, size_t)>(c.EvtIoRead)(q, r, s->Parameters.Read.Length);
            return;
        }
        break;
    case IRP_MJ_WRITE:
        if (c.EvtIoWrite) {
            Call<VOID(NTAPI*)(WDFQUEUE, WDFREQUEST, size_t)>(c.EvtIoWrite)(q, r, s->Parameters.Write.Length);
            return;
        }
        break;
    default: break;
    }
    if (c.EvtIoDefault) {
        Call<VOID(NTAPI*)(WDFQUEUE, WDFREQUEST)>(c.EvtIoDefault)(q, r);
        return;
    }
    CompleteRequest(r, STATUS_INVALID_DEVICE_REQUEST, 0, true);
}

WRequest* NewRequest(PIRP irp, WObj* parent, WFile* file) {
    auto* r = new WRequest;
    r->irp = irp;
    r->file = file;
    Link(r, parent);
    return r;
}

NTSTATUS NTAPI ShimDispatch(PDEVICE_OBJECT dev, PIRP irp) {
    WDevice* d = DeviceOf(dev);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (!d) return CompleteIrp(irp, STATUS_INVALID_DEVICE_STATE, 0);
    switch (s->MajorFunction) {
    case IRP_MJ_CREATE: {
        auto* f = new WFile;
        f->device = d;
        f->fo = s->FileObject;
        Link(f, d);
        if (f->fo) f->fo->FsContext2 = f;
        if (d->has_file_config && d->file_config.EvtDeviceFileCreate) {
            WRequest* r = NewRequest(irp, d, f);
            IoMarkIrpPending(irp);
            Call<VOID(NTAPI*)(WDFDEVICE, WDFREQUEST, WDFFILEOBJECT)>(d->file_config.EvtDeviceFileCreate)(d, r, f);
            return STATUS_PENDING;
        }
        return CompleteIrp(irp, STATUS_SUCCESS, FILE_OPENED);
    }
    case IRP_MJ_CLEANUP: {
        auto* f = s->FileObject ? static_cast<WFile*>(s->FileObject->FsContext2) : nullptr;
        if (Valid(f, WType::File) && d->has_file_config && d->file_config.EvtFileCleanup)
            Call<VOID(NTAPI*)(WDFFILEOBJECT)>(d->file_config.EvtFileCleanup)(f);
        return CompleteIrp(irp, STATUS_SUCCESS, 0);
    }
    case IRP_MJ_CLOSE: {
        auto* f = s->FileObject ? static_cast<WFile*>(s->FileObject->FsContext2) : nullptr;
        if (Valid(f, WType::File)) {
            if (d->has_file_config && d->file_config.EvtFileClose) Call<VOID(NTAPI*)(WDFFILEOBJECT)>(d->file_config.EvtFileClose)(f);
            s->FileObject->FsContext2 = nullptr;
            DeleteObject(f);
        }
        return CompleteIrp(irp, STATUS_SUCCESS, 0);
    }
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL: {
        WQueue* q = d->default_queue;
        if (!q) return CompleteIrp(irp, STATUS_INVALID_DEVICE_STATE, 0);
        auto* f = s->FileObject ? static_cast<WFile*>(s->FileObject->FsContext2) : nullptr;
        WRequest* r = NewRequest(irp, q, Valid(f, WType::File) ? f : nullptr);
        r->queue = q;
        IoMarkIrpPending(irp);
        bool deliver = true;
        AcquireSRWLockExclusive(&q->lock);
        if (q->config.DispatchType == WDF_IO_QUEUE_DISPATCH_SEQUENTIAL) {
            if (q->current) { q->pending.push_back(r); deliver = false; }
            else q->current = r;
        } else if (q->config.DispatchType == WDF_IO_QUEUE_DISPATCH_MANUAL) {
            q->pending.push_back(r);
            deliver = false;
            FIXME(driver, "manual WDF queues are not supported: request parked");
        }
        ReleaseSRWLockExclusive(&q->lock);
        if (deliver) Present(q, r);
        return STATUS_PENDING;
    }
    default:
        return CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

VOID NTAPI ShimUnload(PDRIVER_OBJECT drv) {
    WDriver* d = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    auto it = Drivers().find(drv);
    if (it != Drivers().end()) { d = it->second; Drivers().erase(it); }
    ReleaseSRWLockExclusive(&g_lock);
    if (!d) return;
    if (d->config.EvtDriverUnload) Call<VOID(NTAPI*)(WDFDRIVER)>(d->config.EvtDriverUnload)(d);
    DeleteObject(d);
}

// ---- table functions ---------------------------------------------------------------------------------------------------

NTSTATUS NTAPI F_WdfDriverCreate(PWDF_DRIVER_GLOBALS g, PDRIVER_OBJECT drv, PCUNICODE_STRING reg,
                                 PWDF_OBJECT_ATTRIBUTES attr, PWDF_DRIVER_CONFIG cfg, WDFDRIVER* out) {
    if (!drv || !reg || !cfg || cfg->Size < sizeof(WDF_DRIVER_CONFIG)) return STATUS_INVALID_PARAMETER;
    auto* d = new WDriver;
    d->drv = drv;
    d->config = *cfg;
    d->registry_path = ToWide(*reg);
    d->globals = g;
    AddContext(d, attr);
    if (!(cfg->DriverInitFlags & WDF_DRIVER_INIT_NON_PNP_DRIVER) && cfg->EvtDriverDeviceAdd)
        FIXME(driver, "WdfDriverCreate: PnP drivers are not supported (no PnP manager); EvtDriverDeviceAdd will never be called, "
                      "use WdfDriverInitNonPnpDriver and a control device");
    if (g) g->Driver = d;
    for (UCHAR m : {IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP, IRP_MJ_READ, IRP_MJ_WRITE, IRP_MJ_DEVICE_CONTROL,
                    IRP_MJ_INTERNAL_DEVICE_CONTROL})
        drv->MajorFunction[m] = &ShimDispatch;
    drv->DriverUnload = &ShimUnload;
    AcquireSRWLockExclusive(&g_lock);
    Drivers()[drv] = d;
    ReleaseSRWLockExclusive(&g_lock);
    if (out) *out = d;
    return STATUS_SUCCESS;
}

PWDFDEVICE_INIT NTAPI F_WdfControlDeviceInitAllocate(PWDF_DRIVER_GLOBALS, WDFDRIVER driver, const UNICODE_STRING* sddl) {
    if (!Valid(driver, WType::Driver)) return nullptr;
    if (sddl) TRACE(driver, "WdfControlDeviceInitAllocate: SDDL %ls ignored (no access checks in the host)", ToWide(*sddl).c_str());
    auto* init = new WDeviceInit;
    init->driver = static_cast<WDriver*>(driver);
    init->control = true;
    return reinterpret_cast<PWDFDEVICE_INIT>(init);
}
VOID NTAPI F_WdfDeviceInitFree(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init) { delete reinterpret_cast<WDeviceInit*>(init); }
VOID NTAPI F_WdfDeviceInitSetDeviceType(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, ULONG type) { reinterpret_cast<WDeviceInit*>(init)->type = type; }
VOID NTAPI F_WdfDeviceInitSetExclusive(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, BOOLEAN excl) { reinterpret_cast<WDeviceInit*>(init)->exclusive = excl != 0; }
VOID NTAPI F_WdfDeviceInitSetCharacteristics(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, ULONG chars, BOOLEAN or_in) {
    auto* i = reinterpret_cast<WDeviceInit*>(init);
    i->characteristics = or_in ? (i->characteristics | chars) : chars;
}
VOID NTAPI F_WdfDeviceInitSetIoType(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, int io_type) { reinterpret_cast<WDeviceInit*>(init)->io_type = io_type; }
NTSTATUS NTAPI F_WdfDeviceInitAssignName(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, PCUNICODE_STRING name) {
    auto* i = reinterpret_cast<WDeviceInit*>(init);
    i->has_name = name != nullptr && name->Length != 0;
    i->name = name ? ToWide(*name) : std::wstring();
    return STATUS_SUCCESS;
}
VOID NTAPI F_WdfDeviceInitSetFileObjectConfig(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT init, PWDF_FILEOBJECT_CONFIG cfg, PWDF_OBJECT_ATTRIBUTES) {
    auto* i = reinterpret_cast<WDeviceInit*>(init);
    if (cfg && cfg->Size >= sizeof(WDF_FILEOBJECT_CONFIG)) { i->file_config = *cfg; i->has_file_config = true; }
}

NTSTATUS NTAPI F_WdfDeviceCreate(PWDF_DRIVER_GLOBALS, PWDFDEVICE_INIT* pinit, PWDF_OBJECT_ATTRIBUTES attr, WDFDEVICE* out) {
    if (!pinit || !*pinit || !out) return STATUS_INVALID_PARAMETER;
    std::unique_ptr<WDeviceInit> init(reinterpret_cast<WDeviceInit*>(*pinit));
    *pinit = nullptr;
    WDriver* drv = init->driver;
    UNICODE_STRING name{};
    if (init->has_name) {
        name.Buffer = const_cast<PWSTR>(init->name.c_str());
        name.Length = static_cast<USHORT>(init->name.size() * 2);
        name.MaximumLength = name.Length + 2;
    }
    PDEVICE_OBJECT dev = nullptr;
    const ULONG chars = init->characteristics | (init->control ? FILE_DEVICE_SECURE_OPEN : 0);
    NTSTATUS st = ntk::IoCreateDevice(drv->drv, sizeof(void*), init->has_name ? &name : nullptr, init->type, chars,
                                      init->exclusive, &dev);
    if (!NT_SUCCESS(st)) return st;
    auto* d = new WDevice;
    d->driver = drv;
    d->dev = dev;
    d->name = init->name;
    d->has_file_config = init->has_file_config;
    d->file_config = init->file_config;
    *static_cast<WDevice**>(dev->DeviceExtension) = d;
    if (init->io_type == 1) dev->Flags |= DO_BUFFERED_IO;          // WdfDeviceIoBuffered
    else if (init->io_type == 2) dev->Flags |= DO_DIRECT_IO;       // WdfDeviceIoDirect
    AddContext(d, attr);
    Link(d, attr && attr->ParentObject ? static_cast<WObj*>(attr->ParentObject) : drv);
    *out = d;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI F_WdfDeviceCreateSymbolicLink(PWDF_DRIVER_GLOBALS, WDFDEVICE device, PCUNICODE_STRING link) {
    if (!Valid(device, WType::Device) || !link) return STATUS_INVALID_PARAMETER;
    auto* d = static_cast<WDevice*>(device);
    if (d->name.empty()) return STATUS_INVALID_DEVICE_STATE;
    UNICODE_STRING target;
    target.Buffer = const_cast<PWSTR>(d->name.c_str());
    target.Length = static_cast<USHORT>(d->name.size() * 2);
    target.MaximumLength = target.Length + 2;
    NTSTATUS st = ntk::IoCreateSymbolicLink(const_cast<PUNICODE_STRING>(link), &target);
    if (NT_SUCCESS(st)) d->links.push_back(ToWide(*link));
    return st;
}

PDEVICE_OBJECT NTAPI F_WdfDeviceWdmGetDeviceObject(PWDF_DRIVER_GLOBALS, WDFDEVICE device) {
    return Valid(device, WType::Device) ? static_cast<WDevice*>(device)->dev : nullptr;
}
WDFDRIVER NTAPI F_WdfDeviceGetDriver(PWDF_DRIVER_GLOBALS, WDFDEVICE device) {
    return Valid(device, WType::Device) ? static_cast<WDevice*>(device)->driver : nullptr;
}
VOID NTAPI F_WdfControlFinishInitializing(PWDF_DRIVER_GLOBALS, WDFDEVICE device) {
    if (Valid(device, WType::Device)) static_cast<WDevice*>(device)->dev->Flags &= ~static_cast<ULONG>(DO_DEVICE_INITIALIZING);
}
PDRIVER_OBJECT NTAPI F_WdfDriverWdmGetDriverObject(PWDF_DRIVER_GLOBALS, WDFDRIVER driver) {
    return Valid(driver, WType::Driver) ? static_cast<WDriver*>(driver)->drv : nullptr;
}
PWSTR NTAPI F_WdfDriverGetRegistryPath(PWDF_DRIVER_GLOBALS, WDFDRIVER driver) {
    return Valid(driver, WType::Driver) ? const_cast<PWSTR>(static_cast<WDriver*>(driver)->registry_path.c_str()) : nullptr;
}

NTSTATUS NTAPI F_WdfIoQueueCreate(PWDF_DRIVER_GLOBALS, WDFDEVICE device, PWDF_IO_QUEUE_CONFIG cfg, PWDF_OBJECT_ATTRIBUTES attr,
                                  WDFQUEUE* out) {
    if (!Valid(device, WType::Device) || !cfg || cfg->Size < sizeof(WDF_IO_QUEUE_CONFIG)) return STATUS_INVALID_PARAMETER;
    auto* d = static_cast<WDevice*>(device);
    if (cfg->DefaultQueue && d->default_queue) return STATUS_INVALID_DEVICE_STATE;
    auto* q = new WQueue;
    q->device = d;
    q->config = *cfg;
    AddContext(q, attr);
    Link(q, d);
    if (cfg->DefaultQueue) d->default_queue = q;
    if (out) *out = q;
    else if (!cfg->DefaultQueue) { DeleteObject(q); return STATUS_INVALID_PARAMETER; }
    return STATUS_SUCCESS;
}
WDFDEVICE NTAPI F_WdfIoQueueGetDevice(PWDF_DRIVER_GLOBALS, WDFQUEUE q) {
    return Valid(q, WType::Queue) ? static_cast<WQueue*>(q)->device : nullptr;
}

VOID NTAPI F_WdfRequestComplete(PWDF_DRIVER_GLOBALS, WDFREQUEST request, NTSTATUS st) {
    if (!Valid(request, WType::Request)) return;
    CompleteRequest(static_cast<WRequest*>(request), st, 0, false);
}
VOID NTAPI F_WdfRequestCompleteWithInformation(PWDF_DRIVER_GLOBALS, WDFREQUEST request, NTSTATUS st, ULONG_PTR info) {
    if (!Valid(request, WType::Request)) return;
    CompleteRequest(static_cast<WRequest*>(request), st, info, true);
}
VOID NTAPI F_WdfRequestSetInformation(PWDF_DRIVER_GLOBALS, WDFREQUEST request, ULONG_PTR info) {
    if (Valid(request, WType::Request)) static_cast<WRequest*>(request)->irp->IoStatus.Information = info;
}
ULONG_PTR NTAPI F_WdfRequestGetInformation(PWDF_DRIVER_GLOBALS, WDFREQUEST request) {
    return Valid(request, WType::Request) ? static_cast<WRequest*>(request)->irp->IoStatus.Information : 0;
}
PIRP NTAPI F_WdfRequestWdmGetIrp(PWDF_DRIVER_GLOBALS, WDFREQUEST request) {
    return Valid(request, WType::Request) ? static_cast<WRequest*>(request)->irp : nullptr;
}
KPROCESSOR_MODE NTAPI F_WdfRequestGetRequestorMode(PWDF_DRIVER_GLOBALS, WDFREQUEST request) {
    return Valid(request, WType::Request) ? static_cast<WRequest*>(request)->irp->RequestorMode : KernelMode;
}
WDFFILEOBJECT NTAPI F_WdfRequestGetFileObject(PWDF_DRIVER_GLOBALS, WDFREQUEST request) {
    return Valid(request, WType::Request) ? static_cast<WRequest*>(request)->file : nullptr;
}
WDFQUEUE NTAPI F_WdfRequestGetIoQueue(PWDF_DRIVER_GLOBALS, WDFREQUEST request) {
    return Valid(request, WType::Request) ? static_cast<WRequest*>(request)->queue : nullptr;
}
PFILE_OBJECT NTAPI F_WdfFileObjectWdmGetFileObject(PWDF_DRIVER_GLOBALS, WDFFILEOBJECT file) {
    return Valid(file, WType::File) ? static_cast<WFile*>(file)->fo : nullptr;
}
WDFDEVICE NTAPI F_WdfFileObjectGetDevice(PWDF_DRIVER_GLOBALS, WDFFILEOBJECT file) {
    return Valid(file, WType::File) ? static_cast<WFile*>(file)->device : nullptr;
}

void NTAPI F_WdfRequestGetParameters(PWDF_DRIVER_GLOBALS, WDFREQUEST request, PWDF_REQUEST_PARAMETERS p) {
    if (!Valid(request, WType::Request) || !p) return;
    PIRP irp = static_cast<WRequest*>(request)->irp;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    const USHORT size = p->Size;
    std::memset(p, 0, sizeof(*p));
    p->Size = size;
    p->MinorFunction = s->MinorFunction;
    p->Type = s->MajorFunction;   // WDF_REQUEST_TYPE values equal the IRP_MJ_* codes
    switch (s->MajorFunction) {
    case IRP_MJ_CREATE:
        p->Parameters.Create.SecurityContext = s->Parameters.Create.SecurityContext;
        p->Parameters.Create.Options = s->Parameters.Create.Options;
        p->Parameters.Create.FileAttributes = s->Parameters.Create.FileAttributes;
        p->Parameters.Create.ShareAccess = s->Parameters.Create.ShareAccess;
        p->Parameters.Create.EaLength = s->Parameters.Create.EaLength;
        break;
    case IRP_MJ_READ:
        p->Parameters.Read.Length = s->Parameters.Read.Length;
        p->Parameters.Read.Key = s->Parameters.Read.Key;
        p->Parameters.Read.DeviceOffset = s->Parameters.Read.ByteOffset.QuadPart;
        break;
    case IRP_MJ_WRITE:
        p->Parameters.Write.Length = s->Parameters.Write.Length;
        p->Parameters.Write.Key = s->Parameters.Write.Key;
        p->Parameters.Write.DeviceOffset = s->Parameters.Write.ByteOffset.QuadPart;
        break;
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        p->Parameters.DeviceIoControl.OutputBufferLength = s->Parameters.DeviceIoControl.OutputBufferLength;
        p->Parameters.DeviceIoControl.InputBufferLength = s->Parameters.DeviceIoControl.InputBufferLength;
        p->Parameters.DeviceIoControl.IoControlCode = s->Parameters.DeviceIoControl.IoControlCode;
        p->Parameters.DeviceIoControl.Type3InputBuffer = s->Parameters.DeviceIoControl.Type3InputBuffer;
        break;
    default: break;
    }
}

// Input buffer: write data / ioctl input.  Output buffer: read data / ioctl output.
NTSTATUS Retrieve(WDFREQUEST request, size_t min, PVOID* buffer, size_t* length, bool input) {
    if (!Valid(request, WType::Request) || !buffer) return STATUS_INVALID_PARAMETER;
    PIRP irp = static_cast<WRequest*>(request)->irp;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    size_t len = 0;
    PVOID buf = nullptr;
    switch (s->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL: {
        const ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
        len = input ? s->Parameters.DeviceIoControl.InputBufferLength : s->Parameters.DeviceIoControl.OutputBufferLength;
        switch (code & 3) {
        case METHOD_BUFFERED: buf = irp->AssociatedIrp.SystemBuffer; break;
        case METHOD_IN_DIRECT:
        case METHOD_OUT_DIRECT:
            buf = input ? irp->AssociatedIrp.SystemBuffer : (irp->MdlAddress ? irp->MdlAddress->MappedSystemVa : nullptr);
            break;
        default: buf = input ? s->Parameters.DeviceIoControl.Type3InputBuffer : irp->UserBuffer; break;
        }
        break;
    }
    case IRP_MJ_WRITE:
        if (!input) return STATUS_INVALID_DEVICE_REQUEST;
        len = s->Parameters.Write.Length;
        buf = (irp->Flags & IRP_BUFFERED_IO) ? irp->AssociatedIrp.SystemBuffer
              : irp->MdlAddress ? irp->MdlAddress->MappedSystemVa : irp->UserBuffer;
        break;
    case IRP_MJ_READ:
        if (input) return STATUS_INVALID_DEVICE_REQUEST;
        len = s->Parameters.Read.Length;
        buf = (irp->Flags & IRP_BUFFERED_IO) ? irp->AssociatedIrp.SystemBuffer
              : irp->MdlAddress ? irp->MdlAddress->MappedSystemVa : irp->UserBuffer;
        break;
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (len < min) return STATUS_BUFFER_TOO_SMALL;
    if (len == 0 || !buf) { if (min) return STATUS_INSUFFICIENT_RESOURCES; }
    *buffer = buf;
    if (length) *length = len;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI F_WdfRequestRetrieveInputBuffer(PWDF_DRIVER_GLOBALS, WDFREQUEST r, size_t min, PVOID* buf, size_t* len) {
    return Retrieve(r, min, buf, len, true);
}
NTSTATUS NTAPI F_WdfRequestRetrieveOutputBuffer(PWDF_DRIVER_GLOBALS, WDFREQUEST r, size_t min, PVOID* buf, size_t* len) {
    return Retrieve(r, min, buf, len, false);
}

PVOID NTAPI F_WdfObjectGetTypedContextWorker(PWDF_DRIVER_GLOBALS, WDFOBJECT handle, PCWDF_OBJECT_CONTEXT_TYPE_INFO info) {
    if (!handle || !info) return nullptr;
    auto* o = static_cast<WObj*>(handle);
    if (o->magic != kMagic) return nullptr;
    for (const ContextRec& c : o->contexts)
        if (c.type == Unique(info)) return c.mem;
    return nullptr;
}
NTSTATUS NTAPI F_WdfObjectAllocateContext(PWDF_DRIVER_GLOBALS, WDFOBJECT handle, PWDF_OBJECT_ATTRIBUTES attr, PVOID* out) {
    if (!handle || !attr || !attr->ContextTypeInfo) return STATUS_INVALID_PARAMETER;
    auto* o = static_cast<WObj*>(handle);
    if (o->magic != kMagic) return STATUS_INVALID_HANDLE;
    const size_t before = o->contexts.size();
    PVOID keep_cleanup = o->cleanup, keep_destroy = o->destroy;
    AddContext(o, attr);
    if (!o->cleanup) o->cleanup = keep_cleanup;
    if (!o->destroy) o->destroy = keep_destroy;
    if (out) *out = o->contexts.size() > before ? o->contexts.back().mem : nullptr;
    return STATUS_SUCCESS;
}
VOID NTAPI F_WdfObjectDelete(PWDF_DRIVER_GLOBALS, WDFOBJECT object) {
    auto* o = static_cast<WObj*>(object);
    if (!o || o->magic != kMagic) return;
    if (o->type == WType::Request) {
        ERR(driver, "WdfObjectDelete on a request: complete it with WdfRequestComplete instead");
        return;
    }
    DeleteObject(o);
}
VOID NTAPI F_WdfObjectReferenceActual(PWDF_DRIVER_GLOBALS, WDFOBJECT, PVOID, LONG, PCCH) {}
VOID NTAPI F_WdfObjectDereferenceActual(PWDF_DRIVER_GLOBALS, WDFOBJECT, PVOID, LONG, PCCH) {}

// Locks.
NTSTATUS NewLock(PWDF_OBJECT_ATTRIBUTES attr, PVOID* out, bool wait) {
    auto* l = new WLock;
    l->wait_lock = wait;
    AddContext(l, attr);
    Link(l, attr && attr->ParentObject ? static_cast<WObj*>(attr->ParentObject) : nullptr);
    *out = l;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI F_WdfSpinLockCreate(PWDF_DRIVER_GLOBALS, PWDF_OBJECT_ATTRIBUTES attr, WDFSPINLOCK* out) { return NewLock(attr, out, false); }
VOID NTAPI F_WdfSpinLockAcquire(PWDF_DRIVER_GLOBALS, WDFSPINLOCK lock) {
    auto* l = static_cast<WLock*>(lock);
    l->old = SpinAcquireRaise(&l->spin, DISPATCH_LEVEL);
}
VOID NTAPI F_WdfSpinLockRelease(PWDF_DRIVER_GLOBALS, WDFSPINLOCK lock) {
    auto* l = static_cast<WLock*>(lock);
    SpinReleaseLower(&l->spin, l->old);
}
NTSTATUS NTAPI F_WdfWaitLockCreate(PWDF_DRIVER_GLOBALS, PWDF_OBJECT_ATTRIBUTES attr, WDFWAITLOCK* out) { return NewLock(attr, out, true); }
NTSTATUS NTAPI F_WdfWaitLockAcquire(PWDF_DRIVER_GLOBALS, WDFWAITLOCK lock, PLONGLONG timeout) {
    auto* l = static_cast<WLock*>(lock);
    if (timeout && *timeout == 0) return TryEnterCriticalSection(&l->cs) ? STATUS_SUCCESS : STATUS_TIMEOUT;
    EnterCriticalSection(&l->cs);   // a finite timeout is honoured as "wait" (documented)
    return STATUS_SUCCESS;
}
VOID NTAPI F_WdfWaitLockRelease(PWDF_DRIVER_GLOBALS, WDFWAITLOCK lock) { LeaveCriticalSection(&static_cast<WLock*>(lock)->cs); }

// ---- the table ---------------------------------------------------------------------------------------------------------------

struct Impl { const char* name; void* fn; };
#define F(n) {#n, reinterpret_cast<void*>(&F_##n)}
const Impl kImpls[] = {
    F(WdfDriverCreate), F(WdfDriverWdmGetDriverObject), F(WdfDriverGetRegistryPath),
    F(WdfControlDeviceInitAllocate), F(WdfDeviceInitFree), F(WdfDeviceInitSetDeviceType), F(WdfDeviceInitSetExclusive),
    F(WdfDeviceInitSetCharacteristics), F(WdfDeviceInitSetIoType), F(WdfDeviceInitAssignName),
    F(WdfDeviceInitSetFileObjectConfig), F(WdfDeviceCreate), F(WdfDeviceCreateSymbolicLink),
    F(WdfDeviceWdmGetDeviceObject), F(WdfDeviceGetDriver), F(WdfControlFinishInitializing),
    F(WdfIoQueueCreate), F(WdfIoQueueGetDevice),
    F(WdfRequestComplete), F(WdfRequestCompleteWithInformation), F(WdfRequestSetInformation), F(WdfRequestGetInformation),
    F(WdfRequestWdmGetIrp), F(WdfRequestGetRequestorMode), F(WdfRequestGetFileObject), F(WdfRequestGetIoQueue),
    F(WdfFileObjectWdmGetFileObject), F(WdfFileObjectGetDevice), F(WdfRequestGetParameters),
    F(WdfRequestRetrieveInputBuffer), F(WdfRequestRetrieveOutputBuffer),
    F(WdfObjectGetTypedContextWorker), F(WdfObjectAllocateContext), F(WdfObjectDelete),
    F(WdfObjectReferenceActual), F(WdfObjectDereferenceActual),
    F(WdfSpinLockCreate), F(WdfSpinLockAcquire), F(WdfSpinLockRelease),
    F(WdfWaitLockCreate), F(WdfWaitLockAcquire), F(WdfWaitLockRelease),
};
#undef F

void BuildTable() {
    if (g_table_ready) return;
    g_arena = new StubArena;
    const char* names[kFuncTableEntries] = {};
    for (const FuncName* f = kFuncNames; f->name; ++f)
        if (f->index < kFuncTableEntries) names[f->index] = f->name;
    for (ULONG i = 0; i < kFuncTableEntries; ++i) {
        void* fn = nullptr;
        if (names[i])
            for (const Impl& m : kImpls)
                if (!std::strcmp(m.name, names[i])) { fn = m.fn; break; }
        if (!fn) {
            char label[96];
            if (names[i]) std::snprintf(label, sizeof label, "%s", names[i]);
            else std::snprintf(label, sizeof label, "WdfFunctions[%lu]", static_cast<unsigned long>(i));
            fn = g_arena->Add(label);
        }
        g_table[i] = fn;
    }
    g_arena->Seal();
    g_table_ready = true;
}

// ---- WDFLDR.SYS exports -----------------------------------------------------------------------------------------------------------

NTSTATUS NTAPI WdfVersionBind(PDRIVER_OBJECT drv, PUNICODE_STRING reg, PWDF_BIND_INFO info, PWDF_DRIVER_GLOBALS* out_globals) {
    if (!drv || !reg || !info || !out_globals || !info->FuncTable) return STATUS_INVALID_PARAMETER;
    if (info->Size < sizeof(WDF_BIND_INFO)) return STATUS_INVALID_PARAMETER;
    if (info->FuncCount > kFuncTableEntries) {
        ERR(driver, "WdfVersionBind: client function table (%lu entries) is newer than the shim (%lu)",
            static_cast<unsigned long>(info->FuncCount), static_cast<unsigned long>(kFuncTableEntries));
        return STATUS_INVALID_PARAMETER;
    }
    AcquireSRWLockExclusive(&g_lock);
    BuildTable();
    ReleaseSRWLockExclusive(&g_lock);
    auto* g = static_cast<PWDF_DRIVER_GLOBALS>(PoolAlloc(sizeof(WDF_DRIVER_GLOBALS), NTK_TAG('W', 'd', 'f', 'G')));
    if (!g) return STATUS_INSUFFICIENT_RESOURCES;
    std::wstring r = ToWide(*reg);
    size_t slash = r.find_last_of(L'\\');
    if (slash != std::wstring::npos) r = r.substr(slash + 1);
    for (size_t i = 0; i < r.size() && i < WDF_DRIVER_GLOBALS_NAME_LEN - 1; ++i) g->DriverName[i] = static_cast<char>(r[i]);
    g->DriverFlags = 0;
    g->DriverTag = NTK_TAG('K', 'm', 'd', 'f');
    g->DisplaceDriverUnload = TRUE;   // the client stub puts its unload routine first, like the real framework asks it to
    if (info->FuncCount <= kFuncEntriesV1_13) std::memcpy(info->FuncTable, g_table, info->FuncCount * sizeof(void*));
    else *info->FuncTable = g_table;   // FuncTable arrives pointing at the driver's `WdfFunctions` variable
    if (info->Size >= sizeof(WDF_BIND_INFO2)) {
        auto* i2 = reinterpret_cast<PWDF_BIND_INFO2>(info);
        if (i2->ClientVersionHigherThanFramework) *i2->ClientVersionHigherThanFramework = FALSE;
        if (i2->MinimumVersionRequired) *i2->MinimumVersionRequired = info->Version.Minor;
        if (i2->FuncCountPtr) *i2->FuncCountPtr = info->FuncCount;
        if (i2->StructCountPtr) *i2->StructCountPtr = 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    Bound()[drv] = g;
    ReleaseSRWLockExclusive(&g_lock);
    *out_globals = g;
    TRACE(driver, "WdfVersionBind: %ls bound to KMDF %lu.%lu (%lu table entries)", r.c_str(),
          static_cast<unsigned long>(info->Version.Major), static_cast<unsigned long>(info->Version.Minor),
          static_cast<unsigned long>(info->FuncCount));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI WdfVersionUnbind(PUNICODE_STRING reg, PWDF_BIND_INFO info, PWDF_DRIVER_GLOBALS globals) {
    (void)reg; (void)info;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = Bound().begin(); it != Bound().end(); ++it)
        if (it->second == globals) { Bound().erase(it); break; }
    ReleaseSRWLockExclusive(&g_lock);
    PoolFree(globals, 0, false);
    return STATUS_SUCCESS;
}

const Export kWdfLdrTable[] = {
    {"WdfVersionBind", reinterpret_cast<void*>(&WdfVersionBind)},
    {"WdfVersionUnbind", reinterpret_cast<void*>(&WdfVersionUnbind)},
};

}  // namespace

ExportGroup WdfLdrExports() { return {kWdfLdrTable, sizeof(kWdfLdrTable) / sizeof(kWdfLdrTable[0])}; }

}  // namespace win10xbox::drivers::detail
