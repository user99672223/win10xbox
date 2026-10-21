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
#include "kmtest.h"

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
    PKINTERRUPT Intr;
    volatile LONG IsrCount;
    volatile ULONG IsrIrql;
    LONG SyncCount;
    ULONG SyncIrql;
} DEVICE_EXT, *PDEVICE_EXT;

static PDEVICE_OBJECT g_device;
static WCHAR g_reg_path[96];
static ULONG g_reg_len;

static BOOLEAN NTAPI Isr(PKINTERRUPT intr, PVOID context) {
    PDEVICE_EXT ext = (PDEVICE_EXT)context;
    UNREFERENCED_PARAMETER(intr);
    ext->IsrIrql = KeGetCurrentIrql();
    InterlockedIncrement(&ext->IsrCount);
    return TRUE;
}

/* Runs at the connected interrupt's synchronize IRQL with the interrupt lock held. */
static BOOLEAN NTAPI ReadIsrState(PVOID context) {
    PDEVICE_EXT ext = (PDEVICE_EXT)context;
    ext->SyncCount = ext->IsrCount;
    ext->SyncIrql = KeGetCurrentIrql();
    return TRUE;
}

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

KMT_RESULTS KmtResults;

/* Kernel API checks that run in kernel mode, kmtest style. */
static void SelfTest(PDEVICE_EXT ext) {
    KEVENT ev, sev;
    KSEMAPHORE sem;
    KMUTEX mtx;
    KSPIN_LOCK spin;
    KIRQL old, old2;
    LARGE_INTEGER zero, t0, t1, d;
    ERESOURCE res;
    LIST_ENTRY head, a, b;
    UNICODE_STRING us, us2;
    WCHAR buf[16];
    PVOID p, q;
    NTSTATUS st;
    int i;
    zero.QuadPart = 0;
    KMT_RESET();

    ok_irql(PASSIVE_LEVEL);

    /* events */
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    KeInitializeEvent(&sev, SynchronizationEvent, TRUE);
    ok_eq_hex(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    ok_eq_int(KeSetEvent(&ev, 0, FALSE), 0);
    ok_eq_int(KeSetEvent(&ev, 0, FALSE), 1);               /* returns the previous state */
    ok_eq_hex(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    ok_eq_hex(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);   /* notification: stays set */
    ok_eq_int(KeResetEvent(&ev), 1);
    ok_eq_hex(KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    ok_eq_hex(KeWaitForSingleObject(&sev, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);   /* auto-reset: one waiter */
    ok_eq_hex(KeWaitForSingleObject(&sev, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);

    /* semaphore */
    KeInitializeSemaphore(&sem, 1, 2);
    ok_eq_hex(KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    ok_eq_hex(KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    ok_eq_int(KeReleaseSemaphore(&sem, 0, 1, FALSE), 0);
    ok_eq_hex(KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);

    /* mutex is recursive for its owner */
    KeInitializeMutex(&mtx, 0);
    ok_eq_hex(KeWaitForSingleObject(&mtx, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    ok_eq_hex(KeWaitForSingleObject(&mtx, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    ok_eq_int(KeReleaseMutex(&mtx, FALSE), -1);
    ok_eq_int(KeReleaseMutex(&mtx, FALSE), 0);

    /* spin lock + IRQL */
    KeInitializeSpinLock(&spin);
    KeAcquireSpinLock(&spin, &old);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_int(old, PASSIVE_LEVEL);
    KeReleaseSpinLock(&spin, old);
    ok_irql(PASSIVE_LEVEL);
    KeRaiseIrql(APC_LEVEL, &old);
    ok_irql(APC_LEVEL);
    KeRaiseIrql(DISPATCH_LEVEL, &old2);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_int(old2, APC_LEVEL);
    KeLowerIrql(old2);
    ok_irql(APC_LEVEL);
    KeLowerIrql(old);
    ok_irql(PASSIVE_LEVEL);

    /* fast mutex runs at APC_LEVEL */
    ExAcquireFastMutex(&ext->Lock);
    ok_irql(APC_LEVEL);
    ExReleaseFastMutex(&ext->Lock);
    ok_irql(PASSIVE_LEVEL);

    /* resources: exclusive owner may re-enter shared */
    st = ExInitializeResourceLite(&res);
    ok_ntstatus(st, STATUS_SUCCESS);
    ok(ExAcquireResourceExclusiveLite(&res, FALSE), "exclusive acquire");
    ok(ExAcquireResourceSharedLite(&res, FALSE), "shared acquire by the exclusive owner");
    ExReleaseResourceLite(&res);
    ExReleaseResourceLite(&res);
    ok(ExAcquireResourceSharedLite(&res, FALSE), "shared acquire");
    ExReleaseResourceLite(&res);
    ExDeleteResourceLite(&res);

    /* pool */
    p = ExAllocatePoolWithTag(NonPagedPool, 100, TAG);
    ok(p != NULL, "pool allocation");
    if (p) {
        int zeroed = 1;
        for (i = 0; i < 100; ++i) if (((PUCHAR)p)[i]) zeroed = 0;
        ok(zeroed, "fresh pool is zeroed in this host");
        ok_eq_int(((ULONG_PTR)p) % 16, 0);
        memset(p, 0xAB, 100);
        ExFreePoolWithTag(p, TAG);
    }
    q = ExAllocatePoolWithTag(PagedPool, 4096, TAG);
    ok(q != NULL, "paged pool allocation");
    ExFreePool(q);

    /* lists */
    InitializeListHead(&head);
    ok(IsListEmpty(&head), "empty list");
    InsertTailList(&head, &a);
    InsertHeadList(&head, &b);
    ok(RemoveHeadList(&head) == &b, "head");
    ok(RemoveTailList(&head) == &a, "tail");
    ok(IsListEmpty(&head), "empty again");

    /* unicode strings */
    RtlInitUnicodeString(&us, L"Hello");
    ok_eq_int(us.Length, 10);
    ok_eq_int(us.MaximumLength, 12);
    us2.Buffer = buf; us2.Length = 0; us2.MaximumLength = sizeof(buf);
    RtlCopyUnicodeString(&us2, &us);
    ok_eq_int(us2.Length, 10);
    ok_eq_wstr(us2.Buffer, L"Hello");

    /* time and delays */
    KeQuerySystemTime(&t0);
    d.QuadPart = -200000;   /* 20 ms relative */
    ok_eq_hex(KeDelayExecutionThread(KernelMode, FALSE, &d), STATUS_SUCCESS);
    KeQuerySystemTime(&t1);
    ok((t1.QuadPart - t0.QuadPart) >= 150000, "KeDelayExecutionThread slept");

    /* objects */
    st = ObReferenceObjectByHandle((HANDLE)(LONG_PTR)-1, 0, NULL, KernelMode, &p, NULL);   /* NtCurrentProcess() */
    ok_ntstatus(st, STATUS_SUCCESS);
    ok(p == IoGetCurrentProcess(), "current process object");
    ObfDereferenceObject(p);
    ok(MmIsAddressValid(&st), "stack address valid");
    ok(!MmIsAddressValid((PVOID)(ULONG_PTR)0x10), "null page invalid");

    DbgPrint("OurTestDevice: self test, %lu assertions, %lu failed\n", KmtResults.Executed, KmtResults.Failed);
}

/* SEH: needs the .pdata of the image registered with the host and __C_specific_handler bound to the host's. */
static ULONG g_finally_ran;
static void RaiseInside(void) {
    __try {
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    } __finally {
        g_finally_ran = 1;
    }
}

/* clang only treats calls (not plain stores) inside __try as potentially faulting, so the access goes through a call. */
__attribute__((noinline)) static void Poke(volatile ULONG* p) { *p = 0x1234; }

/* Exception filter recording the code. (GetExceptionCode() in the handler body depends on the unwinder
 * returning the code in RAX, which Windows' __C_specific_handler does and Wine's does not.) */
static int RecordCode(PEXCEPTION_POINTERS info, unsigned long* code) {
    *code = info->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void RunSeh(OTD_SEH_OUT* out) {
    volatile ULONG* null_ptr = (volatile ULONG*)(ULONG_PTR)0;
    g_finally_ran = 0;
    out->CaughtRaise = out->CaughtFault = 0;
    __try {
        RaiseInside();
    } __except (RecordCode(GetExceptionInformation(), &out->RaiseCode)) {
        out->CaughtRaise = 1;
    }
    out->FinallyRan = g_finally_ran;
    __try {
        Poke(null_ptr);
    } __except (RecordCode(GetExceptionInformation(), &out->FaultCode)) {
        out->CaughtFault = 1;
    }
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
        if (outl < sizeof(OTD_SELFTEST_OUT)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        SelfTest(ext);
        memcpy(sysbuf, &KmtResults, sizeof(OTD_SELFTEST_OUT));
        return Complete(irp, STATUS_SUCCESS, sizeof(OTD_SELFTEST_OUT));

    case IOCTL_OTD_SEH:
        if (outl < sizeof(OTD_SEH_OUT)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RunSeh((OTD_SEH_OUT*)sysbuf);
        return Complete(irp, STATUS_SUCCESS, sizeof(OTD_SEH_OUT));

    case IOCTL_OTD_INFO: {
        OTD_INFO* info = (OTD_INFO*)sysbuf;
        if (outl < sizeof(*info)) return Complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        info->Irql = KeGetCurrentIrql();
        info->PreviousMode = (ULONG)ExGetPreviousMode();
        info->ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
        info->ThreadId = (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
        info->OpenCount = (ULONG)ext->OpenCount;
        info->RegistryPathChars = g_reg_len;
        KeSynchronizeExecution(ext->Intr, ReadIsrState, ext);
        info->IsrCount = (ULONG)ext->SyncCount;
        info->IsrIrql = ext->IsrIrql;
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
        if (ext->Intr) IoDisconnectInterrupt(ext->Intr);
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
    if (g_reg_len > 95) g_reg_len = 95;
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

    st = IoConnectInterrupt(&ext->Intr, Isr, ext, NULL, OTD_INTERRUPT_VECTOR, OTD_ISR_IRQL, OTD_ISR_IRQL, Latched, FALSE, 1, FALSE);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(g_device);
        g_device = NULL;
        return st;
    }
    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        IoDisconnectInterrupt(ext->Intr);
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
