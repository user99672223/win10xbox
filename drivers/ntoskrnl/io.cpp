// ntoskrnl Io* surface: devices, symbolic links, attach, IRPs, completion,
// device queues, MDLs. MIT; the IRP completion walk follows the documented
// IoCompleteRequest semantics (completion routines run bottom-up, a routine
// returning STATUS_MORE_PROCESSING_REQUIRED stops the walk).
#include "internal.h"
#include <unordered_map>

namespace ntk {
VOID NTAPI KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
LONG NTAPI KeSetEvent(PKEVENT, LONG, BOOLEAN);
}

namespace win10xbox::drivers::detail {

// Bookkeeping stored in front of every IRP (32 bytes keeps the IRP 16-aligned).
struct IrpHdr {
    std::uint32_t magic;
    IrpKind kind;
    TopContext* top;
    ULONG user_out_len;
    ULONG pad;
};
static_assert(sizeof(IrpHdr) == 24 || sizeof(IrpHdr) == 32);
constexpr std::uint32_t kIrpMagic = 0x1B9A11C5u;
constexpr std::size_t kHdrSize = 32;

namespace {
inline IrpHdr* Hdr(PIRP irp) { return reinterpret_cast<IrpHdr*>(reinterpret_cast<char*>(irp) - kHdrSize); }
}  // namespace

void IrpInit(PIRP irp, USHORT packet_size, CCHAR stack_count) {
    std::memset(irp, 0, packet_size);
    irp->Type = IO_TYPE_IRP;
    irp->Size = packet_size;
    irp->StackCount = stack_count;
    irp->CurrentLocation = static_cast<CHAR>(stack_count + 1);
    irp->Tail.Overlay.CurrentStackLocation =
        reinterpret_cast<PIO_STACK_LOCATION>(reinterpret_cast<char*>(irp) + packet_size);
    InitializeListHead(&irp->ThreadListEntry);
}

PIRP IrpAllocate(CCHAR stack_count, IrpKind kind) {
    if (stack_count < 1) stack_count = 1;
    const USHORT size = IoSizeOfIrp(stack_count);
    char* mem = static_cast<char*>(PoolAlloc(kHdrSize + size, kTagIrp));
    if (!mem) return nullptr;
    auto* h = reinterpret_cast<IrpHdr*>(mem);
    h->magic = kIrpMagic;
    h->kind = kind;
    h->top = nullptr;
    PIRP irp = reinterpret_cast<PIRP>(mem + kHdrSize);
    IrpInit(irp, size, stack_count);
    return irp;
}

IrpKind IrpKindOf(PIRP irp) { return Hdr(irp)->kind; }
void IrpSetTop(PIRP irp, TopContext* t) { Hdr(irp)->top = t; }
TopContext* IrpTop(PIRP irp) { return Hdr(irp)->top; }
void IrpSetUserOutLen(PIRP irp, ULONG n) { Hdr(irp)->user_out_len = n; }

void IrpRelease(PIRP irp) {
    if (!irp) return;
    IrpHdr* h = Hdr(irp);
    if (h->magic != kIrpMagic) { ERR(driver, "IoFreeIrp(%p): not an IRP allocated by this host", static_cast<void*>(irp)); return; }
    if ((irp->Flags & IRP_BUFFERED_IO) && (irp->Flags & IRP_DEALLOCATE_BUFFER) && irp->AssociatedIrp.SystemBuffer)
        PoolFree(irp->AssociatedIrp.SystemBuffer, 0, false);
    for (PMDL m = irp->MdlAddress; m;) {
        PMDL next = m->Next;
        MdlFree(m);
        m = next;
    }
    h->magic = 0;
    PoolFree(h, 0, false);
}

// ---- MDLs -----------------------------------------------------------------------------------------------

PMDL MdlCreate(PVOID va, ULONG length) {
    const ULONG pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(va, length);
    const SIZE_T size = sizeof(MDL) + pages * sizeof(PFN_NUMBER);
    auto* m = static_cast<PMDL>(PoolAlloc(size, kTagMdl));
    if (!m) return nullptr;
    m->Size = static_cast<CSHORT>(size);
    m->StartVa = PAGE_ALIGN(va);
    m->ByteOffset = BYTE_OFFSET(va);
    m->ByteCount = length;
    return m;
}

void MdlFree(PMDL mdl) { PoolFree(mdl, 0, false); }

namespace {
void FillPfns(PMDL m) {
    // Physical memory does not exist for the host: PFNs are the virtual page
    // numbers (identity), enough for drivers that only walk the array.
    const ULONG pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(m->ByteOffset)), m->ByteCount);
    auto* pfn = reinterpret_cast<PPFN_NUMBER>(m + 1);
    for (ULONG i = 0; i < pages; ++i) pfn[i] = (reinterpret_cast<ULONG_PTR>(m->StartVa) >> PAGE_SHIFT) + i;
}
}  // namespace

// ---- I/O-manager side: devices ---------------------------------------------------------------------------

void DeviceFree(PDEVICE_OBJECT dev) {
    TRACE(driver, "device %p freed", static_cast<void*>(dev));
    PoolFree(dev->DeviceObjectExtension, 0, false);
    PoolFree(dev, 0, false);
}

PDEVICE_OBJECT NTAPI AttachedDevice(PDEVICE_OBJECT dev) {
    while (dev && dev->AttachedDevice) dev = dev->AttachedDevice;
    return dev;
}

NTSTATUS NTAPI InvalidDeviceRequest(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    CompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS NTAPI CallDriver(PDEVICE_OBJECT dev, PIRP irp) {
    if (!dev || !irp) return STATUS_INVALID_PARAMETER;
    if (--irp->CurrentLocation <= 0) {
        ERR(driver, "IofCallDriver: no more IRP stack locations (NO_MORE_IRP_STACK_LOCATIONS)");
        irp->CurrentLocation++;
        return STATUS_INVALID_PARAMETER;
    }
    PIO_STACK_LOCATION s = --irp->Tail.Overlay.CurrentStackLocation;
    s->DeviceObject = dev;
    PDRIVER_OBJECT drv = dev->DriverObject;
    if (!drv) { ERR(driver, "IofCallDriver: device %p has no driver", static_cast<void*>(dev)); return STATUS_INVALID_PARAMETER; }
    PDRIVER_DISPATCH fn = drv->MajorFunction[s->MajorFunction];
    TRACE(driver, "IRP %p major=%u minor=%u -> %p", static_cast<void*>(irp), s->MajorFunction, s->MinorFunction,
          reinterpret_cast<void*>(fn));
    return fn(dev, irp);
}

VOID NTAPI CompleteRequest(PIRP irp, CCHAR boost) {
    (void)boost;
    if (irp->CurrentLocation > irp->StackCount) {
        ERR(driver, "IoCompleteRequest(%p): no current stack location, completed twice?", static_cast<void*>(irp));
        return;
    }
    for (;;) {
        PIO_STACK_LOCATION s = irp->Tail.Overlay.CurrentStackLocation;
        const UCHAR control = s->Control;
        PIO_COMPLETION_ROUTINE routine = s->CompletionRoutine;
        PVOID ctx = s->Context;
        irp->CurrentLocation++;
        irp->Tail.Overlay.CurrentStackLocation++;
        const bool has_upper = irp->CurrentLocation <= irp->StackCount;
        irp->PendingReturned = (control & SL_PENDING_RETURNED) ? TRUE : FALSE;
        const NTSTATUS st = irp->IoStatus.Status;
        bool invoke = false;
        if (routine) {
            if (NT_SUCCESS(st)) invoke = (control & SL_INVOKE_ON_SUCCESS) != 0;
            else invoke = (control & SL_INVOKE_ON_ERROR) != 0;
            if (irp->Cancel && (control & SL_INVOKE_ON_CANCEL)) invoke = true;
        }
        if (invoke) {
            PDEVICE_OBJECT upper = has_upper ? irp->Tail.Overlay.CurrentStackLocation->DeviceObject : nullptr;
            NTSTATUS r = routine(upper, irp, ctx);
            if (r == STATUS_MORE_PROCESSING_REQUIRED) return;   // the driver owns the IRP again
        } else if (irp->PendingReturned && has_upper) {
            irp->Tail.Overlay.CurrentStackLocation->Control |= SL_PENDING_RETURNED;
        }
        if (!has_upper) break;
    }
    // Every location completed: final completion by origin.
    switch (Hdr(irp)->kind) {
    case IrpKind::TopLevel:
        TopFinalComplete(irp, Hdr(irp)->top);
        break;
    case IrpKind::IoMgrOwned: {
        if ((irp->Flags & IRP_BUFFERED_IO) && (irp->Flags & IRP_INPUT_OPERATION) && irp->AssociatedIrp.SystemBuffer &&
            irp->UserBuffer && !NT_ERROR(irp->IoStatus.Status)) {
            ULONG_PTR n = irp->IoStatus.Information;
            if (n > Hdr(irp)->user_out_len) n = Hdr(irp)->user_out_len;
            std::memcpy(irp->UserBuffer, irp->AssociatedIrp.SystemBuffer, static_cast<size_t>(n));
        }
        PIO_STATUS_BLOCK iosb = irp->UserIosb;
        PKEVENT ev = irp->UserEvent;
        if (iosb) { iosb->Status = irp->IoStatus.Status; iosb->Information = irp->IoStatus.Information; }
        IrpRelease(irp);
        if (ev) ntk::KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
        break;
    }
    case IrpKind::DriverOwned:
        break;
    }
}

}  // namespace win10xbox::drivers::detail

// ---------------------------------------------------------------------------------------------------
namespace ntk {
using namespace win10xbox::drivers::detail;

namespace {
SRWLOCK g_attach_lock = SRWLOCK_INIT;
std::unordered_map<PDEVICE_OBJECT, PDEVICE_OBJECT>& LowerMap() { static std::unordered_map<PDEVICE_OBJECT, PDEVICE_OBJECT> m; return m; }
KSPIN_LOCK g_cancel_lock = 0;

struct DevObjExt {
    CSHORT Type;
    CSHORT Size;
    PDEVICE_OBJECT DeviceObject;
    ULONG PowerFlags;
    PVOID Dope;
    ULONG ExtensionFlags;
    PVOID DeviceNode;
    PDEVICE_OBJECT AttachedTo;
    LONG StartIoCount;
    LONG StartIoKey;
    ULONG StartIoFlags;
    PVOID Vpb;
};
}  // namespace

NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT drv, ULONG ext_size, PUNICODE_STRING name, ULONG type, ULONG chars,
                              BOOLEAN exclusive, PDEVICE_OBJECT* out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (!drv) return STATUS_INVALID_PARAMETER;
    if (CurrentIrql() != PASSIVE_LEVEL) IrqlViolation("IoCreateDevice above PASSIVE_LEVEL", CurrentIrql(), PASSIVE_LEVEL);
    const bool named = name && name->Length && name->Buffer;
    std::wstring wname;
    if (named) {
        wname = ToWide(*name);
        if (wname[0] != L'\\') return STATUS_OBJECT_NAME_INVALID;
    }
    const SIZE_T head = (sizeof(DEVICE_OBJECT) + 15) & ~SIZE_T{15};
    auto* dev = static_cast<PDEVICE_OBJECT>(PoolAlloc(head + ext_size, kTagDev));
    auto* ext = static_cast<DevObjExt*>(PoolAlloc(sizeof(DevObjExt), kTagDev));
    if (!dev || !ext) {
        PoolFree(dev, 0, false);
        PoolFree(ext, 0, false);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ext->Type = IO_TYPE_DEVICE_OBJECT_EXT;
    ext->Size = sizeof(DevObjExt);
    ext->DeviceObject = dev;
    dev->Type = DEVICE_OBJECT_TYPE_ID;
    dev->Size = static_cast<USHORT>(head + ext_size);
    dev->ReferenceCount = 0;
    dev->DriverObject = drv;
    dev->Flags = DO_DEVICE_INITIALIZING | (exclusive ? DO_EXCLUSIVE : 0) | (named ? DO_DEVICE_HAS_NAME : 0);
    dev->Characteristics = chars;
    dev->DeviceExtension = ext_size ? reinterpret_cast<char*>(dev) + head : nullptr;
    dev->DeviceType = type;
    dev->StackSize = 1;
    dev->AlignmentRequirement = 0;
    dev->DeviceObjectExtension = ext;
    dev->DeviceQueue.Type = 0x0B;      // DeviceQueueObject
    dev->DeviceQueue.Size = sizeof(KDEVICE_QUEUE);
    InitializeListHead(&dev->DeviceQueue.DeviceListHead);
    KeInitializeEvent(&dev->DeviceLock, SynchronizationEvent, TRUE);
    if (named) {
        NTSTATUS st = NsAddDevice(dev, wname);
        if (!NT_SUCCESS(st)) {
            PoolFree(ext, 0, false);
            PoolFree(dev, 0, false);
            return st;
        }
    }
    dev->NextDevice = drv->DeviceObject;
    drv->DeviceObject = dev;
    ObjRegister(dev, ObjType::Device, [](void* p) { DeviceFree(static_cast<PDEVICE_OBJECT>(p)); }, 1);
    *out = dev;
    TRACE(driver, "IoCreateDevice %ls type=%#lx chars=%#lx ext=%lu -> %p", wname.c_str(), type, chars, ext_size,
          static_cast<void*>(dev));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI IoCreateDeviceSecure(PDRIVER_OBJECT drv, ULONG ext_size, PUNICODE_STRING name, ULONG type, ULONG chars,
                                    BOOLEAN exclusive, PCUNICODE_STRING sddl, const GUID* class_guid,
                                    PDEVICE_OBJECT* out) {
    (void)class_guid;
    if (sddl) TRACE(driver, "IoCreateDeviceSecure: SDDL %ls ignored (no access checks in the host)", ToWide(*sddl).c_str());
    return IoCreateDevice(drv, ext_size, name, type, chars, exclusive, out);
}

VOID NTAPI IoDeleteDevice(PDEVICE_OBJECT dev) {
    ObjType t;
    if (!dev || !ObjLookup(dev, &t) || t != ObjType::Device) {
        ERR(driver, "IoDeleteDevice(%p): not a device object", static_cast<void*>(dev));
        return;
    }
    TRACE(driver, "IoDeleteDevice %ls (%p)", NsDeviceName(dev).c_str(), static_cast<void*>(dev));
    NsRemoveDevice(dev);
    PDRIVER_OBJECT drv = dev->DriverObject;
    if (drv) {
        PDEVICE_OBJECT* pp = &drv->DeviceObject;
        while (*pp && *pp != dev) pp = &(*pp)->NextDevice;
        if (*pp == dev) *pp = dev->NextDevice;
    }
    AcquireSRWLockExclusive(&g_attach_lock);
    LowerMap().erase(dev);
    ReleaseSRWLockExclusive(&g_attach_lock);
    ObjDeref(dev);   // the creation reference; opened FILE_OBJECTs keep the device alive
}

NTSTATUS NTAPI IoCreateSymbolicLink(PUNICODE_STRING link, PUNICODE_STRING target) {
    if (!link || !target) return STATUS_INVALID_PARAMETER;
    return NsAddLink(View(*link), View(*target));
}
NTSTATUS NTAPI IoCreateUnprotectedSymbolicLink(PUNICODE_STRING link, PUNICODE_STRING target) { return IoCreateSymbolicLink(link, target); }
NTSTATUS NTAPI IoDeleteSymbolicLink(PUNICODE_STRING link) {
    if (!link) return STATUS_INVALID_PARAMETER;
    return NsRemoveLink(View(*link));
}

PDEVICE_OBJECT NTAPI IoAttachDeviceToDeviceStack(PDEVICE_OBJECT source, PDEVICE_OBJECT target) {
    if (!source || !target) return nullptr;
    PDEVICE_OBJECT top = AttachedDevice(target);
    top->AttachedDevice = source;
    source->StackSize = static_cast<CCHAR>(top->StackSize + 1);
    if (source->AlignmentRequirement < top->AlignmentRequirement) source->AlignmentRequirement = top->AlignmentRequirement;
    AcquireSRWLockExclusive(&g_attach_lock);
    LowerMap()[source] = top;
    ReleaseSRWLockExclusive(&g_attach_lock);
    ObjRef(top);
    TRACE(driver, "IoAttachDeviceToDeviceStack %p on %p (stack size %d)", static_cast<void*>(source), static_cast<void*>(top),
          source->StackSize);
    return top;
}
NTSTATUS NTAPI IoAttachDeviceToDeviceStackSafe(PDEVICE_OBJECT source, PDEVICE_OBJECT target, PDEVICE_OBJECT* attached_to) {
    PDEVICE_OBJECT r = IoAttachDeviceToDeviceStack(source, target);
    if (attached_to) *attached_to = r;
    return r ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}
NTSTATUS NTAPI IoAttachDevice(PDEVICE_OBJECT source, PUNICODE_STRING target_name, PDEVICE_OBJECT* attached_to) {
    PDEVICE_OBJECT t = NsResolve(View(*target_name), nullptr, nullptr);
    if (!t) return STATUS_OBJECT_NAME_NOT_FOUND;
    return IoAttachDeviceToDeviceStackSafe(source, t, attached_to);
}
VOID NTAPI IoDetachDevice(PDEVICE_OBJECT target) {
    if (!target) return;
    PDEVICE_OBJECT above = target->AttachedDevice;
    target->AttachedDevice = nullptr;
    if (above) {
        AcquireSRWLockExclusive(&g_attach_lock);
        LowerMap().erase(above);
        ReleaseSRWLockExclusive(&g_attach_lock);
        ObjDeref(target);
    }
}
PDEVICE_OBJECT NTAPI IoGetAttachedDevice(PDEVICE_OBJECT dev) { return AttachedDevice(dev); }
PDEVICE_OBJECT NTAPI IoGetAttachedDeviceReference(PDEVICE_OBJECT dev) {
    PDEVICE_OBJECT top = AttachedDevice(dev);
    ObjRef(top);
    return top;
}
PDEVICE_OBJECT NTAPI IoGetLowerDeviceObject(PDEVICE_OBJECT dev) {
    AcquireSRWLockShared(&g_attach_lock);
    auto it = LowerMap().find(dev);
    PDEVICE_OBJECT r = it == LowerMap().end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_attach_lock);
    if (r) ObjRef(r);
    return r;
}
PDEVICE_OBJECT NTAPI IoGetDeviceAttachmentBaseRef(PDEVICE_OBJECT dev) {
    PDEVICE_OBJECT cur = dev;
    for (;;) {
        AcquireSRWLockShared(&g_attach_lock);
        auto it = LowerMap().find(cur);
        PDEVICE_OBJECT lower = it == LowerMap().end() ? nullptr : it->second;
        ReleaseSRWLockShared(&g_attach_lock);
        if (!lower) break;
        cur = lower;
    }
    ObjRef(cur);
    return cur;
}

// ---- IRPs ----
PIRP NTAPI IoAllocateIrp(CCHAR stack_size, BOOLEAN charge_quota) {
    (void)charge_quota;
    return IrpAllocate(stack_size, IrpKind::DriverOwned);
}
VOID NTAPI IoInitializeIrp(PIRP irp, USHORT packet_size, CCHAR stack_size) { IrpInit(irp, packet_size, stack_size); }
VOID NTAPI IoFreeIrp(PIRP irp) {
    if (!irp) return;
    if (IrpKindOf(irp) != IrpKind::DriverOwned)
        ERR(driver, "IoFreeIrp(%p): IRP is owned by the I/O manager", static_cast<void*>(irp));
    IrpRelease(irp);
}
VOID NTAPI IoReuseIrp(PIRP irp, NTSTATUS status) {
    IrpInit(irp, irp->Size, irp->StackCount);
    irp->IoStatus.Status = status;
}
NTSTATUS NTAPI IofCallDriver(PDEVICE_OBJECT dev, PIRP irp) { return CallDriver(dev, irp); }
VOID NTAPI IofCompleteRequest(PIRP irp, CCHAR boost) { CompleteRequest(irp, boost); }
VOID NTAPI IoMarkIrpPendingFn(PIRP irp) { IoGetCurrentIrpStackLocation(irp)->Control |= SL_PENDING_RETURNED; }
PIO_STACK_LOCATION NTAPI IoGetCurrentIrpStackLocationFn(PIRP irp) { return IoGetCurrentIrpStackLocation(irp); }
VOID NTAPI IoSetCompletionRoutineEx(PDEVICE_OBJECT dev, PIRP irp, PIO_COMPLETION_ROUTINE routine, PVOID ctx,
                                    BOOLEAN success, BOOLEAN error, BOOLEAN cancel) {
    (void)dev;
    IoSetCompletionRoutine(irp, routine, ctx, success, error, cancel);
}


// ---- IRP builders ----
namespace {
void SetupBuffers(PIRP irp, ULONG method_or_flags_dummy, PDEVICE_OBJECT dev, PVOID user, ULONG len, bool is_read, bool use_dev_flags) {
    (void)method_or_flags_dummy;
    if (!len) return;
    if (use_dev_flags && (dev->Flags & DO_BUFFERED_IO)) {
        void* buf = PoolAlloc(len, kTagBuf);
        irp->AssociatedIrp.SystemBuffer = buf;
        if (!is_read && buf) std::memcpy(buf, user, len);
        irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | (is_read ? IRP_INPUT_OPERATION : 0);
        irp->UserBuffer = user;
    } else if (use_dev_flags && (dev->Flags & DO_DIRECT_IO)) {
        PMDL m = MdlCreate(user, len);
        if (m) {
            m->MdlFlags |= MDL_PAGES_LOCKED | MDL_MAPPED_TO_SYSTEM_VA | (is_read ? MDL_WRITE_OPERATION : 0);
            m->MappedSystemVa = reinterpret_cast<char*>(m->StartVa) + m->ByteOffset;
        }
        irp->MdlAddress = m;
    } else {
        irp->UserBuffer = user;
    }
}

PIRP BuildFsd(ULONG major, PDEVICE_OBJECT dev, PVOID buffer, ULONG length, PLARGE_INTEGER offset, PKEVENT event,
              PIO_STATUS_BLOCK iosb, bool sync) {
    if (!dev) return nullptr;
    PIRP irp = IrpAllocate(dev->StackSize, IrpKind::IoMgrOwned);
    if (!irp) return nullptr;
    irp->UserIosb = iosb;
    irp->UserEvent = event;
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = reinterpret_cast<PETHREAD>(CurrentPseudoThread());
    if (sync) irp->Flags |= IRP_SYNCHRONOUS_API;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = static_cast<UCHAR>(major);
    if (major == IRP_MJ_READ || major == IRP_MJ_WRITE) {
        s->Parameters.Read.Length = length;
        s->Parameters.Read.ByteOffset.QuadPart = offset ? offset->QuadPart : 0;
        SetupBuffers(irp, 0, dev, buffer, length, major == IRP_MJ_READ, true);
        irp->Flags |= (major == IRP_MJ_READ ? IRP_READ_OPERATION : IRP_WRITE_OPERATION);
        IrpSetUserOutLen(irp, major == IRP_MJ_READ ? length : 0);
    }
    return irp;
}
}  // namespace

PIRP NTAPI IoBuildDeviceIoControlRequest(ULONG code, PDEVICE_OBJECT dev, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len, BOOLEAN internal, PKEVENT event, PIO_STATUS_BLOCK iosb) {
    if (!dev) return nullptr;
    PIRP irp = IrpAllocate(dev->StackSize, IrpKind::IoMgrOwned);
    if (!irp) return nullptr;
    irp->UserIosb = iosb;
    irp->UserEvent = event;
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = reinterpret_cast<PETHREAD>(CurrentPseudoThread());
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = internal ? IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.OutputBufferLength = out_len;
    s->Parameters.DeviceIoControl.InputBufferLength = in_len;
    s->Parameters.DeviceIoControl.IoControlCode = code;
    switch (code & 3) {
    case METHOD_BUFFERED: {
        const ULONG n = in_len > out_len ? in_len : out_len;
        if (n) {
            void* buf = PoolAlloc(n, kTagBuf);
            irp->AssociatedIrp.SystemBuffer = buf;
            if (in_len && buf) std::memcpy(buf, in, in_len);
            irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
            if (out_len) { irp->Flags |= IRP_INPUT_OPERATION; irp->UserBuffer = out; }
        }
        break;
    }
    case METHOD_IN_DIRECT:
    case METHOD_OUT_DIRECT:
        if (in_len) {
            void* buf = PoolAlloc(in_len, kTagBuf);
            irp->AssociatedIrp.SystemBuffer = buf;
            if (buf) std::memcpy(buf, in, in_len);
            irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
        }
        if (out_len) {
            PMDL m = MdlCreate(out, out_len);
            if (m) {
                m->MdlFlags |= MDL_PAGES_LOCKED | MDL_MAPPED_TO_SYSTEM_VA | ((code & 3) == METHOD_OUT_DIRECT ? MDL_WRITE_OPERATION : 0);
                m->MappedSystemVa = reinterpret_cast<char*>(m->StartVa) + m->ByteOffset;
            }
            irp->MdlAddress = m;
        }
        break;
    default:   // METHOD_NEITHER
        s->Parameters.DeviceIoControl.Type3InputBuffer = in;
        irp->UserBuffer = out;
        break;
    }
    IrpSetUserOutLen(irp, ((code & 3) == METHOD_BUFFERED) ? out_len : 0);
    return irp;
}
PIRP NTAPI IoBuildSynchronousFsdRequest(ULONG major, PDEVICE_OBJECT dev, PVOID buffer, ULONG length,
                                        PLARGE_INTEGER offset, PKEVENT event, PIO_STATUS_BLOCK iosb) {
    return BuildFsd(major, dev, buffer, length, offset, event, iosb, true);
}
PIRP NTAPI IoBuildAsynchronousFsdRequest(ULONG major, PDEVICE_OBJECT dev, PVOID buffer, ULONG length,
                                         PLARGE_INTEGER offset, PIO_STATUS_BLOCK iosb) {
    return BuildFsd(major, dev, buffer, length, offset, nullptr, iosb, false);
}

// ---- cancellation ----
VOID NTAPI IoAcquireCancelSpinLock(PKIRQL old) { *old = SpinAcquireRaise(&g_cancel_lock, DISPATCH_LEVEL); }
VOID NTAPI IoReleaseCancelSpinLock(KIRQL old) { SpinReleaseLower(&g_cancel_lock, old); }
BOOLEAN NTAPI IoCancelIrp(PIRP irp) {
    irp->Cancel = TRUE;
    KIRQL old;
    IoAcquireCancelSpinLock(&old);
    irp->CancelIrql = old;
    auto routine = reinterpret_cast<PDRIVER_CANCEL>(
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&irp->CancelRoutine), nullptr));
    if (!routine) {
        IoReleaseCancelSpinLock(old);
        return FALSE;
    }
    PIO_STACK_LOCATION s = irp->Tail.Overlay.CurrentStackLocation;
    routine(s->DeviceObject, irp);   // releases the cancel spin lock itself
    return TRUE;
}

// ---- device queues / StartIo ----
VOID NTAPI KeInitializeDeviceQueue(PKDEVICE_QUEUE q) {
    q->Type = 0x0B;
    q->Size = sizeof(KDEVICE_QUEUE);
    InitializeListHead(&q->DeviceListHead);
    q->Lock = 0;
    q->Busy = FALSE;
}
BOOLEAN NTAPI KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE q, PKDEVICE_QUEUE_ENTRY e, ULONG key) {
    KIRQL old = SpinAcquireRaise(&q->Lock, DISPATCH_LEVEL);
    e->SortKey = key;
    bool was_busy = q->Busy != 0;
    if (was_busy) {
        PLIST_ENTRY pos = q->DeviceListHead.Flink;
        while (pos != &q->DeviceListHead &&
               CONTAINING_RECORD(pos, KDEVICE_QUEUE_ENTRY, DeviceListEntry)->SortKey <= key)
            pos = pos->Flink;
        InsertTailList(pos, &e->DeviceListEntry);   // before `pos`
        e->Inserted = TRUE;
    } else {
        q->Busy = TRUE;
        e->Inserted = FALSE;
    }
    SpinReleaseLower(&q->Lock, old);
    return was_busy;
}
BOOLEAN NTAPI KeInsertDeviceQueue(PKDEVICE_QUEUE q, PKDEVICE_QUEUE_ENTRY e) {
    KIRQL old = SpinAcquireRaise(&q->Lock, DISPATCH_LEVEL);
    bool was_busy = q->Busy != 0;
    if (was_busy) {
        InsertTailList(&q->DeviceListHead, &e->DeviceListEntry);
        e->Inserted = TRUE;
    } else {
        q->Busy = TRUE;
        e->Inserted = FALSE;
    }
    SpinReleaseLower(&q->Lock, old);
    return was_busy;
}
PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveDeviceQueue(PKDEVICE_QUEUE q) {
    KIRQL old = SpinAcquireRaise(&q->Lock, DISPATCH_LEVEL);
    PKDEVICE_QUEUE_ENTRY r = nullptr;
    if (IsListEmpty(&q->DeviceListHead)) {
        q->Busy = FALSE;
    } else {
        r = CONTAINING_RECORD(RemoveHeadList(&q->DeviceListHead), KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        r->Inserted = FALSE;
    }
    SpinReleaseLower(&q->Lock, old);
    return r;
}
PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE q, ULONG key) {
    KIRQL old = SpinAcquireRaise(&q->Lock, DISPATCH_LEVEL);
    PKDEVICE_QUEUE_ENTRY r = nullptr;
    if (IsListEmpty(&q->DeviceListHead)) {
        q->Busy = FALSE;
    } else {
        PLIST_ENTRY pos = q->DeviceListHead.Flink;
        while (pos != &q->DeviceListHead &&
               CONTAINING_RECORD(pos, KDEVICE_QUEUE_ENTRY, DeviceListEntry)->SortKey < key)
            pos = pos->Flink;
        if (pos == &q->DeviceListHead) pos = q->DeviceListHead.Flink;
        RemoveEntryList(pos);
        r = CONTAINING_RECORD(pos, KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        r->Inserted = FALSE;
    }
    SpinReleaseLower(&q->Lock, old);
    return r;
}
BOOLEAN NTAPI KeRemoveEntryDeviceQueue(PKDEVICE_QUEUE q, PKDEVICE_QUEUE_ENTRY e) {
    KIRQL old = SpinAcquireRaise(&q->Lock, DISPATCH_LEVEL);
    bool was = e->Inserted != 0;
    if (was) { RemoveEntryList(&e->DeviceListEntry); e->Inserted = FALSE; }
    SpinReleaseLower(&q->Lock, old);
    return was;
}

namespace {
void StartIoCall(PDEVICE_OBJECT dev, PIRP irp) {
    PDRIVER_STARTIO fn = dev->DriverObject ? dev->DriverObject->DriverStartIo : nullptr;
    if (!fn) { ERR(driver, "IoStartPacket: driver has no StartIo routine"); return; }
    fn(dev, irp);
}
}  // namespace

VOID NTAPI IoStartPacket(PDEVICE_OBJECT dev, PIRP irp, PULONG key, PDRIVER_CANCEL cancel) {
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    if (cancel) {
        KIRQL ci;
        IoAcquireCancelSpinLock(&ci);
        irp->CancelRoutine = cancel;
        if (irp->Cancel && InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&irp->CancelRoutine), nullptr)) {
            irp->CancelIrql = ci;
            cancel(dev, irp);
            KfLowerIrql(old);
            return;
        }
        IoReleaseCancelSpinLock(ci);
    }
    bool busy = key ? KeInsertByKeyDeviceQueue(&dev->DeviceQueue, &irp->Tail.Overlay.DeviceQueueEntry, *key)
                    : KeInsertDeviceQueue(&dev->DeviceQueue, &irp->Tail.Overlay.DeviceQueueEntry);
    if (!busy) {
        dev->CurrentIrp = irp;
        if (cancel) {
            KIRQL ci;
            IoAcquireCancelSpinLock(&ci);
            irp->CancelRoutine = nullptr;
            IoReleaseCancelSpinLock(ci);
        }
        StartIoCall(dev, irp);
    }
    KfLowerIrql(old);
}
VOID NTAPI IoStartNextPacket(PDEVICE_OBJECT dev, BOOLEAN cancelable) {
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    PKDEVICE_QUEUE_ENTRY e = KeRemoveDeviceQueue(&dev->DeviceQueue);
    if (e) {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry);
        dev->CurrentIrp = irp;
        if (cancelable) {
            KIRQL ci;
            IoAcquireCancelSpinLock(&ci);
            irp->CancelRoutine = nullptr;
            IoReleaseCancelSpinLock(ci);
        }
        StartIoCall(dev, irp);
    } else {
        dev->CurrentIrp = nullptr;
    }
    KfLowerIrql(old);
}
VOID NTAPI IoStartNextPacketByKey(PDEVICE_OBJECT dev, BOOLEAN cancelable, ULONG key) {
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    PKDEVICE_QUEUE_ENTRY e = KeRemoveByKeyDeviceQueue(&dev->DeviceQueue, key);
    if (e) {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry);
        dev->CurrentIrp = irp;
        if (cancelable) {
            KIRQL ci;
            IoAcquireCancelSpinLock(&ci);
            irp->CancelRoutine = nullptr;
            IoReleaseCancelSpinLock(ci);
        }
        StartIoCall(dev, irp);
    } else {
        dev->CurrentIrp = nullptr;
    }
    KfLowerIrql(old);
}

// ---- DPC / timer requests per device ----
VOID NTAPI IoRequestDpc(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx) { KeInsertQueueDpc(&dev->Dpc, irp, ctx); }
VOID NTAPI IoInitializeDpcRequest(PDEVICE_OBJECT dev, PKDEFERRED_ROUTINE routine) { KeInitializeDpc(&dev->Dpc, routine, dev); }

namespace {
typedef VOID(NTAPI* PIO_TIMER_ROUTINE_T)(PDEVICE_OBJECT, PVOID);
struct IoTimerRec {
    KTIMER timer;
    KDPC dpc;
    PIO_TIMER_ROUTINE_T routine;
    PVOID ctx;
    PDEVICE_OBJECT dev;
};
VOID NTAPI IoTimerDpc(PKDPC, PVOID context, PVOID, PVOID) {
    auto* r = static_cast<IoTimerRec*>(context);
    r->routine(r->dev, r->ctx);
}
}  // namespace
NTSTATUS NTAPI IoInitializeTimer(PDEVICE_OBJECT dev, PIO_TIMER_ROUTINE_T routine, PVOID ctx) {
    auto* r = static_cast<IoTimerRec*>(PoolAlloc(sizeof(IoTimerRec), kTagMisc));
    if (!r) return STATUS_INSUFFICIENT_RESOURCES;
    r->routine = routine;
    r->ctx = ctx;
    r->dev = dev;
    KeInitializeTimerEx(&r->timer, SynchronizationTimer);
    KeInitializeDpc(&r->dpc, &IoTimerDpc, r);
    dev->Timer = r;
    return STATUS_SUCCESS;
}
VOID NTAPI IoStartTimer(PDEVICE_OBJECT dev) {
    auto* r = static_cast<IoTimerRec*>(dev->Timer);
    if (!r) return;
    LARGE_INTEGER due;
    due.QuadPart = -10000000LL;
    KeSetTimerEx(&r->timer, due, 1000, &r->dpc);
}
VOID NTAPI IoStopTimer(PDEVICE_OBJECT dev) {
    auto* r = static_cast<IoTimerRec*>(dev->Timer);
    if (r) KeCancelTimer(&r->timer);
}

// ---- MDLs ----
PMDL NTAPI IoAllocateMdl(PVOID va, ULONG length, BOOLEAN secondary, BOOLEAN charge_quota, PIRP irp) {
    (void)charge_quota;
    PMDL m = MdlCreate(va, length);
    if (m && irp) {
        if (!secondary) {
            irp->MdlAddress = m;
        } else {
            PMDL* pp = &irp->MdlAddress;
            while (*pp) pp = &(*pp)->Next;
            *pp = m;
        }
    }
    return m;
}
VOID NTAPI IoFreeMdl(PMDL m) { MdlFree(m); }
VOID NTAPI MmProbeAndLockPages(PMDL m, KPROCESSOR_MODE mode, LOCK_OPERATION op) {
    (void)mode;
    if (!ProbeRange(reinterpret_cast<char*>(m->StartVa) + m->ByteOffset, m->ByteCount, op != IoReadAccess)) {
        RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, nullptr);
        return;
    }
    m->MdlFlags |= MDL_PAGES_LOCKED | (op != IoReadAccess ? MDL_WRITE_OPERATION : 0);
    FillPfns(m);
}
VOID NTAPI MmUnlockPages(PMDL m) { m->MdlFlags &= static_cast<CSHORT>(~MDL_PAGES_LOCKED); }
VOID NTAPI MmBuildMdlForNonPagedPool(PMDL m) {
    m->MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL;
    m->MappedSystemVa = reinterpret_cast<char*>(m->StartVa) + m->ByteOffset;
    FillPfns(m);
}
PVOID NTAPI MmMapLockedPagesSpecifyCache(PMDL m, KPROCESSOR_MODE mode, MEMORY_CACHING_TYPE cache, PVOID requested,
                                         ULONG bugcheck, ULONG priority) {
    (void)mode; (void)cache; (void)requested; (void)bugcheck; (void)priority;
    if (m->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) return m->MappedSystemVa;
    m->MappedSystemVa = reinterpret_cast<char*>(m->StartVa) + m->ByteOffset;   // identity "mapping"
    m->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
    return m->MappedSystemVa;
}
PVOID NTAPI MmMapLockedPages(PMDL m, KPROCESSOR_MODE mode) {
    return MmMapLockedPagesSpecifyCache(m, mode, MmCached, nullptr, TRUE, NormalPagePriority);
}
VOID NTAPI MmUnmapLockedPages(PVOID base, PMDL m) {
    (void)base;
    if (!(m->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL)) m->MdlFlags &= static_cast<CSHORT>(~MDL_MAPPED_TO_SYSTEM_VA);
}

// ---- misc ----
VOID NTAPI IoGetStackLimits(PULONG_PTR low, PULONG_PTR high) {
    *low = __readgsqword(0x10);
    *high = __readgsqword(0x08);
}
ULONG_PTR NTAPI IoGetRemainingStackSize(void) {
    ULONG_PTR low = __readgsqword(0x10);
    volatile char probe;
    return reinterpret_cast<ULONG_PTR>(&probe) - low;
}
BOOLEAN NTAPI IoIsWdmVersionAvailable(UCHAR major, UCHAR minor) { return major < 1 || (major == 1 && minor <= 0x30); }
NTSTATUS NTAPI IoCreateFile(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb,
                            PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts, PVOID ea,
                            ULONG ea_len, int create_file_type, PVOID internal, ULONG options) {
    (void)create_file_type; (void)internal; (void)options;
    using Fn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                                ULONG, ULONG, PVOID, ULONG);
    auto fn = reinterpret_cast<Fn>(ResolveNtFunction("NtCreateFile"));
    if (!fn) return STATUS_NOT_IMPLEMENTED;
    return fn(h, access, oa, iosb, alloc, attrs, share, disp, opts, ea, ea_len);
}

namespace {
std::vector<PDEVICE_OBJECT> g_shutdown_devs;
SRWLOCK g_shutdown_lock = SRWLOCK_INIT;
}  // namespace
NTSTATUS NTAPI IoRegisterShutdownNotification(PDEVICE_OBJECT dev) {
    AcquireSRWLockExclusive(&g_shutdown_lock);
    g_shutdown_devs.push_back(dev);
    dev->Flags |= DO_SHUTDOWN_REGISTERED;
    ReleaseSRWLockExclusive(&g_shutdown_lock);
    return STATUS_SUCCESS;
}
VOID NTAPI IoUnregisterShutdownNotification(PDEVICE_OBJECT dev) {
    AcquireSRWLockExclusive(&g_shutdown_lock);
    for (auto it = g_shutdown_devs.begin(); it != g_shutdown_devs.end(); ++it)
        if (*it == dev) { g_shutdown_devs.erase(it); break; }
    dev->Flags &= ~static_cast<ULONG>(DO_SHUTDOWN_REGISTERED);
    ReleaseSRWLockExclusive(&g_shutdown_lock);
}

// ---- power (IRPs pass through) ----
NTSTATUS NTAPI PoCallDriver(PDEVICE_OBJECT dev, PIRP irp) { return CallDriver(dev, irp); }
VOID NTAPI PoStartNextPowerIrp(PIRP irp) { (void)irp; }
ULONG NTAPI PoSetPowerState(PDEVICE_OBJECT dev, int type, ULONG state) { (void)dev; (void)type; return state; }

}  // namespace ntk

namespace win10xbox::drivers::detail {
std::vector<PDEVICE_OBJECT> ShutdownDevices() {
    using namespace ntk;
    AcquireSRWLockShared(&g_shutdown_lock);
    auto v = g_shutdown_devs;
    ReleaseSRWLockShared(&g_shutdown_lock);
    return v;
}
namespace {
const Export kIoTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
    X(IoCreateDevice), X(IoCreateDeviceSecure), X(IoDeleteDevice), X(IoCreateSymbolicLink),
    X(IoCreateUnprotectedSymbolicLink), X(IoDeleteSymbolicLink), X(IoAttachDeviceToDeviceStack),
    X(IoAttachDeviceToDeviceStackSafe), X(IoAttachDevice), X(IoDetachDevice), X(IoGetAttachedDevice),
    X(IoGetAttachedDeviceReference), X(IoGetLowerDeviceObject), X(IoGetDeviceAttachmentBaseRef),
    X(IoAllocateIrp), X(IoInitializeIrp), X(IoFreeIrp), X(IoReuseIrp), X(IofCallDriver), X(IofCompleteRequest),
    X(IoSetCompletionRoutineEx),
    X(IoBuildDeviceIoControlRequest), X(IoBuildSynchronousFsdRequest), X(IoBuildAsynchronousFsdRequest),
    X(IoAcquireCancelSpinLock), X(IoReleaseCancelSpinLock), X(IoCancelIrp),
    X(KeInitializeDeviceQueue), X(KeInsertByKeyDeviceQueue), X(KeInsertDeviceQueue), X(KeRemoveDeviceQueue),
    X(KeRemoveByKeyDeviceQueue), X(KeRemoveEntryDeviceQueue),
    X(IoStartPacket), X(IoStartNextPacket), X(IoStartNextPacketByKey), X(IoRequestDpc), X(IoInitializeDpcRequest),
    X(IoInitializeTimer), X(IoStartTimer), X(IoStopTimer),
    X(IoAllocateMdl), X(IoFreeMdl), X(MmProbeAndLockPages), X(MmUnlockPages), X(MmBuildMdlForNonPagedPool),
    X(MmMapLockedPagesSpecifyCache), X(MmMapLockedPages), X(MmUnmapLockedPages),
    X(IoGetStackLimits), X(IoGetRemainingStackSize), X(IoIsWdmVersionAvailable), X(IoCreateFile),
    X(IoRegisterShutdownNotification), X(IoUnregisterShutdownNotification), X(IoGetDeviceObjectPointer),
    X(IoCreateDriver), X(IoDeleteDriver),
    X(PoCallDriver), X(PoStartNextPowerIrp), X(PoSetPowerState),
#undef X
    // Functions that are macros/inlines in the WDK headers but also exported.
    {"IoCallDriver", reinterpret_cast<void*>(&ntk::IofCallDriver)},
    {"IoCompleteRequest", reinterpret_cast<void*>(&ntk::IofCompleteRequest)},
    {"IoMarkIrpPending", reinterpret_cast<void*>(&ntk::IoMarkIrpPendingFn)},
    {"IoGetCurrentIrpStackLocation", reinterpret_cast<void*>(&ntk::IoGetCurrentIrpStackLocationFn)},
};
}  // namespace
ExportGroup IoExports() { return {kIoTable, sizeof(kIoTable) / sizeof(kIoTable[0])}; }
}  // namespace win10xbox::drivers::detail
