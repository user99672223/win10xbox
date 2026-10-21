/*
 * wdf_types.h - the subset of the KMDF 1.33 public structures the WDF shim reads or writes.
 * MIT. Field layout is Microsoft's (third_party/Windows-Driver-Frameworks/src/publicinc/wdf/kmdf/1.33);
 * tests/drivers/layout_check compares every structure below against the real headers.
 * Callback fields are untyped pointers here: the shim casts them where it calls them.
 */
#ifndef WIN10XBOX_WDF_TYPES_H
#define WIN10XBOX_WDF_TYPES_H

#include <stddef.h>
#include "ntk/ntk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID WDFOBJECT;
typedef PVOID WDFDRIVER, WDFDEVICE, WDFQUEUE, WDFREQUEST, WDFFILEOBJECT, WDFMEMORY, WDFSPINLOCK, WDFWAITLOCK;
typedef struct _WDFDEVICE_INIT *PWDFDEVICE_INIT;

#define WDF_DRIVER_GLOBALS_NAME_LEN 32
typedef struct _WDF_DRIVER_GLOBALS {
    WDFDRIVER Driver;
    ULONG DriverFlags;
    ULONG DriverTag;
    CHAR DriverName[WDF_DRIVER_GLOBALS_NAME_LEN];
    BOOLEAN DisplaceDriverUnload;
} WDF_DRIVER_GLOBALS, *PWDF_DRIVER_GLOBALS;

struct _WDF_OBJECT_CONTEXT_TYPE_INFO;
typedef const struct _WDF_OBJECT_CONTEXT_TYPE_INFO *PCWDF_OBJECT_CONTEXT_TYPE_INFO;
typedef struct _WDF_OBJECT_CONTEXT_TYPE_INFO {
    ULONG Size;
    const char *ContextName;
    size_t ContextSize;
    PCWDF_OBJECT_CONTEXT_TYPE_INFO UniqueType;
    PVOID EvtDriverGetUniqueContextType;
} WDF_OBJECT_CONTEXT_TYPE_INFO;

typedef struct _WDF_OBJECT_ATTRIBUTES {
    ULONG Size;
    PVOID EvtCleanupCallback;      /* VOID (NTAPI*)(WDFOBJECT) */
    PVOID EvtDestroyCallback;      /* VOID (NTAPI*)(WDFOBJECT) */
    int ExecutionLevel;
    int SynchronizationScope;
    WDFOBJECT ParentObject;
    size_t ContextSizeOverride;
    PCWDF_OBJECT_CONTEXT_TYPE_INFO ContextTypeInfo;
} WDF_OBJECT_ATTRIBUTES, *PWDF_OBJECT_ATTRIBUTES;

typedef struct _WDF_DRIVER_CONFIG {
    ULONG Size;
    PVOID EvtDriverDeviceAdd;      /* NTSTATUS (NTAPI*)(WDFDRIVER, PWDFDEVICE_INIT) */
    PVOID EvtDriverUnload;         /* VOID (NTAPI*)(WDFDRIVER) */
    ULONG DriverInitFlags;
    ULONG DriverPoolTag;
} WDF_DRIVER_CONFIG, *PWDF_DRIVER_CONFIG;
#define WDF_DRIVER_INIT_NON_PNP_DRIVER 0x1
#define WDF_DRIVER_INIT_NO_DISPATCH_OVERRIDE 0x2

typedef struct _WDF_FILEOBJECT_CONFIG {
    ULONG Size;
    PVOID EvtDeviceFileCreate;     /* VOID (NTAPI*)(WDFDEVICE, WDFREQUEST, WDFFILEOBJECT) */
    PVOID EvtFileClose;            /* VOID (NTAPI*)(WDFFILEOBJECT) */
    PVOID EvtFileCleanup;          /* VOID (NTAPI*)(WDFFILEOBJECT) */
    int AutoForwardCleanupClose;
    int FileObjectClass;
} WDF_FILEOBJECT_CONFIG, *PWDF_FILEOBJECT_CONFIG;

typedef struct _WDF_IO_QUEUE_CONFIG {
    ULONG Size;
    int DispatchType;              /* 1 sequential, 2 parallel, 3 manual */
    int PowerManaged;
    BOOLEAN AllowZeroLengthRequests;
    BOOLEAN DefaultQueue;
    PVOID EvtIoDefault;            /* VOID (NTAPI*)(WDFQUEUE, WDFREQUEST) */
    PVOID EvtIoRead;               /* VOID (NTAPI*)(WDFQUEUE, WDFREQUEST, size_t Length) */
    PVOID EvtIoWrite;              /* VOID (NTAPI*)(WDFQUEUE, WDFREQUEST, size_t Length) */
    PVOID EvtIoDeviceControl;      /* VOID (NTAPI*)(WDFQUEUE, WDFREQUEST, size_t Out, size_t In, ULONG Code) */
    PVOID EvtIoInternalDeviceControl;
    PVOID EvtIoStop;
    PVOID EvtIoResume;
    PVOID EvtIoCanceledOnQueue;
    union {
        struct {
            ULONG NumberOfPresentedRequests;
        } Parallel;
    } Settings;
    WDFDRIVER Driver;
} WDF_IO_QUEUE_CONFIG, *PWDF_IO_QUEUE_CONFIG;
#define WDF_IO_QUEUE_DISPATCH_SEQUENTIAL 1
#define WDF_IO_QUEUE_DISPATCH_PARALLEL 2
#define WDF_IO_QUEUE_DISPATCH_MANUAL 3

#define WDF_REQUEST_TYPE_CREATE 0x0
#define WDF_REQUEST_TYPE_CLOSE 0x2
#define WDF_REQUEST_TYPE_READ 0x3
#define WDF_REQUEST_TYPE_WRITE 0x4
#define WDF_REQUEST_TYPE_DEVICE_CONTROL 0xe
#define WDF_REQUEST_TYPE_DEVICE_CONTROL_INTERNAL 0xf
#define WDF_REQUEST_TYPE_CLEANUP 0x12

typedef struct _WDF_REQUEST_PARAMETERS {
    USHORT Size;
    UCHAR MinorFunction;
    int Type;
    union {
        struct {
            PIO_SECURITY_CONTEXT SecurityContext;
            ULONG Options;
            POINTER_ALIGNMENT USHORT FileAttributes;
            USHORT ShareAccess;
            POINTER_ALIGNMENT ULONG EaLength;
        } Create;
        struct {
            size_t Length;
            POINTER_ALIGNMENT ULONG Key;
            LONGLONG DeviceOffset;
        } Read;
        struct {
            size_t Length;
            POINTER_ALIGNMENT ULONG Key;
            LONGLONG DeviceOffset;
        } Write;
        struct {
            size_t OutputBufferLength;
            POINTER_ALIGNMENT size_t InputBufferLength;
            POINTER_ALIGNMENT ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct {
            PVOID Arg1;
            PVOID Arg2;
            POINTER_ALIGNMENT ULONG IoControlCode;
            PVOID Arg4;
        } Others;
    } Parameters;
} WDF_REQUEST_PARAMETERS, *PWDF_REQUEST_PARAMETERS;

/* WDFLDR bind protocol (src/framework/shared/inc/private/common/fxldr.h) */
typedef struct _WDF_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Build;
} WDF_VERSION;

typedef struct _WDF_BIND_INFO {
    ULONG Size;
    PWCHAR Component;
    WDF_VERSION Version;
    ULONG FuncCount;
    PVOID *FuncTable;              /* arrives pointing at the driver's `WdfFunctions` variable */
    PVOID Module;
} WDF_BIND_INFO, *PWDF_BIND_INFO;

typedef struct _WDF_BIND_INFO2 {
    WDF_BIND_INFO V1;
    ULONG *MinimumVersionRequired;
    BOOLEAN *ClientVersionHigherThanFramework;
    ULONG *FuncCountPtr;
    ULONG *StructCountPtr;
    PVOID StructTable;
} WDF_BIND_INFO2, *PWDF_BIND_INFO2;

#ifdef __cplusplus
}
#endif
#endif
