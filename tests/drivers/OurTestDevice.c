/*
 * OurTestDevice.sys - the test WDM driver of the driver host (MIT).
 *
 * Creates \Device\OurTestDevice and \DosDevices\OurTestDevice (\??\OurTestDevice) and answers
 * IOCTLs of every buffering method, read/write, a request that completes from a DPC, a timer, and
 * a self test that exercises the kernel API from kernel mode. Built freestanding against
 * ntk/ntk.h with the import library of the host's ntoskrnl.
 */
#include "ntk/ntk.h"
#include "our_test_device.h"

#define TAG NTK_TAG('O', 'T', 's', 't')

typedef struct _DEVICE_EXT {
    LONG OpenCount;
    FAST_MUTEX Lock;
    KDPC Dpc;
    KDPC TimerDpcObj;
    KTIMER Timer;
    PIRP DeferredIrp;
    ULONG DispatchIrql;
    ULONG CallerThreadId;
    ULONG TimerMs;
    LARGE_INTEGER TimerStart;
    UCHAR Store[4096];
    ULONG StoreLen;
} DEVICE_EXT, *PDEVICE_EXT;

static PDEVICE_OBJECT g_device;
static WCHAR g_reg_path[64];
static ULONG g_reg_len;

static NTSTATUS Complete(PIRP irp, NTSTATUS status, ULONG_PTR info) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    PDEVICE_EXT ext = (PDEVICE_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (s->MajorFunction == IRP_MJ_CREATE) InterlockedIncrement(&ext->OpenCount);
    else if (s->MajorFunction == IRP_MJ_CLOSE) InterlockedDecrement(&ext->OpenCount);
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DispatchCleanup(PDEVICE_OBJECT dev, PIRP irp) {
    UNREFERENCED_PARAMETER(dev);
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DispatchRead(PDEVICE_OBJECT dev, PIRP irp) {
    PDEVICE_EXT ext = (PDEVICE_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG n = s->Parameters.Read.Length;
    if (n > ext->StoreLen) n = ext->StoreLen;
    memcpy(irp->AssociatedIrp.SystemBuffer, ext->Store, n); /* DO_BUFFERED_IO */
    return Complete(irp, STATUS_SUCCESS, n);
}

static NTSTATUS DispatchWrite(PDEVICE_OBJECT dev, PIRP irp) {
    PDEVICE_EXT ext = (PDEVICE_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG n = s->Parameters.Write.Length;
    if (n > sizeof(ext->Store)) n = sizeof(ext->Store);
    memcpy(ext->Store, irp->AssociatedIrp.SystemBuffer, n);
    ext->StoreLen = n;
    return Complete(irp, STATUS_SUCCESS, n);
}

static VOID NTAPI CancelPending(PDEVICE_OBJECT dev, PIRP irp) {
    PDEVICE_EXT ext = (PDEVICE_EXT)dev->DeviceExtension;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeCancelTimer(&ext->Timer);
    if (ext->DeferredIrp == irp) ext->DeferredIrp = NULL;
    Complete(irp, STATUS_CANCELLED, 0);
}

/* Parks `irp` as the pending request (cancellable). Returns STATUS_PENDING. */
static NTSTATUS Park(PDEVICE_EXT ext, PIRP irp) {
    ext->DeferredIrp = irp;
    IoMarkIrpPending(irp);
    IoSetCancelRoutine(irp, CancelPending);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        ext->DeferredIrp = NULL;
        Complete(irp, STATUS_CANCELLED, 0);
    }
    return STATUS_PENDING;
}

static VOID NTAPI DeferredDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2) {
    PDEVICE_EXT ext = (PDEVICE_EXT)context;
    PIRP irp = ext->DeferredIrp;
    OTD_DEFERRED_OUT* out;
    UNREFERENCED_PARAMETER(dpc); UNREFERENCED_PARAMETER(a1); UNREFERENCED_PARAMETER(a2);
    if (!irp || !IoSetCancelRoutine(irp, NULL)) return;   /* nothing parked, or the cancel routine owns it */
    ext->DeferredIrp = NULL;
    out = (OTD_DEFERRED_OUT*)irp->AssociatedIrp.SystemBuffer;
    out->Magic = OTD_DEFERRED_MAGIC;
    out->DispatchIrql = ext->DispatchIrql;
    out->DpcIrql = KeGetCurrentIrql();
    out->DpcThreadId = (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
    out->CallerThreadId = ext->CallerThreadId;
    Complete(irp, STATUS_SUCCESS, sizeof(*out));
}

static VOID NTAPI TimerDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2) {
    PDEVICE_EXT ext = (PDEVICE_EXT)context;
    PIRP irp = ext->DeferredIrp;
    LARGE_INTEGER now;
    UNREFERENCED_PARAMETER(dpc); UNREFERENCED_PARAMETER(a1); UNREFERENCED_PARAMETER(a2);
    if (!irp || !IoSetCancelRoutine(irp, NULL)) return;
    ext->DeferredIrp = NULL;
    KeQuerySystemTime(&now);
    *(ULONG*)irp->AssociatedIrp.SystemBuffer = (ULONG)((now.QuadPart - ext->TimerStart.QuadPart) / 10000); /* ms */
    Complete(irp, STATUS_SUCCESS, sizeof(ULONG));
}

/* Kernel API checks that run in kernel mode; bit i set = check i passed. */
static ULONG SelfTest(PDEVICE_EXT ext) {
    ULONG ok = 0;
    KEVENT ev, sev;
    KSEMAPHORE sem;
    KMUTEX mtx;
    KSPIN_LOCK spin;
    KIRQL old;
    LARGE_INTEGER zero, t0, t1, d;
    ERESOURCE res;
    LIST_ENTRY head, a, b;
    UNICODE_STRING us, us2;
    WCHAR buf[16];
    PVOID p;
    NTSTATUS st;
    zero.QuadPart = 0;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    KeInitializeEvent(&sev, SynchronizationEvent, TRUE);
    if (KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero) == STATUS_TIMEOUT) ok |= 1u << 0;
    KeSetEvent(&ev, 0, FALSE);
    if (KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS) ok |= 1u << 1;
    /* a synchronization event releases exactly one waiter and resets */
    if (KeWaitForSingleObject(&sev, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS &&
        KeWaitForSingleObject(&sev, Executive, KernelMode, FALSE, &zero) == STATUS_TIMEOUT) ok |= 1u << 2;

    KeInitializeSemaphore(&sem, 1, 2);
    if (KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS &&
        KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero) == STATUS_TIMEOUT) {
        KeReleaseSemaphore(&sem, 0, 1, FALSE);
        if (KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS) ok |= 1u << 3;
    }

    KeInitializeMutex(&mtx, 0);
    if (KeWaitForSingleObject(&mtx, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS &&
        KeWaitForSingleObject(&mtx, Executive, KernelMode, FALSE, &zero) == STATUS_SUCCESS) { /* recursive */
        KeReleaseMutex(&mtx, FALSE);
        KeReleaseMutex(&mtx, FALSE);
        ok |= 1u << 4;
    }

    KeInitializeSpinLock(&spin);
    KeAcquireSpinLock(&spin, &old);
    if (KeGetCurrentIrql() == DISPATCH_LEVEL) ok |= 1u << 5;
    KeReleaseSpinLock(&spin, old);
    if (KeGetCurrentIrql() == old && old == PASSIVE_LEVEL) ok |= 1u << 6;

    KeRaiseIrql(APC_LEVEL, &old);
    if (KeGetCurrentIrql() == APC_LEVEL) ok |= 1u << 7;
    KeLowerIrql(old);

    ExAcquireFastMutex(&ext->Lock);
    if (KeGetCurrentIrql() == APC_LEVEL) ok |= 1u << 8;
    ExReleaseFastMutex(&ext->Lock);

    ExInitializeResourceLite(&res);
    if (ExAcquireResourceExclusiveLite(&res, FALSE) && ExAcquireResourceSharedLite(&res, FALSE)) {
        ExReleaseResourceLite(&res);
        ExReleaseResourceLite(&res);
        ok |= 1u << 9;
    }
    ExDeleteResourceLite(&res);

    p = ExAllocatePoolWithTag(NonPagedPool, 100, TAG);
    if (p) {
        memset(p, 0xAB, 100);
        ExFreePoolWithTag(p, TAG);
        ok |= 1u << 10;
    }

    InitializeListHead(&head);
    InsertTailList(&head, &a);
    InsertHeadList(&head, &b);
    if (RemoveHeadList(&head) == &b && RemoveTailList(&head) == &a && IsListEmpty(&head)) ok |= 1u << 11;

    RtlInitUnicodeString(&us, L"Hello");
    us2.Buffer = buf; us2.Length = 0; us2.MaximumLength = sizeof(buf);
    RtlCopyUnicodeString(&us2, &us);
    if (us2.Length == 10 && us2.Buffer[0] == L'H' && us2.Buffer[4] == L'o') ok |= 1u << 12;

    KeQuerySystemTime(&t0);
    d.QuadPart = -200000; /* 20 ms relative */
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    KeQuerySystemTime(&t1);
    if ((t1.QuadPart - t0.QuadPart) >= 150000) ok |= 1u << 13;

    st = ObReferenceObjectByHandle((HANDLE)(LONG_PTR)-1, 0, NULL, KernelMode, &p, NULL);
    if (st == STATUS_SUCCESS && p == IoGetCurrentProcess()) { ObfDereferenceObject(p); ok |= 1u << 14; }

    DbgPrint("OurTestDevice: self test, mask %08lx\n", (unsigned long)ok);
    if (MmIsAddressValid(&ok)) ok |= 1u << 15;
    return ok;
}

static NTSTATUS DispatchControl(PDEVICE_OBJECT dev, PIRP irp) {
    PDEVICE_EXT ext = (PDEVICE_EXT)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG inl = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outl = s->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR sysbuf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
    ULONG i;

    switch (code) {
    case IOCTL_OTD_XOR:
        if (outl < inl) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        for (i = 0; i < inl; ++i) sysbuf[i] ^= OTD_XOR_KEY;
        return Complete(irp, STATUS_SUCCESS, inl);

    case IOCTL_OTD_FILL: {
        PUCHAR out;
        if (inl < 1 || !irp->MdlAddress) return Complete(irp, STATUS_INVALID_PARAMETER, 0);
        out = (PUCHAR)MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
        if (!out) return Complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        memset(out, sysbuf[0], MmGetMdlByteCount(irp->MdlAddress));
        return Complete(irp, STATUS_SUCCESS, MmGetMdlByteCount(irp->MdlAddress));
    }

    case IOCTL_OTD_SUM: {
        PUCHAR data;
        ULONG sum = 0;
        if (!irp->MdlAddress) return Complete(irp, STATUS_INVALID_PARAMETER, 0);
        data = (PUCHAR)MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
        if (!data) return Complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        for (i = 0; i < MmGetMdlByteCount(irp->MdlAddress); ++i) sum += data[i];
        return Complete(irp, STATUS_SUCCESS, sum);
    }

    case IOCTL_OTD_NEITHER: {
        PULONG in = (PULONG)s->Parameters.DeviceIoControl.Type3InputBuffer;
        PULONG out = (PULONG)irp->UserBuffer;
        if (inl < sizeof(ULONG) || outl < sizeof(ULONG) || !in || !out) return Complete(irp, STATUS_INVALID_PARAMETER, 0);
        *out = *in + 1;
        return Complete(irp, STATUS_SUCCESS, sizeof(ULONG));
    }

    case IOCTL_OTD_DEFERRED:
        if (outl < sizeof(OTD_DEFERRED_OUT)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        ext->DispatchIrql = KeGetCurrentIrql();
        ext->CallerThreadId = (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
        Park(ext, irp);
        KeInsertQueueDpc(&ext->Dpc, NULL, NULL);
        return STATUS_PENDING;

    case IOCTL_OTD_TIMER: {
        LARGE_INTEGER due;
        if (inl < sizeof(ULONG) || outl < sizeof(ULONG)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        ext->TimerMs = *(PULONG)sysbuf;
        KeQuerySystemTime(&ext->TimerStart);
        Park(ext, irp);
        due.QuadPart = -(LONGLONG)ext->TimerMs * 10000;
        KeSetTimer(&ext->Timer, due, &ext->TimerDpcObj);
        return STATUS_PENDING;
    }

    case IOCTL_OTD_SELFTEST:
        if (outl < sizeof(ULONG)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        *(PULONG)sysbuf = SelfTest(ext);
        return Complete(irp, STATUS_SUCCESS, sizeof(ULONG));

    case IOCTL_OTD_INFO: {
        OTD_INFO* info = (OTD_INFO*)sysbuf;
        if (outl < sizeof(*info)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        info->Irql = KeGetCurrentIrql();
        info->PreviousMode = (ULONG)ExGetPreviousMode();
        info->ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
        info->ThreadId = (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
        info->OpenCount = (ULONG)ext->OpenCount;
        info->RegistryPathChars = g_reg_len;
        memcpy(info->RegistryPath, g_reg_path, g_reg_len * sizeof(WCHAR));
        return Complete(irp, STATUS_SUCCESS, sizeof(*info));
    }

    default:
        return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID NTAPI DriverUnload(PDRIVER_OBJECT drv) {
    UNICODE_STRING link;
    UNREFERENCED_PARAMETER(drv);
    RtlInitUnicodeString(&link, L"\\DosDevices\\OurTestDevice");
    IoDeleteSymbolicLink(&link);
    if (g_device) {
        PDEVICE_EXT ext = (PDEVICE_EXT)g_device->DeviceExtension;
        KeCancelTimer(&ext->Timer);
        IoDeleteDevice(g_device);
        g_device = NULL;
    }
    DbgPrint("OurTestDevice: unloaded\n");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING registry_path) {
    UNICODE_STRING name, link;
    PDEVICE_EXT ext;
    NTSTATUS st;

    DbgPrint("OurTestDevice: DriverEntry, registry path %wZ\n", registry_path);
    g_reg_len = registry_path->Length / sizeof(WCHAR);
    if (g_reg_len > 63) g_reg_len = 63;
    memcpy(g_reg_path, registry_path->Buffer, g_reg_len * sizeof(WCHAR));

    RtlInitUnicodeString(&name, L"\\Device\\OurTestDevice");
    RtlInitUnicodeString(&link, L"\\DosDevices\\OurTestDevice");
    st = IoCreateDevice(drv, sizeof(DEVICE_EXT), &name, OTD_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN, FALSE, &g_device);
    if (!NT_SUCCESS(st)) return st;
    g_device->Flags |= DO_BUFFERED_IO;
    ext = (PDEVICE_EXT)g_device->DeviceExtension;
    ExInitializeFastMutex(&ext->Lock);
    KeInitializeDpc(&ext->Dpc, DeferredDpc, ext);
    KeInitializeDpc(&ext->TimerDpcObj, TimerDpc, ext);
    KeInitializeTimerEx(&ext->Timer, NotificationTimer);

    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(g_device);
        g_device = NULL;
        return st;
    }
    drv->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLEANUP] = DispatchCleanup;
    drv->MajorFunction[IRP_MJ_READ] = DispatchRead;
    drv->MajorFunction[IRP_MJ_WRITE] = DispatchWrite;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchControl;
    drv->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}
