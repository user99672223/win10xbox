/*
 * kmdf_stub.c - stand-in for the WDK's WdfDriverEntry.lib (FxDriverEntry), MIT.
 *
 * A KMDF driver's real entry point is the stub's FxDriverEntry: it binds to the framework through
 * WDFLDR.SYS (WdfVersionBind), calls the driver's DriverEntry, and unbinds on failure or at unload.
 * This is the same protocol; the WDK only ships the stub as a binary library.
 */
#include <ntddk.h>
#include "kmdf_min.h"

/* WDF_BIND_INFO (fxldr.h): private to Microsoft's loader, mirrored here. */
typedef struct _STUB_WDF_BIND_INFO {
    ULONG Size;
    PWCHAR Component;
    struct { ULONG Major, Minor, Build; } Version;
    ULONG FuncCount;
    const WDFFUNC **FuncTable;
    PVOID Module;
} STUB_WDF_BIND_INFO;

/* WDF_BIND_INFO2: what stubs of KMDF >= 1.25 pass (V1 embedded, then pointers to the stub's globals). */
typedef struct _STUB_WDF_BIND_INFO2 {
    STUB_WDF_BIND_INFO V1;
    ULONG *MinimumVersionRequired;
    BOOLEAN *ClientVersionHigherThanFramework;
    ULONG *FuncCountPtr;
    ULONG *StructCountPtr;
    WDF_STRUCT_INFO *StructTable;
} STUB_WDF_BIND_INFO2;

DECLSPEC_IMPORT NTSTATUS NTAPI WdfVersionBind(PDRIVER_OBJECT, PUNICODE_STRING, STUB_WDF_BIND_INFO2 *, PWDF_DRIVER_GLOBALS *);
DECLSPEC_IMPORT NTSTATUS NTAPI WdfVersionUnbind(PUNICODE_STRING, STUB_WDF_BIND_INFO2 *, PWDF_DRIVER_GLOBALS);

PWDF_DRIVER_GLOBALS WdfDriverGlobals = NULL;
const WDFFUNC *WdfFunctions_01033 = NULL;

BOOLEAN WdfClientVersionHigherThanFramework = FALSE;
ULONG WdfFunctionCount = WdfFunctionTableNumEntries;
ULONG WdfStructureCount = 0;
WDF_STRUCT_INFO WdfStructures = NULL;

static STUB_WDF_BIND_INFO2 WdfBindInfo = {
    {sizeof(STUB_WDF_BIND_INFO2), L"KmdfLibrary", {1, 33, 0}, WdfFunctionTableNumEntries, &WdfFunctions_01033, NULL},
    &WdfMinimumVersionRequired, &WdfClientVersionHigherThanFramework, &WdfFunctionCount, &WdfStructureCount, &WdfStructures};

static UNICODE_STRING g_registry_path;
static WCHAR g_registry_buffer[260];
static PDRIVER_UNLOAD g_original_unload;

DRIVER_INITIALIZE DriverEntry;   /* the driver's own */

static VOID NTAPI FxDriverUnload(PDRIVER_OBJECT drv) {
    if (g_original_unload) g_original_unload(drv);
    WdfVersionUnbind(&g_registry_path, &WdfBindInfo, WdfDriverGlobals);
}

NTSTATUS NTAPI FxDriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING registry_path) {
    NTSTATUS st;
    USHORT n = registry_path->Length;
    if (n > sizeof(g_registry_buffer) - sizeof(WCHAR)) n = sizeof(g_registry_buffer) - sizeof(WCHAR);
    RtlCopyMemory(g_registry_buffer, registry_path->Buffer, n);
    g_registry_path.Buffer = g_registry_buffer;
    g_registry_path.Length = n;
    g_registry_path.MaximumLength = sizeof(g_registry_buffer);

    st = WdfVersionBind(drv, &g_registry_path, &WdfBindInfo, &WdfDriverGlobals);
    if (!NT_SUCCESS(st)) return st;
    st = DriverEntry(drv, registry_path);
    if (!NT_SUCCESS(st)) {
        WdfVersionUnbind(&g_registry_path, &WdfBindInfo, WdfDriverGlobals);
        return st;
    }
    if (WdfDriverGlobals->DisplaceDriverUnload) {
        g_original_unload = drv->DriverUnload;
        drv->DriverUnload = FxDriverUnload;
    }
    return st;
}
