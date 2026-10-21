/*
 * OurStubProbe.sys / OurFailDriver.sys (one source, two builds) - MIT.
 *
 * Probe: DriverEntry calls an ntoskrnl API the host does not implement and succeeds only if the
 * call returned STATUS_NOT_IMPLEMENTED (the loud stub), then creates \Device\OurStubProbe.
 * Fail (PROBE_FAIL): creates a device and a symbolic link, then returns STATUS_UNSUCCESSFUL so the
 * host has to roll the half-initialised driver back.
 */
#include "ntk/ntk.h"

NTKAPI NTSTATUS NTAPI IoRegisterPlugPlayNotification(int EventCategory, ULONG EventCategoryFlags, PVOID EventCategoryData,
                                                      PDRIVER_OBJECT DriverObject, PVOID CallbackRoutine, PVOID Context,
                                                      PVOID *NotificationEntry);

#ifdef PROBE_FAIL
#define DEV_NAME L"\\Device\\OurFailDriver"
#define LINK_NAME L"\\DosDevices\\OurFailDriver"
#else
#define DEV_NAME L"\\Device\\OurStubProbe"
#define LINK_NAME L"\\DosDevices\\OurStubProbe"
#endif

static NTSTATUS NTAPI Unsupported(PDEVICE_OBJECT dev, PIRP irp) {
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING registry_path) {
    UNICODE_STRING name, link;
    PDEVICE_OBJECT dev;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(registry_path);

#ifndef PROBE_FAIL
    PVOID entry = NULL;
    st = IoRegisterPlugPlayNotification(0, 0, NULL, drv, NULL, NULL, &entry);
    if (st != STATUS_NOT_IMPLEMENTED) return STATUS_UNSUCCESSFUL; /* a silent success would be a host bug */
#endif
    RtlInitUnicodeString(&name, DEV_NAME);
    RtlInitUnicodeString(&link, LINK_NAME);
    st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;
    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) return st;
    drv->MajorFunction[IRP_MJ_CREATE] = Unsupported;
    drv->MajorFunction[IRP_MJ_CLOSE] = Unsupported;
#ifdef PROBE_FAIL
    return STATUS_UNSUCCESSFUL;
#else
    return STATUS_SUCCESS;
#endif
}
