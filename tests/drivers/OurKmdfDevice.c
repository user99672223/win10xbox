/*
 * OurKmdfDevice.sys - a KMDF (non-PnP, control device) test driver, MIT. Built against Microsoft's
 * public KMDF 1.33 headers; runs on the host's WDF shim (drivers/kmdf).
 */
#include <ntddk.h>
#include "kmdf_min.h"
#include "our_kmdf_device.h"

typedef struct _DEVICE_CONTEXT {
    ULONG Opens;
    ULONG Closes;
    ULONG Ioctls;
    WDFSPINLOCK Lock;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

typedef struct _FILE_CONTEXT {
    ULONG Serial;
} FILE_CONTEXT, *PFILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT, GetFileContext)

static WDFDEVICE g_device;

static VOID NTAPI EvtFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file) {
    PDEVICE_CONTEXT dc = GetDeviceContext(device);
    WDF_OBJECT_ATTRIBUTES attr;
    PFILE_CONTEXT fc = NULL;
    NTSTATUS st;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, FILE_CONTEXT);
    st = WdfObjectAllocateContext(file, &attr, (PVOID *)&fc);
    if (NT_SUCCESS(st) && fc) {
        WdfSpinLockAcquire(dc->Lock);
        fc->Serial = ++dc->Opens;
        WdfSpinLockRelease(dc->Lock);
    }
    WdfRequestComplete(request, st);
}

static VOID NTAPI EvtFileClose(WDFFILEOBJECT file) {
    PDEVICE_CONTEXT dc = GetDeviceContext(WdfFileObjectGetDevice(file));
    UNREFERENCED_PARAMETER(file);
    InterlockedIncrement((volatile LONG *)&dc->Closes);
}

static VOID NTAPI EvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t out_len, size_t in_len, ULONG code) {
    PDEVICE_CONTEXT dc = GetDeviceContext(WdfIoQueueGetDevice(queue));
    PVOID in = NULL, out = NULL;
    size_t in_got = 0, out_got = 0, i;
    NTSTATUS st;
    InterlockedIncrement((volatile LONG *)&dc->Ioctls);

    switch (code) {
    case IOCTL_OKD_XOR:
        st = WdfRequestRetrieveInputBuffer(request, 1, &in, &in_got);
        if (!NT_SUCCESS(st)) break;
        st = WdfRequestRetrieveOutputBuffer(request, in_got, &out, &out_got);
        if (!NT_SUCCESS(st)) break;
        for (i = 0; i < in_got; ++i) ((PUCHAR)out)[i] = ((PUCHAR)in)[i] ^ OKD_XOR_KEY;
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, in_got);
        return;

    case IOCTL_OKD_FILL:
        st = WdfRequestRetrieveInputBuffer(request, 1, &in, &in_got);
        if (!NT_SUCCESS(st)) break;
        st = WdfRequestRetrieveOutputBuffer(request, 1, &out, &out_got);
        if (!NT_SUCCESS(st)) break;
        RtlFillMemory(out, out_got, *(PUCHAR)in);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, out_got);
        return;

    case IOCTL_OKD_STATS: {
        OKD_STATS *s;
        WDF_REQUEST_PARAMETERS params;
        PFILE_CONTEXT fc = GetFileContext(WdfRequestGetFileObject(request));
        PIRP irp = WdfRequestWdmGetIrp(request);
        st = WdfRequestRetrieveOutputBuffer(request, sizeof(*s), &out, &out_got);
        if (!NT_SUCCESS(st)) break;
        s = (OKD_STATS *)out;
        WDF_REQUEST_PARAMETERS_INIT(&params);
        WdfRequestGetParameters(request, &params);
        RtlZeroMemory(s, sizeof(*s));
        s->Opens = dc->Opens;
        s->Closes = dc->Closes;
        s->Ioctls = dc->Ioctls;
        s->ParamIoctlCode = params.Parameters.DeviceIoControl.IoControlCode;
        s->ParamInputLength = (ULONG)params.Parameters.DeviceIoControl.InputBufferLength;
        s->ParamOutputLength = (ULONG)params.Parameters.DeviceIoControl.OutputBufferLength;
        s->RequestorMode = WdfRequestGetRequestorMode(request);
        s->WdmIoctl = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.IoControlCode;
        s->FileCtx = fc ? fc->Serial : 0;
        /* No KeGetCurrentIrql() here: the ddk headers inline it as `mov rax, cr8`, which belongs to the
         * privileged-instruction handler, not to the driver host. */
        s->SpinLockOk = 1;
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*s));
        return;
    }

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    UNREFERENCED_PARAMETER(in_len);
    UNREFERENCED_PARAMETER(out_len);
    WdfRequestComplete(request, st);
}

static VOID NTAPI EvtIoWrite(WDFQUEUE queue, WDFREQUEST request, size_t length) {
    UNREFERENCED_PARAMETER(queue);
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, length);   /* accepts and drops */
}

static VOID NTAPI EvtDriverUnload(WDFDRIVER driver) {
    UNREFERENCED_PARAMETER(driver);
    DbgPrint("OurKmdfDevice: EvtDriverUnload\n");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING registry_path) {
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_FILEOBJECT_CONFIG fconfig;
    WDF_IO_QUEUE_CONFIG qconfig;
    WDFDRIVER driver;
    PWDFDEVICE_INIT init;
    PDEVICE_CONTEXT dc;
    UNICODE_STRING name, link;
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NTSTATUS st;

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = EvtDriverUnload;
    st = WdfDriverCreate(drv, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(st)) return st;

    init = WdfControlDeviceInitAllocate(driver, &sddl);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    RtlInitUnicodeString(&name, L"\\Device\\OurKmdfDevice");
    RtlInitUnicodeString(&link, L"\\DosDevices\\OurKmdfDevice");
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    st = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(st)) { WdfDeviceInitFree(init); return st; }
    WDF_FILEOBJECT_CONFIG_INIT(&fconfig, EvtFileCreate, EvtFileClose, WDF_NO_EVENT_CALLBACK);
    WdfDeviceInitSetFileObjectConfig(init, &fconfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&init, &attr, &g_device);
    if (!NT_SUCCESS(st)) { if (init) WdfDeviceInitFree(init); return st; }
    dc = GetDeviceContext(g_device);
    if (!dc) return STATUS_UNSUCCESSFUL;
    st = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &dc->Lock);
    if (!NT_SUCCESS(st)) return st;

    st = WdfDeviceCreateSymbolicLink(g_device, &link);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qconfig, WdfIoQueueDispatchSequential);
    qconfig.EvtIoDeviceControl = EvtIoDeviceControl;
    qconfig.EvtIoWrite = EvtIoWrite;
    st = WdfIoQueueCreate(g_device, &qconfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(st)) return st;

    WdfControlFinishInitializing(g_device);
    DbgPrint("OurKmdfDevice: control device ready\n");
    return STATUS_SUCCESS;
}
