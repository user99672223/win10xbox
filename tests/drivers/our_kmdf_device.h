/* Shared between OurKmdfDevice.sys and its tests. */
#ifndef OUR_KMDF_DEVICE_H
#define OUR_KMDF_DEVICE_H

#define OKD_XOR_KEY 0xA5

/* METHOD_BUFFERED: out[i] = in[i] ^ 0xA5 (WdfRequestRetrieveInputBuffer / OutputBuffer) */
#define IOCTL_OKD_XOR CTL_CODE(0x22, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* METHOD_OUT_DIRECT: fills the output buffer with the byte value in the input */
#define IOCTL_OKD_FILL CTL_CODE(0x22, 0x901, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
/* METHOD_BUFFERED: returns OKD_STATS */
#define IOCTL_OKD_STATS CTL_CODE(0x22, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _OKD_STATS {
    unsigned long Opens;           /* EvtDeviceFileCreate calls (device context) */
    unsigned long Closes;          /* EvtFileClose calls */
    unsigned long Ioctls;          /* device control requests (queue context) */
    unsigned long ParamIoctlCode;  /* what WdfRequestGetParameters reported for this request */
    unsigned long ParamInputLength;
    unsigned long ParamOutputLength;
    unsigned long RequestorMode;
    unsigned long WdmIoctl;        /* ioctl read through WdfRequestWdmGetIrp */
    unsigned long FileCtx;         /* per-file-object context value */
    unsigned long SpinLockOk;
} OKD_STATS;

#endif
