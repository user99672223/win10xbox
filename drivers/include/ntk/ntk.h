/*
 * ntk.h - Windows kernel-mode (WDM) types and constants for the win10xbox
 * driver host. MIT licensed, written from the documented Windows 10 x64
 * layouts (verified against the mingw-w64 ddk headers by
 * tests/drivers/layout_check.c; the offsets below are what real drivers
 * compiled against the WDK expect).
 *
 * One header serves three consumers:
 *   - kernel-mode drivers built for the host (the test drivers): include it
 *     alone, no <windows.h>;
 *   - the host-side ntoskrnl implementation (C++): include <windows.h> and
 *     <winternl.h> first, then this header, with NTK_NO_API_DECLS defined
 *     (the API is implemented, not imported);
 *   - tools/tests that only need a layout.
 * x64 only.
 */
#ifndef WIN10XBOX_NTK_H
#define WIN10XBOX_NTK_H

#ifdef _MSC_VER
#define NTK_ALIGN(n) __declspec(align(n))
#else
#define NTK_ALIGN(n) __attribute__((aligned(n)))
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201 4200 4324)   /* nameless struct/union, zero-sized array, padded */
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* ---- basic types (only when <windows.h> is absent) ------------------------ */
#ifndef _WINDOWS_
typedef void VOID, *PVOID;
typedef const void *PCVOID;
typedef unsigned char UCHAR, BOOLEAN, *PUCHAR, *PBOOLEAN, BYTE;
typedef char CHAR, CCHAR, *PCHAR;
typedef unsigned short USHORT, *PUSHORT, WCHAR, *PWCHAR, *PWSTR;
typedef const WCHAR *PCWSTR;
typedef const char *PCSTR, *PCCHAR;
typedef short SHORT;
typedef int INT, *PINT;
typedef unsigned int UINT;
typedef int LONG, *PLONG;               /* LLP64: LONG is 32 bits */
typedef unsigned int ULONG, *PULONG, DWORD;
typedef long long LONGLONG, LONG64, *PLONGLONG, INT64;
typedef unsigned long long ULONGLONG, ULONG64, *PULONGLONG, UINT64, ULONG_PTR, SIZE_T, *PSIZE_T, *PULONG_PTR, DWORD_PTR;
typedef long long LONG_PTR, SSIZE_T, INT_PTR;
typedef void *HANDLE, **PHANDLE;
typedef int BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#define CONST const
#define IN
#define OUT
#define OPTIONAL
#define NTAPI
#define WINAPI
#define FORCEINLINE static inline
#define DECLSPEC_NORETURN
typedef LONG NTSTATUS, *PNTSTATUS;
typedef ULONG ACCESS_MASK;
typedef union _LARGE_INTEGER { struct { ULONG LowPart; LONG HighPart; } u; LONGLONG QuadPart; struct { ULONG LowPart; LONG HighPart; }; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef union _ULARGE_INTEGER { struct { ULONG LowPart; ULONG HighPart; } u; ULONGLONG QuadPart; struct { ULONG LowPart; ULONG HighPart; }; } ULARGE_INTEGER, *PULARGE_INTEGER;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
typedef struct _SINGLE_LIST_ENTRY { struct _SINGLE_LIST_ENTRY *Next; } SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;
typedef struct _GUID { ULONG Data1; USHORT Data2; USHORT Data3; UCHAR Data4[8]; } GUID, *PGUID;
typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef struct _STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } STRING, ANSI_STRING, OEM_STRING, *PSTRING, *PANSI_STRING;
typedef const STRING *PCANSI_STRING;
typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName; ULONG Attributes;
    PVOID SecurityDescriptor; PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _IO_STATUS_BLOCK { union { NTSTATUS Status; PVOID Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef struct NTK_ALIGN(16) _SLIST_HEADER { ULONGLONG Alignment; ULONGLONG Region; } SLIST_HEADER, *PSLIST_HEADER;
typedef struct _SLIST_ENTRY { struct _SLIST_ENTRY *Next; } SLIST_ENTRY, *PSLIST_ENTRY;
typedef struct _EXCEPTION_RECORD EXCEPTION_RECORD;
#define OBJ_INHERIT 0x2
#define OBJ_PERMANENT 0x10
#define OBJ_EXCLUSIVE 0x20
#define OBJ_CASE_INSENSITIVE 0x40
#define OBJ_OPENIF 0x80
#define OBJ_KERNEL_HANDLE 0x200
#ifndef InitializeObjectAttributes
#define InitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); (p)->RootDirectory = (r); (p)->Attributes = (a); \
    (p)->ObjectName = (n); (p)->SecurityDescriptor = (s); (p)->SecurityQualityOfService = NULL; }
#endif
#define MAXUSHORT 0xffff
#define MAXULONG 0xffffffffu
#ifndef CONTAINING_RECORD
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - (ULONG_PTR)(&((type *)0)->field)))
#endif
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)(LONG_PTR) & (((type *)0)->field))
#endif
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define ARRAYSIZE(a) RTL_NUMBER_OF(a)
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define DECLARE_CONST_UNICODE_STRING(n, s) \
    static const WCHAR n##_buffer[] = s; static const UNICODE_STRING n = {sizeof(s) - sizeof(WCHAR), sizeof(s), (PWSTR)n##_buffer}
#ifdef _MSC_VER
long _InterlockedIncrement(long volatile *);
long _InterlockedDecrement(long volatile *);
void *_InterlockedExchangePointer(void *volatile *, void *);
#pragma intrinsic(_InterlockedIncrement, _InterlockedDecrement, _InterlockedExchangePointer)
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchangePointer _InterlockedExchangePointer
#else
FORCEINLINE LONG InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
FORCEINLINE LONG InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
FORCEINLINE PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __sync_lock_test_and_set(p, v); }
#endif
#define FILE_ANY_ACCESS 0
#define FILE_SPECIAL_ACCESS FILE_ANY_ACCESS
#define FILE_READ_ACCESS 0x0001
#define FILE_WRITE_ACCESS 0x0002
#define METHOD_BUFFERED 0
#define METHOD_IN_DIRECT 1
#define METHOD_OUT_DIRECT 2
#define METHOD_NEITHER 3
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define DEVICE_TYPE_FROM_CTL_CODE(c) (((ULONG)(c) & 0xffff0000) >> 16)
#define FILE_DEVICE_UNKNOWN 0x00000022
#define FILE_DEVICE_FILE_SYSTEM 0x00000009
#define FILE_DEVICE_NETWORK 0x00000012
#define FILE_DEVICE_DISK 0x00000007
#define FILE_DEVICE_KEYBOARD 0x0000000b
#define FILE_DEVICE_SERIAL_PORT 0x0000001b
#define FILE_DEVICE_BEEP 0x00000001
#define FILE_DEVICE_NULL 0x00000015
#define FILE_DEVICE_CONTROLLER 0x00000004
#define FILE_DEVICE_SECURE_OPEN 0x00000100
#define FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL 0x00020000
#define FILE_OPENED 0x00000001
#define FILE_CREATED 0x00000002
#define FILE_SYNCHRONOUS_IO_ALERT 0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_SHARE_READ 0x1
#define FILE_SHARE_WRITE 0x2
#define FILE_SHARE_DELETE 0x4
#define FILE_OPEN 1
#define FILE_OPEN_IF 3
#define FILE_CREATE 2
#define SYNCHRONIZE 0x00100000
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define GENERIC_ALL 0x10000000
#else /* _WINDOWS_ */
#include <winternl.h>
#include <winioctl.h>
#endif /* _WINDOWS_ */
typedef short CSHORT;   /* identical redeclaration is legal in C11 and C++ */
#ifndef FILE_DEVICE_SECURE_OPEN
#define FILE_DEVICE_SECURE_OPEN 0x00000100
#endif
#ifndef FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL
#define FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL 0x00020000
#endif
#ifndef FILE_OPENED
#define FILE_OPENED 0x00000001
#define FILE_CREATED 0x00000002
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_ALERT 0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#endif

#ifndef POINTER_ALIGNMENT
#define POINTER_ALIGNMENT NTK_ALIGN(8)
#endif

/* Pool tags are written as 'Tag1' (little-endian) in the WDK; this is the portable spelling. */
#ifndef NTK_TAG
#define NTK_TAG(a, b, c, d) (((ULONG)(unsigned char)(a)) | ((ULONG)(unsigned char)(b) << 8) | ((ULONG)(unsigned char)(c) << 16) | ((ULONG)(unsigned char)(d) << 24))
#endif

/* ---- NTSTATUS values -------------------------------------------------------- */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_WAIT_0
#define STATUS_WAIT_0 ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif
#ifndef STATUS_ABANDONED_WAIT_0
#define STATUS_ABANDONED_WAIT_0 ((NTSTATUS)0x00000080L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_INVALID_INFO_CLASS
#define STATUS_INVALID_INFO_CLASS ((NTSTATUS)0xC0000003L)
#endif
#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#endif
#ifndef STATUS_INVALID_HANDLE
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_NO_SUCH_DEVICE
#define STATUS_NO_SUCH_DEVICE ((NTSTATUS)0xC000000EL)
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011L)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_OBJECT_TYPE_MISMATCH
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xC0000024L)
#endif
#ifndef STATUS_OBJECT_NAME_INVALID
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xC0000033L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#endif
#ifndef STATUS_MORE_PROCESSING_REQUIRED
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016L)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((NTSTATUS)0xC0000120L)
#endif
#ifndef STATUS_DELETE_PENDING
#define STATUS_DELETE_PENDING ((NTSTATUS)0xC0000056L)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif
#ifndef STATUS_DRIVER_ENTRYPOINT_NOT_FOUND
#define STATUS_DRIVER_ENTRYPOINT_NOT_FOUND ((NTSTATUS)0xC0000263L)
#endif
#ifndef STATUS_DEVICE_CONFIGURATION_ERROR
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182L)
#endif
#ifndef STATUS_INVALID_BUFFER_SIZE
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xC0000206L)
#endif
#ifndef STATUS_INVALID_DEVICE_STATE
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184L)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY ((NTSTATUS)0xC00000BAL)
#endif
#ifndef STATUS_ALREADY_REGISTERED
#define STATUS_ALREADY_REGISTERED ((NTSTATUS)0xC0000718L)
#endif
#ifndef STATUS_DEVICE_ALREADY_ATTACHED
#define STATUS_DEVICE_ALREADY_ATTACHED ((NTSTATUS)0xC0000038L)
#endif
#ifndef STATUS_REQUEST_ABORTED
#define STATUS_REQUEST_ABORTED ((NTSTATUS)0xC0000240L)
#endif
#ifndef STATUS_IO_DEVICE_ERROR
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185L)
#endif
#ifndef STATUS_BAD_INITIAL_PC
#define STATUS_BAD_INITIAL_PC ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_PROCEDURE_NOT_FOUND
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007AL)
#endif
#ifndef STATUS_IMAGE_ALREADY_LOADED
#define STATUS_IMAGE_ALREADY_LOADED ((NTSTATUS)0xC000010EL)
#endif
#ifndef STATUS_INVALID_IMAGE_FORMAT
#define STATUS_INVALID_IMAGE_FORMAT ((NTSTATUS)0xC000007BL)
#endif
#ifndef STATUS_NAME_TOO_LONG
#define STATUS_NAME_TOO_LONG ((NTSTATUS)0xC0000106L)
#endif
#ifndef STATUS_DRIVER_UNABLE_TO_LOAD
#define STATUS_DRIVER_UNABLE_TO_LOAD ((NTSTATUS)0xC000026CL)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#ifndef NT_ERROR
#define NT_ERROR(s) ((((ULONG)(s)) >> 30) == 3)
#endif

/* ---- list helpers (wdm.h inline equivalents) ------------------------------------ */
#ifndef NTK_NO_LIST_HELPERS
FORCEINLINE void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
FORCEINLINE BOOLEAN IsListEmpty(const LIST_ENTRY *h) { return (BOOLEAN)(h->Flink == h); }
FORCEINLINE BOOLEAN RemoveEntryList(PLIST_ENTRY e) {
    PLIST_ENTRY b = e->Blink, f = e->Flink;
    b->Flink = f; f->Blink = b;
    return (BOOLEAN)(f == b);
}
FORCEINLINE PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h) {
    PLIST_ENTRY e = h->Flink, f = e->Flink;
    h->Flink = f; f->Blink = h;
    return e;
}
FORCEINLINE PLIST_ENTRY RemoveTailList(PLIST_ENTRY h) {
    PLIST_ENTRY e = h->Blink, b = e->Blink;
    h->Blink = b; b->Flink = h;
    return e;
}
FORCEINLINE void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e) {
    PLIST_ENTRY b = h->Blink;
    e->Flink = h; e->Blink = b; b->Flink = e; h->Blink = e;
}
FORCEINLINE void InsertHeadList(PLIST_ENTRY h, PLIST_ENTRY e) {
    PLIST_ENTRY f = h->Flink;
    e->Flink = f; e->Blink = h; f->Blink = e; h->Flink = e;
}
FORCEINLINE void AppendTailList(PLIST_ENTRY h, PLIST_ENTRY l) {
    PLIST_ENTRY lt = l->Blink, ht = h->Blink;
    ht->Flink = l; l->Blink = ht; h->Blink = lt; lt->Flink = h;
}
FORCEINLINE void PushEntryList(PSINGLE_LIST_ENTRY h, PSINGLE_LIST_ENTRY e) { e->Next = h->Next; h->Next = e; }
FORCEINLINE PSINGLE_LIST_ENTRY PopEntryList(PSINGLE_LIST_ENTRY h) {
    PSINGLE_LIST_ENTRY f = h->Next;
    if (f) h->Next = f->Next;
    return f;
}
#endif

/* ---- scalar kernel types --------------------------------------------------------- */
typedef UCHAR KIRQL, *PKIRQL;
typedef CCHAR KPROCESSOR_MODE;
typedef ULONG_PTR KSPIN_LOCK, *PKSPIN_LOCK;
typedef ULONG_PTR KAFFINITY;
typedef LONG KPRIORITY;
typedef PVOID PEPROCESS, PETHREAD, PKTHREAD, PKPROCESS;

enum { KernelMode = 0, UserMode = 1, MaximumMode = 2 };

#define PASSIVE_LEVEL 0
#define LOW_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define CMCI_LEVEL 5
#define CLOCK_LEVEL 13
#define IPI_LEVEL 14
#define DRS_LEVEL 14
#define POWER_LEVEL 14
#define PROFILE_LEVEL 15
#define HIGH_LEVEL 15
#define SYNCH_LEVEL DISPATCH_LEVEL

/* ---- dispatcher objects ------------------------------------------------------------ */
typedef enum _EVENT_TYPE { NotificationEvent = 0, SynchronizationEvent = 1 } EVENT_TYPE;
typedef enum _TIMER_TYPE_NTK { NotificationTimer = 0, SynchronizationTimer = 1 } TIMER_TYPE;
typedef enum _WAIT_TYPE_NTK { WaitAll = 0, WaitAny = 1, WaitNotification = 2, WaitDequeue = 3, WaitDpc = 4 } WAIT_TYPE;
#ifndef _WINDOWS_
typedef enum _KWAIT_REASON { Executive = 0, UserRequest = 6, DelayExecution = 4 } KWAIT_REASON;
#endif

/* DISPATCHER_HEADER.Type values. */
#define NTK_TYPE_NOTIFICATION_EVENT 0
#define NTK_TYPE_SYNCHRONIZATION_EVENT 1
#define NTK_TYPE_MUTANT 2
#define NTK_TYPE_PROCESS 3
#define NTK_TYPE_QUEUE 4
#define NTK_TYPE_SEMAPHORE 5
#define NTK_TYPE_THREAD 6
#define NTK_TYPE_NOTIFICATION_TIMER 8
#define NTK_TYPE_SYNCHRONIZATION_TIMER 9

typedef struct _DISPATCHER_HEADER {
    UCHAR Type;
    UCHAR Absolute;           /* Absolute / TimerControlFlags */
    UCHAR Size;
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct _KEVENT { DISPATCHER_HEADER Header; } KEVENT, *PKEVENT, *PRKEVENT;
typedef struct _KSEMAPHORE { DISPATCHER_HEADER Header; LONG Limit; } KSEMAPHORE, *PKSEMAPHORE, *PRKSEMAPHORE;
typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;
    LIST_ENTRY MutantListEntry;
    PKTHREAD OwnerThread;
    UCHAR Abandoned;
    UCHAR ApcDisable;
} KMUTANT, *PKMUTANT, KMUTEX, *PKMUTEX, *PRKMUTEX;
struct _KDPC;
typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    ULARGE_INTEGER DueTime;
    LIST_ENTRY TimerListEntry;
    struct _KDPC *Dpc;
    USHORT Processor;
    USHORT TimerType;
    ULONG Period;
} KTIMER, *PKTIMER, *PRKTIMER;

typedef VOID (NTAPI *PKDEFERRED_ROUTINE)(struct _KDPC *Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);
typedef struct _KDPC {
    UCHAR Type;
    UCHAR Importance;
    volatile USHORT Number;
    LIST_ENTRY DpcListEntry;
    PKDEFERRED_ROUTINE DeferredRoutine;
    PVOID DeferredContext;
    PVOID SystemArgument1;
    PVOID SystemArgument2;
    volatile PVOID DpcData;
} KDPC, *PKDPC, *PRKDPC;

typedef struct _KSPIN_LOCK_QUEUE {
    struct _KSPIN_LOCK_QUEUE *volatile Next;
    PKSPIN_LOCK volatile Lock;
} KSPIN_LOCK_QUEUE, *PKSPIN_LOCK_QUEUE;
typedef struct _KLOCK_QUEUE_HANDLE { KSPIN_LOCK_QUEUE LockQueue; KIRQL OldIrql; } KLOCK_QUEUE_HANDLE, *PKLOCK_QUEUE_HANDLE;

typedef struct _FAST_MUTEX {
    LONG Count;
    PKTHREAD Owner;
    ULONG Contention;
    KEVENT Event;
    ULONG OldIrql;
} FAST_MUTEX, *PFAST_MUTEX, KGUARDED_MUTEX, *PKGUARDED_MUTEX;

typedef struct _ERESOURCE_NTK {
    LIST_ENTRY SystemResourcesList;
    PVOID OwnerTable;                 /* host: points to the implementation state */
    SHORT ActiveCount;
    USHORT Flag;
    PVOID SharedWaiters;
    PVOID ExclusiveWaiters;
    UCHAR OwnerEntry[16];
    ULONG ActiveEntries;
    ULONG ContentionCount;
    ULONG NumberOfSharedWaiters;
    ULONG NumberOfExclusiveWaiters;
    PVOID Reserved2;
    PVOID Address;
    KSPIN_LOCK SpinLock;
} ERESOURCE_NTK;
typedef ERESOURCE_NTK ERESOURCE, *PERESOURCE;

typedef struct _WORK_QUEUE_ITEM {
    LIST_ENTRY List;
    VOID (NTAPI *WorkerRoutine)(PVOID Parameter);
    PVOID Parameter;
} WORK_QUEUE_ITEM, *PWORK_QUEUE_ITEM;
typedef enum _WORK_QUEUE_TYPE { CriticalWorkQueue = 0, DelayedWorkQueue = 1, HyperCriticalWorkQueue = 2 } WORK_QUEUE_TYPE;

typedef PVOID (NTAPI *PALLOCATE_FUNCTION)(int PoolType, SIZE_T NumberOfBytes, ULONG Tag);
typedef VOID (NTAPI *PFREE_FUNCTION)(PVOID Buffer);
typedef struct _GENERAL_LOOKASIDE {
    SLIST_HEADER ListHead;
    USHORT Depth;
    USHORT MaximumDepth;
    ULONG TotalAllocates;
    ULONG AllocateMisses;
    ULONG TotalFrees;
    ULONG FreeMisses;
    int Type;
    ULONG Tag;
    ULONG Size;
    PALLOCATE_FUNCTION Allocate;
    PFREE_FUNCTION Free;
    LIST_ENTRY ListEntry;
    ULONG LastTotalAllocates;
    ULONG LastAllocateMisses;
    ULONG Future[2];
} GENERAL_LOOKASIDE, *PGENERAL_LOOKASIDE;
typedef struct _NPAGED_LOOKASIDE_LIST { GENERAL_LOOKASIDE L; } NPAGED_LOOKASIDE_LIST, *PNPAGED_LOOKASIDE_LIST;
typedef struct _PAGED_LOOKASIDE_LIST { GENERAL_LOOKASIDE L; } PAGED_LOOKASIDE_LIST, *PPAGED_LOOKASIDE_LIST;

/* ---- pool ------------------------------------------------------------------------------ */
typedef enum _POOL_TYPE {
    NonPagedPool = 0,
    PagedPool = 1,
    NonPagedPoolMustSucceed = 2,
    DontUseThisType = 3,
    NonPagedPoolCacheAligned = 4,
    PagedPoolCacheAligned = 5,
    NonPagedPoolNx = 512,
    NonPagedPoolNxCacheAligned = 516
} POOL_TYPE;
typedef ULONG64 POOL_FLAGS;
#define POOL_FLAG_REQUIRED_START 0x1ull
#define POOL_FLAG_UNINITIALIZED 0x2ull
#define POOL_FLAG_RAISE_ON_FAILURE 0x20ull
#define POOL_FLAG_NON_PAGED 0x40ull
#define POOL_FLAG_NON_PAGED_EXECUTE 0x80ull
#define POOL_FLAG_PAGED 0x100ull
#define POOL_FLAG_CACHE_ALIGNED 0x8ull

/* ---- MDL ------------------------------------------------------------------------------------ */
typedef struct _MDL {
    struct _MDL *Next;
    CSHORT Size;
    CSHORT MdlFlags;
    PVOID Process;
    PVOID MappedSystemVa;
    PVOID StartVa;
    ULONG ByteCount;
    ULONG ByteOffset;
} MDL, *PMDL;
typedef ULONG_PTR PFN_NUMBER, *PPFN_NUMBER;
#define MDL_MAPPED_TO_SYSTEM_VA 0x0001
#define MDL_PAGES_LOCKED 0x0002
#define MDL_SOURCE_IS_NONPAGED_POOL 0x0004
#define MDL_ALLOCATED_FIXED_SIZE 0x0008
#define MDL_PARTIAL 0x0010
#define MDL_PARTIAL_HAS_BEEN_MAPPED 0x0020
#define MDL_IO_PAGE_READ 0x0040
#define MDL_WRITE_OPERATION 0x0080
#define MDL_PARENT_MAPPED_SYSTEM_VA 0x0100
#define MDL_FREE_EXTRA_PTES 0x0200
#define MDL_IO_SPACE 0x0800
#define MDL_NETWORK_HEADER 0x1000
#define MDL_MAPPING_CAN_FAIL 0x2000
#define MDL_ALLOCATED_MUST_SUCCEED 0x4000
#define MDL_INTERNAL 0x8000
#define PAGE_SIZE 0x1000
#define PAGE_SHIFT 12
#define BYTE_OFFSET(va) ((ULONG)((ULONG_PTR)(va) & (PAGE_SIZE - 1)))
#define PAGE_ALIGN(va) ((PVOID)((ULONG_PTR)(va) & ~((ULONG_PTR)PAGE_SIZE - 1)))
#define ADDRESS_AND_SIZE_TO_SPAN_PAGES(va, size) \
    ((ULONG)((((ULONG_PTR)(size)) + BYTE_OFFSET(va) + PAGE_SIZE - 1) >> PAGE_SHIFT))
typedef enum _MM_PAGE_PRIORITY { LowPagePriority = 0, NormalPagePriority = 16, HighPagePriority = 32 } MM_PAGE_PRIORITY;
typedef enum _LOCK_OPERATION { IoReadAccess = 0, IoWriteAccess = 1, IoModifyAccess = 2 } LOCK_OPERATION;
typedef enum _MEMORY_CACHING_TYPE { MmNonCached = 0, MmCached = 1, MmWriteCombined = 2 } MEMORY_CACHING_TYPE;

/* ---- device / driver / file objects ----------------------------------------------------------- */
struct _DEVICE_OBJECT;
struct _DRIVER_OBJECT;
struct _IRP;
struct _FILE_OBJECT;
struct _IO_STACK_LOCATION;

typedef struct _KDEVICE_QUEUE {
    CSHORT Type;
    CSHORT Size;
    LIST_ENTRY DeviceListHead;
    KSPIN_LOCK Lock;
    BOOLEAN Busy;
} KDEVICE_QUEUE, *PKDEVICE_QUEUE;
typedef struct _KDEVICE_QUEUE_ENTRY {
    LIST_ENTRY DeviceListEntry;
    ULONG SortKey;
    BOOLEAN Inserted;
} KDEVICE_QUEUE_ENTRY, *PKDEVICE_QUEUE_ENTRY;

typedef struct _WAIT_CONTEXT_BLOCK {
    KDEVICE_QUEUE_ENTRY WaitQueueEntry;
    PVOID DeviceRoutine;
    PVOID DeviceContext;
    ULONG NumberOfMapRegisters;
    PVOID DeviceObject;
    PVOID CurrentIrp;
    PKDPC BufferChainingDpc;
} WAIT_CONTEXT_BLOCK, *PWAIT_CONTEXT_BLOCK;

typedef struct _DEVICE_OBJECT {
    CSHORT Type;
    USHORT Size;
    LONG ReferenceCount;
    struct _DRIVER_OBJECT *DriverObject;
    struct _DEVICE_OBJECT *NextDevice;
    struct _DEVICE_OBJECT *AttachedDevice;
    struct _IRP *CurrentIrp;
    PVOID Timer;
    ULONG Flags;
    ULONG Characteristics;
    PVOID Vpb;
    PVOID DeviceExtension;
    ULONG DeviceType;
    CCHAR StackSize;
    union { LIST_ENTRY ListEntry; WAIT_CONTEXT_BLOCK Wcb; } Queue;
    ULONG AlignmentRequirement;
    KDEVICE_QUEUE DeviceQueue;
    KDPC Dpc;
    ULONG ActiveThreadCount;
    PVOID SecurityDescriptor;
    KEVENT DeviceLock;
    USHORT SectorSize;
    USHORT Spare1;
    PVOID DeviceObjectExtension;
    PVOID Reserved;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _FILE_OBJECT {
    CSHORT Type;
    CSHORT Size;
    PDEVICE_OBJECT DeviceObject;
    PVOID Vpb;
    PVOID FsContext;
    PVOID FsContext2;
    PVOID SectionObjectPointer;
    PVOID PrivateCacheMap;
    NTSTATUS FinalStatus;
    struct _FILE_OBJECT *RelatedFileObject;
    BOOLEAN LockOperation;
    BOOLEAN DeletePending;
    BOOLEAN ReadAccess;
    BOOLEAN WriteAccess;
    BOOLEAN DeleteAccess;
    BOOLEAN SharedRead;
    BOOLEAN SharedWrite;
    BOOLEAN SharedDelete;
    ULONG Flags;
    UNICODE_STRING FileName;
    LARGE_INTEGER CurrentByteOffset;
    volatile ULONG Waiters;
    volatile ULONG Busy;
    PVOID LastLock;
    KEVENT Lock;
    KEVENT Event;
    PVOID CompletionContext;
    KSPIN_LOCK IrpListLock;
    LIST_ENTRY IrpList;
    PVOID FileObjectExtension;
} FILE_OBJECT, *PFILE_OBJECT;
#define FO_FILE_OPEN 0x00000001
#define FO_SYNCHRONOUS_IO 0x00000002
#define FO_ALERTABLE_IO 0x00000004
#define FO_NO_INTERMEDIATE_BUFFERING 0x00000008
#define FO_HANDLE_CREATED 0x00000010
#define FO_CLEANUP_COMPLETE 0x00004000

typedef struct _DRIVER_EXTENSION {
    struct _DRIVER_OBJECT *DriverObject;
    NTSTATUS (NTAPI *AddDevice)(struct _DRIVER_OBJECT *DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);
    ULONG Count;
    UNICODE_STRING ServiceKeyName;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

typedef NTSTATUS (NTAPI DRIVER_INITIALIZE)(struct _DRIVER_OBJECT *DriverObject, PUNICODE_STRING RegistryPath);
typedef DRIVER_INITIALIZE *PDRIVER_INITIALIZE;
typedef VOID (NTAPI DRIVER_UNLOAD)(struct _DRIVER_OBJECT *DriverObject);
typedef DRIVER_UNLOAD *PDRIVER_UNLOAD;
typedef NTSTATUS (NTAPI DRIVER_DISPATCH)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp);
typedef DRIVER_DISPATCH *PDRIVER_DISPATCH;
typedef VOID (NTAPI DRIVER_STARTIO)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp);
typedef DRIVER_STARTIO *PDRIVER_STARTIO;
typedef NTSTATUS (NTAPI DRIVER_ADD_DEVICE)(struct _DRIVER_OBJECT *DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);
typedef DRIVER_ADD_DEVICE *PDRIVER_ADD_DEVICE;
typedef VOID (NTAPI DRIVER_CANCEL)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp);
typedef DRIVER_CANCEL *PDRIVER_CANCEL;
typedef NTSTATUS (NTAPI IO_COMPLETION_ROUTINE)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp, PVOID Context);
typedef IO_COMPLETION_ROUTINE *PIO_COMPLETION_ROUTINE;

#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CREATE_NAMED_PIPE 0x01
#define IRP_MJ_CLOSE 0x02
#define IRP_MJ_READ 0x03
#define IRP_MJ_WRITE 0x04
#define IRP_MJ_QUERY_INFORMATION 0x05
#define IRP_MJ_SET_INFORMATION 0x06
#define IRP_MJ_QUERY_EA 0x07
#define IRP_MJ_SET_EA 0x08
#define IRP_MJ_FLUSH_BUFFERS 0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0a
#define IRP_MJ_SET_VOLUME_INFORMATION 0x0b
#define IRP_MJ_DIRECTORY_CONTROL 0x0c
#define IRP_MJ_FILE_SYSTEM_CONTROL 0x0d
#define IRP_MJ_DEVICE_CONTROL 0x0e
#define IRP_MJ_INTERNAL_DEVICE_CONTROL 0x0f
#define IRP_MJ_SCSI IRP_MJ_INTERNAL_DEVICE_CONTROL
#define IRP_MJ_SHUTDOWN 0x10
#define IRP_MJ_LOCK_CONTROL 0x11
#define IRP_MJ_CLEANUP 0x12
#define IRP_MJ_CREATE_MAILSLOT 0x13
#define IRP_MJ_QUERY_SECURITY 0x14
#define IRP_MJ_SET_SECURITY 0x15
#define IRP_MJ_POWER 0x16
#define IRP_MJ_SYSTEM_CONTROL 0x17
#define IRP_MJ_DEVICE_CHANGE 0x18
#define IRP_MJ_QUERY_QUOTA 0x19
#define IRP_MJ_SET_QUOTA 0x1a
#define IRP_MJ_PNP 0x1b
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b

#define IRP_MN_USER_FS_REQUEST 0x00
#define IRP_MN_START_DEVICE 0x00
#define IRP_MN_QUERY_REMOVE_DEVICE 0x01
#define IRP_MN_REMOVE_DEVICE 0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE 0x03
#define IRP_MN_STOP_DEVICE 0x04
#define IRP_MN_SURPRISE_REMOVAL 0x17

#define DO_VERIFY_VOLUME 0x00000002
#define DO_BUFFERED_IO 0x00000004
#define DO_EXCLUSIVE 0x00000008
#define DO_DIRECT_IO 0x00000010
#define DO_MAP_IO_BUFFER 0x00000020
#define DO_DEVICE_HAS_NAME 0x00000040
#define DO_DEVICE_INITIALIZING 0x00000080
#define DO_SYSTEM_BOOT_PARTITION 0x00000100
#define DO_LONG_TERM_REQUESTS 0x00000200
#define DO_NEVER_LAST_DEVICE 0x00000400
#define DO_SHUTDOWN_REGISTERED 0x00000800
#define DO_BUS_ENUMERATED_DEVICE 0x00001000
#define DO_POWER_PAGABLE 0x00002000
#define DO_POWER_INRUSH 0x00004000
#define DRVO_UNLOAD_INVOKED 0x00000001
#define DRVO_LEGACY_DRIVER 0x00000002
#define DRVO_BUILTIN_DRIVER 0x00000004
#define DEVICE_OBJECT_TYPE_ID 3
#define DRIVER_OBJECT_TYPE_ID 4
#define FILE_OBJECT_TYPE_ID 5
#define IO_TYPE_IRP 6
#define IO_TYPE_DEVICE_OBJECT_EXT 13

typedef struct _DRIVER_OBJECT {
    CSHORT Type;
    CSHORT Size;
    PDEVICE_OBJECT DeviceObject;
    ULONG Flags;
    PVOID DriverStart;
    ULONG DriverSize;
    PVOID DriverSection;
    PDRIVER_EXTENSION DriverExtension;
    UNICODE_STRING DriverName;
    PUNICODE_STRING HardwareDatabase;
    PVOID FastIoDispatch;
    PDRIVER_INITIALIZE DriverInit;
    PDRIVER_STARTIO DriverStartIo;
    PDRIVER_UNLOAD DriverUnload;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _KLDR_DATA_TABLE_ENTRY_NTK {
    LIST_ENTRY InLoadOrderLinks;
    PVOID ExceptionTable;
    ULONG ExceptionTableSize;
    PVOID GpValue;
    PVOID NonPagedDebugInfo;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
    ULONG Flags;
    USHORT LoadCount;
    USHORT SignatureLevel;
    PVOID SectionPointer;
    ULONG CheckSum;
    ULONG CoverageSectionSize;
    PVOID CoverageSection;
    PVOID LoadedImports;
    PVOID Spare;
} KLDR_DATA_TABLE_ENTRY_NTK;

/* ---- IRP ---------------------------------------------------------------------------------------- */
#define IRP_NOCACHE 0x00000001
#define IRP_PAGING_IO 0x00000002
#define IRP_MOUNT_COMPLETION 0x00000002
#define IRP_SYNCHRONOUS_API 0x00000004
#define IRP_ASSOCIATED_IRP 0x00000008
#define IRP_BUFFERED_IO 0x00000010
#define IRP_DEALLOCATE_BUFFER 0x00000020
#define IRP_INPUT_OPERATION 0x00000040
#define IRP_SYNCHRONOUS_PAGING_IO 0x00000040
#define IRP_CREATE_OPERATION 0x00000080
#define IRP_READ_OPERATION 0x00000100
#define IRP_WRITE_OPERATION 0x00000200
#define IRP_CLOSE_OPERATION 0x00000400
#define IRP_DEFER_IO_COMPLETION 0x00000800

#define SL_PENDING_RETURNED 0x01
#define SL_ERROR_RETURNED 0x02
#define SL_INVOKE_ON_CANCEL 0x20
#define SL_INVOKE_ON_SUCCESS 0x40
#define SL_INVOKE_ON_ERROR 0x80
#define SL_FORCE_ACCESS_CHECK 0x01
#define SL_OPEN_PAGING_FILE 0x02
#define SL_OPEN_TARGET_DIRECTORY 0x04
#define SL_CASE_SENSITIVE 0x80

typedef struct _IO_SECURITY_CONTEXT {
    PVOID SecurityQos;
    PVOID AccessState;
    ACCESS_MASK DesiredAccess;
    ULONG FullCreateOptions;
} IO_SECURITY_CONTEXT, *PIO_SECURITY_CONTEXT;

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union {
        struct {
            PIO_SECURITY_CONTEXT SecurityContext;
            ULONG Options;
            POINTER_ALIGNMENT USHORT FileAttributes;
            USHORT ShareAccess;
            POINTER_ALIGNMENT ULONG EaLength;
        } Create;
        struct {
            ULONG Length;
            POINTER_ALIGNMENT ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Read;
        struct {
            ULONG Length;
            POINTER_ALIGNMENT ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Write;
        struct {
            ULONG Length;
            POINTER_ALIGNMENT ULONG FileInformationClass;
        } QueryFile;
        struct {
            ULONG Length;
            POINTER_ALIGNMENT ULONG FileInformationClass;
            PFILE_OBJECT FileObject;
        } SetFile;
        struct {
            ULONG OutputBufferLength;
            POINTER_ALIGNMENT ULONG InputBufferLength;
            POINTER_ALIGNMENT ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct {
            PVOID Argument1;
            PVOID Argument2;
            PVOID Argument3;
            PVOID Argument4;
        } Others;
    } Parameters;
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    CSHORT Type;
    USHORT Size;
    PMDL MdlAddress;
    ULONG Flags;
    union {
        struct _IRP *MasterIrp;
        volatile LONG IrpCount;
        PVOID SystemBuffer;
    } AssociatedIrp;
    LIST_ENTRY ThreadListEntry;
    IO_STATUS_BLOCK IoStatus;
    CCHAR RequestorMode;
    BOOLEAN PendingReturned;
    CHAR StackCount;
    CHAR CurrentLocation;
    BOOLEAN Cancel;
    KIRQL CancelIrql;
    CCHAR ApcEnvironment;
    UCHAR AllocationFlags;
    PIO_STATUS_BLOCK UserIosb;
    PKEVENT UserEvent;
    union {
        struct {
            union {
                PVOID UserApcRoutine;
                PVOID IssuingProcess;
            };
            PVOID UserApcContext;
        } AsynchronousParameters;
        LARGE_INTEGER AllocationSize;
    } Overlay;
    volatile PDRIVER_CANCEL CancelRoutine;
    PVOID UserBuffer;
    union {
        struct {
            union {
                KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
                struct {
                    PVOID DriverContext[4];
                };
            };
            PETHREAD Thread;
            PCHAR AuxiliaryBuffer;
            struct {
                LIST_ENTRY ListEntry;
                union {
                    struct _IO_STACK_LOCATION *CurrentStackLocation;
                    ULONG PacketType;
                };
            };
            PFILE_OBJECT OriginalFileObject;
            PVOID IrpExtension;
        } Overlay;
        unsigned char Apc[0x58];
        PVOID CompletionKey;
    } Tail;
} IRP, *PIRP;

#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->Tail.Overlay.CurrentStackLocation)
#define IoGetNextIrpStackLocation(Irp) ((Irp)->Tail.Overlay.CurrentStackLocation - 1)
#define IoSkipCurrentIrpStackLocation(Irp) \
    do { (Irp)->CurrentLocation++; (Irp)->Tail.Overlay.CurrentStackLocation++; } while (0)
#define IoMarkIrpPending(Irp) (IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED)
#define IoSizeOfIrp(StackSize) ((USHORT)(sizeof(IRP) + ((StackSize) * sizeof(IO_STACK_LOCATION))))
#define IoCopyCurrentIrpStackLocationToNext(Irp) \
    do { \
        PIO_STACK_LOCATION _cur = IoGetCurrentIrpStackLocation(Irp); \
        PIO_STACK_LOCATION _next = IoGetNextIrpStackLocation(Irp); \
        _next->MajorFunction = _cur->MajorFunction; _next->MinorFunction = _cur->MinorFunction; \
        _next->Flags = _cur->Flags; _next->Control = 0; \
        _next->Parameters = _cur->Parameters; _next->FileObject = _cur->FileObject; \
    } while (0)
#define IoSetCompletionRoutine(Irp, Routine, CompletionContext, Success, Error, Cancel) \
    do { \
        PIO_STACK_LOCATION _next = IoGetNextIrpStackLocation(Irp); \
        _next->CompletionRoutine = (Routine); _next->Context = (CompletionContext); \
        _next->Control = 0; \
        if (Success) _next->Control = SL_INVOKE_ON_SUCCESS; \
        if (Error) _next->Control |= SL_INVOKE_ON_ERROR; \
        if (Cancel) _next->Control |= SL_INVOKE_ON_CANCEL; \
    } while (0)
#define IoSetNextIrpStackLocation(Irp) \
    do { (Irp)->CurrentLocation--; (Irp)->Tail.Overlay.CurrentStackLocation--; } while (0)

#define IO_NO_INCREMENT 0
#define IO_DISK_INCREMENT 1
#define IO_KEYBOARD_INCREMENT 6
#define IO_NETWORK_INCREMENT 2

/* ---- Interrupts, miscellaneous ------------------------------------------------------------------------ */
typedef BOOLEAN (NTAPI KSERVICE_ROUTINE)(PVOID Interrupt, PVOID ServiceContext);
typedef KSERVICE_ROUTINE *PKSERVICE_ROUTINE;
typedef BOOLEAN (NTAPI KSYNCHRONIZE_ROUTINE)(PVOID SynchronizeContext);
typedef KSYNCHRONIZE_ROUTINE *PKSYNCHRONIZE_ROUTINE;
typedef enum _KINTERRUPT_MODE { LevelSensitive = 0, Latched = 1 } KINTERRUPT_MODE;
typedef struct _KINTERRUPT_NTK *PKINTERRUPT;

typedef struct _OSVERSIONINFOW_NTK {
    ULONG dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    WCHAR szCSDVersion[128];
} RTL_OSVERSIONINFOW_NTK;

typedef struct _TIME_FIELDS_NTK {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS_NTK;

/* DbgPrintEx levels */
#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 1
#define DPFLTR_TRACE_LEVEL 2
#define DPFLTR_INFO_LEVEL 3
#define DPFLTR_IHVDRIVER_ID 77

typedef VOID (NTAPI WORKER_THREAD_ROUTINE)(PVOID Parameter);
typedef WORKER_THREAD_ROUTINE *PWORKER_THREAD_ROUTINE;
typedef VOID (NTAPI IO_WORKITEM_ROUTINE)(PDEVICE_OBJECT DeviceObject, PVOID Context);
typedef IO_WORKITEM_ROUTINE *PIO_WORKITEM_ROUTINE;
typedef struct _IO_WORKITEM_NTK *PIO_WORKITEM;
typedef VOID (NTAPI KSTART_ROUTINE)(PVOID StartContext);
typedef KSTART_ROUTINE *PKSTART_ROUTINE;

/* Bug check codes drivers commonly use. */
#define DRIVER_IRQL_NOT_LESS_OR_EQUAL 0x000000D1
#define IRQL_NOT_LESS_OR_EQUAL 0x0000000A
#define IRQL_NOT_GREATER_OR_EQUAL 0x00000009
#define MANUALLY_INITIATED_CRASH 0x000000E2
#define KMODE_EXCEPTION_NOT_HANDLED 0x0000001E

/* ---- imported API (drivers only) ----------------------------------------------------------------------------
 * Declared with dllimport so a driver built against the import library
 * (ntoskrnl.exe) reaches the host through its IAT, resolved by the driver
 * host's import resolver. The host implementation defines NTK_NO_API_DECLS. */
#ifndef NTK_NO_API_DECLS
#ifdef _MSC_VER
#define NTKAPI __declspec(dllimport)
#else
#define NTKAPI __attribute__((dllimport))
#endif

NTKAPI NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, PUNICODE_STRING DeviceName,
                                     ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive,
                                     PDEVICE_OBJECT *DeviceObject);
NTKAPI VOID NTAPI IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
NTKAPI NTSTATUS NTAPI IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
NTKAPI NTSTATUS NTAPI IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);
NTKAPI PDEVICE_OBJECT NTAPI IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice);
NTKAPI VOID NTAPI IoDetachDevice(PDEVICE_OBJECT TargetDevice);
NTKAPI PIRP NTAPI IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
NTKAPI VOID NTAPI IoFreeIrp(PIRP Irp);
NTKAPI NTSTATUS NTAPI IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTKAPI VOID NTAPI IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
NTKAPI PIRP NTAPI IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject, PVOID InputBuffer,
                                                ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength,
                                                BOOLEAN InternalDeviceIoControl, PKEVENT Event,
                                                PIO_STATUS_BLOCK IoStatusBlock);
#define IoCallDriver(d, i) IofCallDriver(d, i)
#define IoCompleteRequest(i, b) IofCompleteRequest(i, b)

NTKAPI VOID NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
NTKAPI LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
NTKAPI LONG NTAPI KeResetEvent(PKEVENT Event);
NTKAPI NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                                            BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTKAPI KIRQL NTAPI KeGetCurrentIrql(void);
NTKAPI KIRQL NTAPI KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock);
NTKAPI VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
NTKAPI VOID NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
NTKAPI BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
NTKAPI VOID NTAPI KeInitializeTimer(PKTIMER Timer);
NTKAPI BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
NTKAPI BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer);

NTKAPI PVOID NTAPI ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
NTKAPI VOID NTAPI ExFreePoolWithTag(PVOID P, ULONG Tag);
NTKAPI VOID NTAPI ExFreePool(PVOID P);

NTKAPI VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString);
NTKAPI VOID NTAPI RtlCopyUnicodeString(PUNICODE_STRING DestinationString, PCUNICODE_STRING SourceString);
NTKAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress);
NTKAPI PVOID NTAPI MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName);
NTKAPI NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK DesiredAccess, PVOID ObjectType,
                                                KPROCESSOR_MODE AccessMode, PVOID *Object, PVOID HandleInformation);
NTKAPI LONG_PTR NTAPI ObfDereferenceObject(PVOID Object);
#define ObDereferenceObject(o) ObfDereferenceObject(o)
NTKAPI NTSTATUS NTAPI ZwClose(HANDLE Handle);
NTKAPI ULONG __cdecl DbgPrint(const char *Format, ...);
NTKAPI ULONG __cdecl DbgPrintEx(ULONG ComponentId, ULONG Level, const char *Format, ...);

NTKAPI VOID NTAPI KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit);
NTKAPI LONG NTAPI KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait);
NTKAPI VOID NTAPI KeInitializeMutex(PKMUTEX Mutex, ULONG Level);
NTKAPI LONG NTAPI KeReleaseMutex(PKMUTEX Mutex, BOOLEAN Wait);
NTKAPI VOID NTAPI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock);
NTKAPI VOID NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock);
NTKAPI KIRQL NTAPI KfRaiseIrql(KIRQL NewIrql);
NTKAPI VOID NTAPI KfLowerIrql(KIRQL NewIrql);
NTKAPI VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime);
NTKAPI VOID NTAPI KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type);
NTKAPI BOOLEAN NTAPI KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc);
NTKAPI VOID NTAPI KeStallExecutionProcessor(ULONG Microseconds);
NTKAPI NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
NTKAPI VOID NTAPI ExAcquireFastMutex(PFAST_MUTEX FastMutex);
NTKAPI VOID NTAPI ExReleaseFastMutex(PFAST_MUTEX FastMutex);
NTKAPI NTSTATUS NTAPI ExInitializeResourceLite(PERESOURCE Resource);
NTKAPI BOOLEAN NTAPI ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait);
NTKAPI BOOLEAN NTAPI ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait);
NTKAPI VOID NTAPI ExReleaseResourceLite(PERESOURCE Resource);
NTKAPI NTSTATUS NTAPI ExDeleteResourceLite(PERESOURCE Resource);
NTKAPI VOID NTAPI ExQueueWorkItem(PWORK_QUEUE_ITEM WorkItem, WORK_QUEUE_TYPE QueueType);
NTKAPI KPROCESSOR_MODE NTAPI ExGetPreviousMode(void);
NTKAPI PVOID NTAPI IoGetCurrentProcess(void);
NTKAPI HANDLE NTAPI PsGetCurrentProcessId(void);
NTKAPI HANDLE NTAPI PsGetCurrentThreadId(void);
NTKAPI PMDL NTAPI IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp);
NTKAPI VOID NTAPI IoFreeMdl(PMDL Mdl);
NTKAPI PVOID NTAPI MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode, MEMORY_CACHING_TYPE CacheType,
                                                PVOID RequestedAddress, ULONG BugCheckOnFailure, ULONG Priority);
NTKAPI VOID NTAPI ProbeForRead(PVOID Address, SIZE_T Length, ULONG Alignment);
NTKAPI VOID NTAPI ProbeForWrite(PVOID Address, SIZE_T Length, ULONG Alignment);
NTKAPI VOID NTAPI IoStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp, PULONG Key, PDRIVER_CANCEL CancelFunction);
NTKAPI VOID NTAPI IoStartNextPacket(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable);
NTKAPI BOOLEAN NTAPI IoCancelIrp(PIRP Irp);
NTKAPI VOID NTAPI IoAcquireCancelSpinLock(PKIRQL Irql);
NTKAPI VOID NTAPI IoReleaseCancelSpinLock(KIRQL Irql);
NTKAPI PIRP NTAPI IoBuildSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length,
                                               PLARGE_INTEGER StartingOffset, PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock);
NTKAPI NTSTATUS NTAPI IoGetDeviceObjectPointer(PUNICODE_STRING ObjectName, ACCESS_MASK DesiredAccess,
                                               PFILE_OBJECT *FileObject, PDEVICE_OBJECT *DeviceObject);
NTKAPI VOID NTAPI RtlInitAnsiString(PANSI_STRING DestinationString, const char *SourceString);
NTKAPI NTSTATUS NTAPI RtlGetVersion(PVOID VersionInformation);
NTKAPI VOID NTAPI KeQueryTickCount(PLARGE_INTEGER TickCount);
NTKAPI NTSTATUS NTAPI PsCreateSystemThread(PHANDLE ThreadHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                           HANDLE ProcessHandle, PVOID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
NTKAPI NTSTATUS NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus);
NTKAPI NTSTATUS NTAPI ZwCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                   PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                   ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTKAPI NTSTATUS NTAPI ZwDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                            PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                            ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
NTKAPI VOID NTAPI KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4);

NTKAPI PVOID __cdecl memcpy(PVOID dst, const void *src, SIZE_T n);
NTKAPI PVOID __cdecl memset(PVOID dst, int c, SIZE_T n);
NTKAPI int __cdecl memcmp(const void *a, const void *b, SIZE_T n);
NTKAPI SIZE_T __cdecl wcslen(const WCHAR *s);

/* Inline helpers that wdm.h defines as macros or inline functions. */
#define MmGetMdlByteCount(m) ((m)->ByteCount)
#define MmGetMdlByteOffset(m) ((m)->ByteOffset)
#define MmGetMdlVirtualAddress(m) ((PVOID)((PUCHAR)(m)->StartVa + (m)->ByteOffset))
#define MmGetSystemAddressForMdlSafe(m, priority) \
    (((m)->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) \
         ? (m)->MappedSystemVa \
         : MmMapLockedPagesSpecifyCache((m), KernelMode, MmCached, NULL, FALSE, (priority)))
FORCEINLINE VOID KeInitializeSpinLock(PKSPIN_LOCK l) { *l = 0; }
FORCEINLINE VOID KeClearEvent(PKEVENT e) { e->Header.SignalState = 0; }
FORCEINLINE VOID ExInitializeFastMutex(PFAST_MUTEX m) {
    m->Count = 1; m->Owner = NULL; m->Contention = 0;
    KeInitializeEvent(&m->Event, SynchronizationEvent, FALSE);
}
FORCEINLINE VOID ExInitializeWorkItem(PWORK_QUEUE_ITEM item, VOID (NTAPI *routine)(PVOID), PVOID context) {
    item->List.Flink = NULL; item->WorkerRoutine = routine; item->Parameter = context;
}
#define KeAcquireSpinLock(l, old) (*(old) = KeAcquireSpinLockRaiseToDpc(l))
#define KeRaiseIrql(n, old) (*(old) = KfRaiseIrql(n))
#define KeLowerIrql(n) KfLowerIrql(n)
#define PAGED_CODE() ((void)0)
#define IoSetCancelRoutine(irp, routine) \
    ((PDRIVER_CANCEL)InterlockedExchangePointer((PVOID volatile *)&(irp)->CancelRoutine, (PVOID)(routine)))
#endif /* NTK_NO_API_DECLS */

#ifdef __cplusplus
}
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif /* WIN10XBOX_NTK_H */
