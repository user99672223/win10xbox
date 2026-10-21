// The I/O manager side of the driver host: turns guest NtCreateFile /
// NtDeviceIoControlFile / NtReadFile / NtWriteFile / NtClose on driver
// devices into IRPs, sends them through IoCallDriver and copies the results
// back (the work of IopParseDevice, IopXxxControlFile, IopCompleteRequest).
#include "../ntoskrnl/internal.h"
#include "win10xbox/dispatch.h"
#include <atomic>
#include <unordered_map>

namespace ntk {
VOID NTAPI KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
}

namespace win10xbox::drivers::detail {

struct OpenFile {
    HANDLE handle = nullptr;
    PFILE_OBJECT fo = nullptr;
    PDEVICE_OBJECT dev = nullptr;     // device the name resolved to
    bool sync = false;
    ACCESS_MASK access = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<PIRP> pending;
    std::atomic<int> inflight{0};
};

struct TopContext {
    HANDLE done = nullptr;
    bool async = false;
    PVOID user_out = nullptr;
    ULONG user_out_len = 0;
    PIO_STATUS_BLOCK user_iosb = nullptr;
    HANDLE user_event = nullptr;
    HANDLE file_event = nullptr;
    PIO_APC_ROUTINE apc = nullptr;
    OpenFile* file = nullptr;
    PFILE_OBJECT fo = nullptr;
    std::atomic<bool> completed{false};
    // Security context for IRP_MJ_CREATE.
    IO_SECURITY_CONTEXT sec{};
    alignas(16) unsigned char access_state[0x100]{};
};

namespace {

namespace dsp = win10xbox::dispatch;

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<HANDLE, OpenFile*>& Handles() { static std::unordered_map<HANDLE, OpenFile*> m; return m; }
std::unordered_map<HANDLE, std::wstring>& LinkHandles() { static std::unordered_map<HANDLE, std::wstring> m; return m; }
std::atomic<int> g_open_count{0};
std::atomic<std::uint64_t> g_serial{1};

struct Guard {   // keeps an OpenFile alive across one operation
    OpenFile* of = nullptr;
    explicit Guard(HANDLE h) {
        AcquireSRWLockShared(&g_lock);
        auto it = Handles().find(h);
        if (it != Handles().end()) { of = it->second; ++of->inflight; }
        ReleaseSRWLockShared(&g_lock);
    }
    ~Guard() { if (of) --of->inflight; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

KPROCESSOR_MODE CallerMode() {
    if (InDriverCall()) return KernelMode;
    return win10xbox::process::IsGuestThread() ? UserMode : KernelMode;
}

void FileDestroy(void* p);

PFILE_OBJECT NewFileObject(PDEVICE_OBJECT dev, const std::wstring& remainder, ACCESS_MASK access, ULONG share,
                           ULONG options) {
    auto* fo = static_cast<PFILE_OBJECT>(PoolAlloc(sizeof(FILE_OBJECT), kTagFile));
    if (!fo) return nullptr;
    fo->Type = FILE_OBJECT_TYPE_ID;
    fo->Size = sizeof(FILE_OBJECT);
    fo->DeviceObject = dev;
    if (options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) fo->Flags |= FO_SYNCHRONOUS_IO;
    if (options & FILE_SYNCHRONOUS_IO_ALERT) fo->Flags |= FO_ALERTABLE_IO;
    fo->Flags |= FO_HANDLE_CREATED;
    fo->ReadAccess = (access & (FILE_READ_DATA | 0x20 /*FILE_EXECUTE*/)) != 0;
    fo->WriteAccess = (access & (FILE_WRITE_DATA | 0x4 /*FILE_APPEND_DATA*/)) != 0;
    fo->DeleteAccess = (access & 0x10000 /*DELETE*/) != 0;
    fo->SharedRead = (share & FILE_SHARE_READ) != 0;
    fo->SharedWrite = (share & FILE_SHARE_WRITE) != 0;
    fo->SharedDelete = (share & FILE_SHARE_DELETE) != 0;
    if (!remainder.empty()) {
        size_t bytes = remainder.size() * sizeof(wchar_t);
        auto* b = static_cast<wchar_t*>(PoolAlloc(bytes + sizeof(wchar_t), kTagStr));
        if (b) {
            std::memcpy(b, remainder.c_str(), bytes);
            fo->FileName.Buffer = b;
            fo->FileName.Length = static_cast<USHORT>(bytes);
            fo->FileName.MaximumLength = static_cast<USHORT>(bytes + sizeof(wchar_t));
        }
    }
    ntk::KeInitializeEvent(&fo->Lock, SynchronizationEvent, TRUE);
    ntk::KeInitializeEvent(&fo->Event, NotificationEvent, FALSE);
    InitializeListHead(&fo->IrpList);
    ObjRegister(fo, ObjType::File, FileDestroy, 1);
    ObjRef(dev);   // the file object holds its device
    return fo;
}

NTSTATUS DispatchIrp(PIRP irp, PDEVICE_OBJECT top, TopContext* t, bool async, ULONG_PTR* information) {
    t->async = async;
    if (!async) t->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (t->fo) ObjRef(t->fo);
    if (t->file) {
        AcquireSRWLockExclusive(&t->file->lock);
        t->file->pending.push_back(irp);
        ReleaseSRWLockExclusive(&t->file->lock);
    }
    KPROCESSOR_MODE prev = SwapPreviousMode(irp->RequestorMode);
    bool was_in = SwapInDriverCall(true);
    NTSTATUS st = CallDriver(top, irp);
    SwapInDriverCall(was_in);
    SwapPreviousMode(prev);
    if (async) return st;   // the IRP may already be gone
    if (st == STATUS_PENDING) {
        WaitForSingleObject(t->done, INFINITE);
        st = irp->IoStatus.Status;
    } else if (!t->completed.load()) {
        WARN(driver, "driver returned %08lx without completing IRP %p (major %u)", static_cast<unsigned long>(st),
             static_cast<void*>(irp), IoGetCurrentIrpStackLocation(irp)->MajorFunction);
    }
    if (information) *information = irp->IoStatus.Information;
    CloseHandle(t->done);
    PFILE_OBJECT fo = t->fo;
    IrpRelease(irp);
    delete t;
    if (fo) ObjDeref(fo);
    return st;
}

PIRP NewTopIrp(PDEVICE_OBJECT top, PFILE_OBJECT fo, UCHAR major, KPROCESSOR_MODE mode, ULONG flags, TopContext* t) {
    PIRP irp = IrpAllocate(top->StackSize, IrpKind::TopLevel);
    if (!irp) return nullptr;
    IrpSetTop(irp, t);
    t->fo = fo;
    irp->RequestorMode = mode;
    irp->Flags = flags;
    irp->Tail.Overlay.OriginalFileObject = fo;
    irp->Tail.Overlay.Thread = reinterpret_cast<PETHREAD>(CurrentPseudoThread());
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = major;
    s->FileObject = fo;
    return irp;
}

// A request without buffers (cleanup, close, flush, shutdown ...).
NTSTATUS SendSimple(PDEVICE_OBJECT top, PFILE_OBJECT fo, UCHAR major, KPROCESSOR_MODE mode, ULONG flags,
                    ULONG_PTR* information = nullptr) {
    auto* t = new TopContext;
    PIRP irp = NewTopIrp(top, fo, major, mode, flags | IRP_SYNCHRONOUS_API, t);
    if (!irp) { delete t; return STATUS_INSUFFICIENT_RESOURCES; }
    return DispatchIrp(irp, top, t, false, information);
}

void FileDestroy(void* p) {
    auto* fo = static_cast<PFILE_OBJECT>(p);
    PDEVICE_OBJECT dev = fo->DeviceObject;
    if (dev && !(fo->Flags & FO_CLEANUP_COMPLETE) && dev->DriverObject) {
        // Kernel-opened file object (IoGetDeviceObjectPointer): the last reference closes it.
        PDEVICE_OBJECT top = AttachedDevice(dev);
        fo->Flags |= FO_CLEANUP_COMPLETE;
        SendSimple(top, fo, IRP_MJ_CLEANUP, KernelMode, 0);
        SendSimple(top, fo, IRP_MJ_CLOSE, KernelMode, IRP_CLOSE_OPERATION);
    }
    PoolFree(fo->FileName.Buffer, 0, false);
    PoolFree(fo, 0, false);
    if (dev) ObjDeref(dev);
}

bool WritableIosb(PIO_STATUS_BLOCK io) { return !io || ProbeRange(io, sizeof(*io), true); }
void SetIosb(PIO_STATUS_BLOCK io, NTSTATUS st, ULONG_PTR info) {
    if (!io) return;
    io->Status = st;
    io->Information = info;
}

// ---- opening ------------------------------------------------------------------------------------------------

NTSTATUS OpenInternal(ACCESS_MASK access, const std::wstring& name, ULONG share, ULONG disposition, ULONG options,
                      ULONG file_attributes, KPROCESSOR_MODE mode, PHANDLE out_handle, PFILE_OBJECT* out_fo,
                      PIO_STATUS_BLOCK user_iosb) {
    std::wstring remainder;
    PDEVICE_OBJECT dev = NsResolve(name, &remainder, nullptr);
    if (!dev) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (dev->Flags & DO_DEVICE_INITIALIZING) return STATUS_DEVICE_NOT_READY;
    PDEVICE_OBJECT top = AttachedDevice(dev);
    if (dev->Flags & DO_EXCLUSIVE) {
        bool busy = false;
        AcquireSRWLockShared(&g_lock);
        for (auto& kv : Handles())
            if (kv.second->dev == dev) { busy = true; break; }
        ReleaseSRWLockShared(&g_lock);
        if (busy) return STATUS_ACCESS_DENIED;
    }
    PFILE_OBJECT fo = NewFileObject(dev, remainder, access, share, options);
    if (!fo) return STATUS_INSUFFICIENT_RESOURCES;

    auto* t = new TopContext;
    PIRP irp = NewTopIrp(top, fo, IRP_MJ_CREATE, mode, IRP_CREATE_OPERATION | IRP_SYNCHRONOUS_API, t);
    if (!irp) {
        delete t;
        fo->Flags |= FO_CLEANUP_COMPLETE;
        ObjDeref(fo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    t->sec.DesiredAccess = access;
    t->sec.AccessState = t->access_state;
    t->sec.FullCreateOptions = options;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->Parameters.Create.SecurityContext = &t->sec;
    s->Parameters.Create.Options = (disposition << 24) | (options & 0x00FFFFFF);
    s->Parameters.Create.FileAttributes = static_cast<USHORT>(file_attributes);
    s->Parameters.Create.ShareAccess = static_cast<USHORT>(share);
    s->Parameters.Create.EaLength = 0;
    ULONG_PTR info = 0;
    NTSTATUS st = DispatchIrp(irp, top, t, false, &info);
    if (!NT_SUCCESS(st)) {
        TRACE(driver, "open %ls failed: %08lx", name.c_str(), static_cast<unsigned long>(st));
        fo->Flags |= FO_CLEANUP_COMPLETE;   // a failed create is not followed by cleanup/close
        ObjDeref(fo);
        SetIosb(user_iosb, st, info);
        return st;
    }
    if (out_fo && !out_handle) {
        *out_fo = fo;           // caller owns the reference
        return st;
    }
    auto* of = new OpenFile;
    of->fo = fo;
    of->dev = dev;
    of->sync = (options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
    of->access = access;
    HANDLE h = dsp::RegisterEmulatedObject(dsp::ObjectKind::Device, g_serial++, of);
    if (!h) {
        delete of;
        SendSimple(top, fo, IRP_MJ_CLEANUP, KernelMode, 0);
        SendSimple(top, fo, IRP_MJ_CLOSE, KernelMode, IRP_CLOSE_OPERATION);
        fo->Flags |= FO_CLEANUP_COMPLETE;
        ObjDeref(fo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    of->handle = h;
    AcquireSRWLockExclusive(&g_lock);
    Handles()[h] = of;
    ReleaseSRWLockExclusive(&g_lock);
    ++g_open_count;
    *out_handle = h;
    SetIosb(user_iosb, st, info);
    TRACE(driver, "opened %ls -> handle %p (file object %p)", name.c_str(), h, static_cast<void*>(fo));
    return st;
}

// ---- closing --------------------------------------------------------------------------------------------------------

NTSTATUS CloseOpenFile(OpenFile* of) {
    // Already removed from the tables by the caller. Cancel first: a request blocked in the
    // driver would otherwise keep `inflight` up.
    std::vector<PIRP> pend;
    AcquireSRWLockShared(&of->lock);
    pend = of->pending;
    ReleaseSRWLockShared(&of->lock);
    for (PIRP irp : pend) ntk::IoCancelIrp(irp);
    for (int i = 0; i < 3000; ++i) {
        AcquireSRWLockShared(&of->lock);
        bool empty = of->pending.empty();
        ReleaseSRWLockShared(&of->lock);
        if (empty) break;
        Sleep(1);
    }
    for (int i = 0; of->inflight.load() > 0 && i < 5000; ++i) Sleep(1);
    PFILE_OBJECT fo = of->fo;
    PDEVICE_OBJECT top = AttachedDevice(of->dev);
    if (!(fo->Flags & FO_CLEANUP_COMPLETE)) {
        fo->Flags |= FO_CLEANUP_COMPLETE;
        SendSimple(top, fo, IRP_MJ_CLEANUP, CallerMode(), 0);
        SendSimple(top, fo, IRP_MJ_CLOSE, CallerMode(), IRP_CLOSE_OPERATION);
    }
    HANDLE h = of->handle;
    delete of;
    --g_open_count;
    ObjDeref(fo);
    dsp::UnregisterEmulatedObject(h);
    return STATUS_SUCCESS;
}

// ---- buffer setup shared by read/write/ioctl -----------------------------------------------------------------------------

PMDL MakeUserMdl(PVOID va, ULONG len, bool device_writes) {
    PMDL m = MdlCreate(va, len);
    if (!m) return nullptr;
    m->MdlFlags |= MDL_PAGES_LOCKED | MDL_MAPPED_TO_SYSTEM_VA | (device_writes ? MDL_WRITE_OPERATION : 0);
    m->MappedSystemVa = reinterpret_cast<char*>(m->StartVa) + m->ByteOffset;   // user VA doubles as system VA
    return m;
}

NTSTATUS PrepareAsyncOrSync(OpenFile* of, HANDLE event, PIO_APC_ROUTINE apc, PIO_STATUS_BLOCK iosb, TopContext* t,
                            bool* async) {
    *async = !of->sync;
    t->file = of;
    t->user_iosb = iosb;
    t->user_event = event;
    t->apc = apc;
    t->file_event = of->handle;
    if (apc) FIXME(driver, "APC completion routines on driver device handles are not delivered (use an event)");
    if (*async && !event) ResetEvent(of->handle);
    return STATUS_SUCCESS;
}

NTSTATUS FinishRequest(PIRP irp, PDEVICE_OBJECT top, TopContext* t, bool async) {
    ULONG_PTR info = 0;
    return DispatchIrp(irp, top, t, async, &info);
}

NTSTATUS DoControl(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_ctx, PIO_STATUS_BLOCK iosb, ULONG code,
                   PVOID in, ULONG in_len, PVOID out, ULONG out_len, bool fsctl) {
    (void)apc_ctx;
    Guard g(h);
    if (!g.of) return STATUS_INVALID_HANDLE;
    OpenFile* of = g.of;
    const KPROCESSOR_MODE mode = CallerMode();
    if (mode == UserMode) {
        if (!WritableIosb(iosb)) return STATUS_ACCESS_VIOLATION;
        if (in_len && !ProbeRange(in, in_len, false)) return STATUS_ACCESS_VIOLATION;
        if (out_len && !ProbeRange(out, out_len, true)) return STATUS_ACCESS_VIOLATION;
    }
    PDEVICE_OBJECT top = AttachedDevice(of->fo->DeviceObject);
    auto* t = new TopContext;
    PIRP irp = NewTopIrp(top, of->fo, fsctl ? IRP_MJ_FILE_SYSTEM_CONTROL : IRP_MJ_DEVICE_CONTROL, mode, 0, t);
    if (!irp) { delete t; return STATUS_INSUFFICIENT_RESOURCES; }
    bool async;
    PrepareAsyncOrSync(of, event, apc, iosb, t, &async);
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    if (fsctl) s->MinorFunction = IRP_MN_USER_FS_REQUEST;
    s->Parameters.DeviceIoControl.OutputBufferLength = out_len;
    s->Parameters.DeviceIoControl.InputBufferLength = in_len;
    s->Parameters.DeviceIoControl.IoControlCode = code;
    t->user_out = out;
    t->user_out_len = out_len;
    switch (code & 3) {
    case METHOD_BUFFERED: {
        const ULONG n = in_len > out_len ? in_len : out_len;
        if (n) {
            void* buf = PoolAlloc(n, kTagBuf);
            if (!buf) { IrpRelease(irp); delete t; return STATUS_INSUFFICIENT_RESOURCES; }
            irp->AssociatedIrp.SystemBuffer = buf;
            if (in_len) std::memcpy(buf, in, in_len);
            irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
            if (out_len) { irp->Flags |= IRP_INPUT_OPERATION; irp->UserBuffer = out; }
        }
        break;
    }
    case METHOD_IN_DIRECT:
    case METHOD_OUT_DIRECT:
        if (in_len) {
            void* buf = PoolAlloc(in_len, kTagBuf);
            if (!buf) { IrpRelease(irp); delete t; return STATUS_INSUFFICIENT_RESOURCES; }
            irp->AssociatedIrp.SystemBuffer = buf;
            std::memcpy(buf, in, in_len);
            irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
        }
        if (out_len) irp->MdlAddress = MakeUserMdl(out, out_len, (code & 3) == METHOD_OUT_DIRECT);
        break;
    default:   // METHOD_NEITHER
        s->Parameters.DeviceIoControl.Type3InputBuffer = in;
        irp->UserBuffer = out;
        break;
    }
    TRACE(driver, "%s %p code=%08lx in=%lu out=%lu%s", fsctl ? "FSCTL" : "IOCTL", h, static_cast<unsigned long>(code),
          in_len, out_len, async ? " (async)" : "");
    return FinishRequest(irp, top, t, async);
}

NTSTATUS DoReadWrite(bool write, HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, PIO_STATUS_BLOCK iosb, PVOID buffer,
                     ULONG length, PLARGE_INTEGER offset, PULONG key) {
    Guard g(h);
    if (!g.of) return STATUS_INVALID_HANDLE;
    OpenFile* of = g.of;
    const KPROCESSOR_MODE mode = CallerMode();
    if (mode == UserMode) {
        if (!WritableIosb(iosb)) return STATUS_ACCESS_VIOLATION;
        if (length && !ProbeRange(buffer, length, !write)) return STATUS_ACCESS_VIOLATION;
    }
    PDEVICE_OBJECT dev = of->fo->DeviceObject;
    PDEVICE_OBJECT top = AttachedDevice(dev);
    auto* t = new TopContext;
    PIRP irp = NewTopIrp(top, of->fo, write ? IRP_MJ_WRITE : IRP_MJ_READ, mode,
                         write ? IRP_WRITE_OPERATION : IRP_READ_OPERATION, t);
    if (!irp) { delete t; return STATUS_INSUFFICIENT_RESOURCES; }
    bool async;
    PrepareAsyncOrSync(of, event, apc, iosb, t, &async);
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->Parameters.Read.Length = length;
    s->Parameters.Read.Key = key ? *key : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = offset ? offset->QuadPart : of->fo->CurrentByteOffset.QuadPart;
    s->Parameters.Read.ByteOffset = pos;
    t->user_out = buffer;
    t->user_out_len = write ? 0 : length;
    if (length) {
        if (top->Flags & DO_BUFFERED_IO) {
            void* buf = PoolAlloc(length, kTagBuf);
            if (!buf) { IrpRelease(irp); delete t; return STATUS_INSUFFICIENT_RESOURCES; }
            irp->AssociatedIrp.SystemBuffer = buf;
            if (write) std::memcpy(buf, buffer, length);
            irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | (write ? 0 : IRP_INPUT_OPERATION);
            irp->UserBuffer = buffer;
        } else if (top->Flags & DO_DIRECT_IO) {
            irp->MdlAddress = MakeUserMdl(buffer, length, !write);
        } else {
            irp->UserBuffer = buffer;
        }
    }
    TRACE(driver, "%s %p len=%lu", write ? "WRITE" : "READ", h, length);
    NTSTATUS st = FinishRequest(irp, top, t, async);
    if (!async && (of->fo->Flags & FO_SYNCHRONOUS_IO) && !offset && NT_SUCCESS(st) && iosb)
        of->fo->CurrentByteOffset.QuadPart += static_cast<LONGLONG>(iosb->Information);
    return st;
}

}  // namespace

// Called by IofCompleteRequest when every location of a top-level IRP completed.
void TopFinalComplete(PIRP irp, TopContext* t) {
    const NTSTATUS st = irp->IoStatus.Status;
    const ULONG_PTR info = irp->IoStatus.Information;
    if ((irp->Flags & IRP_BUFFERED_IO) && (irp->Flags & IRP_INPUT_OPERATION) && irp->AssociatedIrp.SystemBuffer &&
        t->user_out && !NT_ERROR(st)) {
        ULONG_PTR n = info < t->user_out_len ? info : t->user_out_len;
        if (n && ProbeRange(t->user_out, static_cast<size_t>(n), true))
            std::memcpy(t->user_out, irp->AssociatedIrp.SystemBuffer, static_cast<size_t>(n));
    }
    SetIosb(t->user_iosb, st, info);
    if (t->fo) t->fo->FinalStatus = st;
    if (t->file) {
        AcquireSRWLockExclusive(&t->file->lock);
        auto& v = t->file->pending;
        for (auto it = v.begin(); it != v.end(); ++it)
            if (*it == irp) { v.erase(it); break; }
        ReleaseSRWLockExclusive(&t->file->lock);
    }
    t->completed.store(true);
    if (t->async) {
        HANDLE ev = t->user_event ? t->user_event : t->file_event;
        PFILE_OBJECT fo = t->fo;
        IrpRelease(irp);
        delete t;
        if (ev) SetEvent(ev);
        if (fo) ObjDeref(fo);
        return;
    }
    if (t->user_event) SetEvent(t->user_event);
    SetEvent(t->done);   // last touch: the waiter frees the IRP and the context
}

PFILE_OBJECT HostFileObjectForHandle(HANDLE h) {
    AcquireSRWLockShared(&g_lock);
    auto it = Handles().find(h);
    PFILE_OBJECT fo = it == Handles().end() ? nullptr : it->second->fo;
    ReleaseSRWLockShared(&g_lock);
    return fo;
}

void CloseHandlesOfDriver(DRIVER_OBJECT* drv) {
    std::vector<OpenFile*> victims;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = Handles().begin(); it != Handles().end();) {
        if (it->second->dev->DriverObject == drv) { victims.push_back(it->second); it = Handles().erase(it); }
        else ++it;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (OpenFile* of : victims) {
        WARN(driver, "driver unloading with an open handle %p, closing it", of->handle);
        CloseOpenFile(of);
    }
}

void SendShutdownIrps() {
    for (PDEVICE_OBJECT d : ShutdownDevices()) {
        TRACE(driver, "IRP_MJ_SHUTDOWN -> %ls", NsDeviceName(d).c_str());
        SendSimple(AttachedDevice(d), nullptr, IRP_MJ_SHUTDOWN, KernelMode, 0);
    }
}

NTSTATUS OpenInternalPublic(ACCESS_MASK access, const std::wstring& name, ULONG share, ULONG disposition, ULONG options,
                            ULONG file_attributes, KPROCESSOR_MODE mode, PFILE_OBJECT* out_fo) {
    return OpenInternal(access, name, share, disposition, options, file_attributes, mode, nullptr, out_fo, nullptr);
}

void CloseAllHandles() {
    std::vector<OpenFile*> all;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& kv : Handles()) all.push_back(kv.second);
    Handles().clear();
    LinkHandles().clear();
    ReleaseSRWLockExclusive(&g_lock);
    for (OpenFile* of : all) CloseOpenFile(of);
}

}  // namespace win10xbox::drivers::detail

// ---------------------------------------------------------------------------------------------------
namespace ntk {
using namespace win10xbox::drivers::detail;

NTSTATUS NTAPI IoGetDeviceObjectPointer(PUNICODE_STRING name, ACCESS_MASK access, PFILE_OBJECT* file, PDEVICE_OBJECT* dev) {
    if (!name || !file || !dev) return STATUS_INVALID_PARAMETER;
    PFILE_OBJECT fo = nullptr;
    NTSTATUS st = OpenInternalPublic(access, ToWide(*name), 0, FILE_OPEN, 0, 0, KernelMode, &fo);
    if (!NT_SUCCESS(st)) return st;
    *file = fo;
    *dev = AttachedDevice(fo->DeviceObject);
    ObjRef(*dev);
    return STATUS_SUCCESS;
}

}  // namespace ntk

namespace win10xbox::drivers {
using namespace detail;
namespace {
namespace dsp = win10xbox::dispatch;
}

bool OwnsPath(const OBJECT_ATTRIBUTES* attr) {
    if (!attr || !attr->ObjectName || attr->RootDirectory) return false;
    if (!NsHasObjects()) return false;
    const UNICODE_STRING* us = attr->ObjectName;
    if (!us->Buffer || !us->Length || !ProbeRange(us->Buffer, us->Length, false)) return false;
    return NsResolve(View(*us), nullptr, nullptr) != nullptr;
}

bool OwnsHandle(HANDLE h) {
    if (g_open_count.load() == 0) {
        AcquireSRWLockShared(&g_lock);
        bool any = !LinkHandles().empty();
        ReleaseSRWLockShared(&g_lock);
        if (!any) return false;
    }
    AcquireSRWLockShared(&g_lock);
    bool owned = Handles().count(h) != 0 || LinkHandles().count(h) != 0;
    ReleaseSRWLockShared(&g_lock);
    return owned;
}

NTSTATUS NTAPI Drv_NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                                PLARGE_INTEGER alloc_size, ULONG attributes, ULONG sharing, ULONG disposition,
                                ULONG options, PVOID ea, ULONG ea_length) {
    (void)alloc_size; (void)ea;
    if (!handle || !attr || !attr->ObjectName || !attr->ObjectName->Buffer) return STATUS_INVALID_PARAMETER;
    const KPROCESSOR_MODE mode = (attr->Attributes & OBJ_KERNEL_HANDLE) ? KernelMode : CallerMode();
    if (mode == UserMode && (!ProbeRange(handle, sizeof(*handle), true) || !WritableIosb(io))) return STATUS_ACCESS_VIOLATION;
    if (ea_length) FIXME(driver, "extended attributes passed to a driver device open are dropped");
    HANDLE h = nullptr;
    NTSTATUS st = OpenInternal(access, ToWide(*attr->ObjectName), sharing, disposition, options, attributes, mode, &h, nullptr, io);
    if (NT_SUCCESS(st)) *handle = h;
    return st;
}

NTSTATUS NTAPI Drv_NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK io,
                              ULONG sharing, ULONG options) {
    return Drv_NtCreateFile(handle, access, attr, io, nullptr, 0, sharing, FILE_OPEN, options, nullptr, 0);
}

NTSTATUS NTAPI Drv_NtClose(HANDLE handle) {
    OpenFile* of = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    auto it = Handles().find(handle);
    if (it != Handles().end()) { of = it->second; Handles().erase(it); }
    else if (LinkHandles().erase(handle)) {
        ReleaseSRWLockExclusive(&g_lock);
        dsp::UnregisterEmulatedObject(handle);
        return STATUS_SUCCESS;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!of) return STATUS_INVALID_HANDLE;
    return CloseOpenFile(of);
}

NTSTATUS NTAPI Drv_NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                         PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len) {
    return DoControl(file, event, apc, apc_context, io, code, in, in_len, out, out_len, false);
}
NTSTATUS NTAPI Drv_NtFsControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                   PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out, ULONG out_len) {
    return DoControl(file, event, apc, apc_context, io, code, in, in_len, out, out_len, true);
}
NTSTATUS NTAPI Drv_NtReadFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                              PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key) {
    (void)apc_context;
    return DoReadWrite(false, file, event, apc, io, buffer, length, offset, key);
}
NTSTATUS NTAPI Drv_NtWriteFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context, PIO_STATUS_BLOCK io,
                               PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key) {
    (void)apc_context;
    return DoReadWrite(true, file, event, apc, io, buffer, length, offset, key);
}

NTSTATUS NTAPI Drv_NtFlushBuffersFile(HANDLE file, PIO_STATUS_BLOCK io) {
    Guard g(file);
    if (!g.of) return STATUS_INVALID_HANDLE;
    ULONG_PTR info = 0;
    NTSTATUS st = SendSimple(AttachedDevice(g.of->fo->DeviceObject), g.of->fo, IRP_MJ_FLUSH_BUFFERS, CallerMode(), 0, &info);
    SetIosb(io, st, info);
    return st;
}

NTSTATUS NTAPI Drv_NtQueryVolumeInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer, ULONG length,
                                                ULONG fs_information_class) {
    Guard g(file);
    if (!g.of) return STATUS_INVALID_HANDLE;
    if (fs_information_class != 4 /* FileFsDeviceInformation */) {
        FIXME(driver, "NtQueryVolumeInformationFile class %lu on a driver device is not implemented", fs_information_class);
        return STATUS_NOT_IMPLEMENTED;
    }
    if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
    PDEVICE_OBJECT d = g.of->fo->DeviceObject;
    ULONG v[2] = {d->DeviceType, d->Characteristics};
    std::memcpy(buffer, v, sizeof v);
    SetIosb(io, STATUS_SUCCESS, sizeof v);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Drv_NtOpenSymbolicLinkObject(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr) {
    (void)access;
    if (!handle || !attr || !attr->ObjectName) return STATUS_INVALID_PARAMETER;
    std::wstring target;
    if (!NsQueryLink(View(*attr->ObjectName), &target)) return STATUS_OBJECT_NAME_NOT_FOUND;
    HANDLE h = dsp::RegisterEmulatedObject(dsp::ObjectKind::Other, g_serial++, nullptr);
    if (!h) return STATUS_INSUFFICIENT_RESOURCES;
    AcquireSRWLockExclusive(&g_lock);
    LinkHandles()[h] = target;
    ReleaseSRWLockExclusive(&g_lock);
    *handle = h;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Drv_NtQuerySymbolicLinkObject(HANDLE handle, PUNICODE_STRING target, PULONG returned_length) {
    std::wstring t;
    AcquireSRWLockShared(&g_lock);
    auto it = LinkHandles().find(handle);
    bool found = it != LinkHandles().end();
    if (found) t = it->second;
    ReleaseSRWLockShared(&g_lock);
    if (!found) return STATUS_INVALID_HANDLE;
    const ULONG bytes = static_cast<ULONG>(t.size() * sizeof(wchar_t));
    if (returned_length) *returned_length = bytes;
    if (!target) return STATUS_INVALID_PARAMETER;
    if (target->MaximumLength < bytes) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(target->Buffer, t.c_str(), bytes);
    target->Length = static_cast<USHORT>(bytes);
    if (target->MaximumLength >= bytes + sizeof(wchar_t)) target->Buffer[bytes / 2] = 0;
    return STATUS_SUCCESS;
}

bool IsDriverLinkPath(const OBJECT_ATTRIBUTES* attr) {
    if (!attr || !attr->ObjectName || attr->RootDirectory || !NsHasObjects()) return false;
    return NsQueryLink(View(*attr->ObjectName), nullptr);
}

}  // namespace win10xbox::drivers
