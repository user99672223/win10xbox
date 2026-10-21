/* Shared between OurTestDevice.sys (kernel side) and the user-mode clients.
 * Plain C, no includes: callers bring their own CTL_CODE. MIT. */
#ifndef OUR_TEST_DEVICE_H
#define OUR_TEST_DEVICE_H

#define OTD_DEVICE_TYPE 0x22 /* FILE_DEVICE_UNKNOWN */
#define OTD_XOR_KEY 0x5A

/* METHOD_BUFFERED: out[i] = in[i] ^ 0x5A, Information = input length */
#define IOCTL_OTD_XOR CTL_CODE(OTD_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* METHOD_OUT_DIRECT: input byte = pattern; the whole MDL output buffer is filled with it */
#define IOCTL_OTD_FILL CTL_CODE(OTD_DEVICE_TYPE, 0x801, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
/* METHOD_IN_DIRECT: the MDL buffer holds data; Information = sum of its bytes */
#define IOCTL_OTD_SUM CTL_CODE(OTD_DEVICE_TYPE, 0x802, METHOD_IN_DIRECT, FILE_ANY_ACCESS)
/* METHOD_NEITHER: Type3InputBuffer -> ULONG v; UserBuffer -> ULONG, set to v + 1 */
#define IOCTL_OTD_NEITHER CTL_CODE(OTD_DEVICE_TYPE, 0x803, METHOD_NEITHER, FILE_ANY_ACCESS)
/* METHOD_BUFFERED, completes later from a DPC (STATUS_PENDING): output = OTD_DEFERRED_OUT */
#define IOCTL_OTD_DEFERRED CTL_CODE(OTD_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* METHOD_BUFFERED, input ULONG milliseconds, completes from a kernel timer + DPC */
#define IOCTL_OTD_TIMER CTL_CODE(OTD_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* METHOD_BUFFERED: runs the kmtest-style kernel API checks inside the driver, output OTD_SELFTEST_OUT */
#define IOCTL_OTD_SELFTEST CTL_CODE(OTD_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* METHOD_BUFFERED: output OTD_INFO */
#define IOCTL_OTD_INFO CTL_CODE(OTD_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define OTD_INTERRUPT_VECTOR 0x42
#define OTD_ISR_IRQL 5
/* METHOD_BUFFERED: structured exception handling inside the driver; output OTD_SEH_OUT */
#define IOCTL_OTD_SEH CTL_CODE(OTD_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _OTD_SEH_OUT {
    unsigned long CaughtRaise;       /* ExRaiseStatus caught by __except, code stored below */
    unsigned long RaiseCode;
    unsigned long CaughtFault;       /* NULL dereference caught by __except */
    unsigned long FaultCode;
    unsigned long FinallyRan;        /* __finally block ran while unwinding */
} OTD_SEH_OUT;

#define OTD_DEFERRED_MAGIC 0xD3F3AAEDu

typedef struct _OTD_DEFERRED_OUT {
    unsigned long Magic;
    unsigned long DispatchIrql;   /* IRQL in the IOCTL dispatch routine */
    unsigned long DpcIrql;        /* IRQL in the DPC that completed the request */
    unsigned long DpcThreadId;    /* thread that ran the DPC (never the caller) */
    unsigned long CallerThreadId;
} OTD_DEFERRED_OUT;

typedef struct _OTD_INFO {
    unsigned long Irql;
    unsigned long PreviousMode;     /* 1 = UserMode */
    unsigned long ProcessId;
    unsigned long ThreadId;
    unsigned long OpenCount;
    unsigned long RegistryPathChars;
    unsigned long IsrCount;         /* interrupts delivered on OTD_INTERRUPT_VECTOR (read under KeSynchronizeExecution) */
    unsigned long IsrIrql;          /* IRQL the service routine ran at */
    unsigned short RegistryPath[96];
} OTD_INFO;

typedef struct _OTD_SELFTEST_OUT {
    unsigned long Executed;   /* ok() assertions evaluated in kernel mode */
    unsigned long Failed;
    unsigned long Skipped;
} OTD_SELFTEST_OUT;
#define OTD_SELFTEST_MIN_ASSERTIONS 40

#endif
