/*
 * drv_kmdf_client: guest program for integration.driver_kmdf_ioctl. Talks to \\.\OurKmdfDevice, a KMDF
 * control-device driver running on the host's WDF shim, with plain CreateFile / DeviceIoControl. MIT.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include "our_kmdf_device.h"

static int g_fail;
static void report(int ok, const char* name, const char* detail) {
    printf("%s %s%s%s\n", ok ? "OK  " : "FAIL", name, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) ++g_fail;
    fflush(stdout);
}

int main(void) {
    char msg[200];
    HANDLE h = CreateFileW(L"\\\\.\\OurKmdfDevice", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("FAIL open \\\\.\\OurKmdfDevice: error %lu\n", GetLastError());
        return 2;
    }
    report(1, "kmdf open", "");

    {
        unsigned char in[] = "KMDF request", out[sizeof(in)] = {0};
        DWORD ret = 0, i;
        int same = 1;
        BOOL ok = DeviceIoControl(h, IOCTL_OKD_XOR, in, sizeof(in), out, sizeof(out), &ret, NULL);
        for (i = 0; i < sizeof(in); ++i) if (out[i] != (unsigned char)(in[i] ^ OKD_XOR_KEY)) same = 0;
        snprintf(msg, sizeof msg, "ok=%d ret=%lu", ok, ret);
        report(ok && ret == sizeof(in) && same, "kmdf buffered xor", msg);
    }
    {
        static unsigned char buf[6000];
        unsigned char pat = 0x42;
        DWORD ret = 0, i;
        int all = 1;
        BOOL ok = DeviceIoControl(h, IOCTL_OKD_FILL, &pat, 1, buf, sizeof buf, &ret, NULL);
        for (i = 0; i < sizeof buf; ++i) if (buf[i] != pat) { all = 0; break; }
        snprintf(msg, sizeof msg, "ok=%d ret=%lu", ok, ret);
        report(ok && ret == sizeof buf && all, "kmdf out_direct fill", msg);
    }
    {
        OKD_STATS s;
        DWORD ret = 0;
        BOOL ok;
        memset(&s, 0, sizeof s);
        ok = DeviceIoControl(h, IOCTL_OKD_STATS, NULL, 0, &s, sizeof s, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d opens=%lu ioctls=%lu param_code=%08lx wdm_code=%08lx in=%lu out=%lu mode=%lu filectx=%lu",
                 ok, s.Opens, s.Ioctls, s.ParamIoctlCode, s.WdmIoctl, s.ParamInputLength, s.ParamOutputLength, s.RequestorMode, s.FileCtx);
        report(ok && s.Opens == 1 && s.Ioctls == 3 && s.ParamIoctlCode == IOCTL_OKD_STATS && s.WdmIoctl == IOCTL_OKD_STATS &&
                   s.ParamInputLength == 0 && s.ParamOutputLength == sizeof s && s.RequestorMode == 1 && s.FileCtx == 1,
               "kmdf request parameters and contexts", msg);
    }
    {
        DWORD w = 0;
        BOOL ok = WriteFile(h, "abc", 3, &w, NULL);
        snprintf(msg, sizeof msg, "ok=%d written=%lu", ok, w);
        report(ok && w == 3, "kmdf write", msg);
    }
    {
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, CTL_CODE(0x22, 0xFFF, METHOD_BUFFERED, FILE_ANY_ACCESS), NULL, 0, NULL, 0, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d error=%lu", ok, GetLastError());
        report(!ok && GetLastError() == ERROR_INVALID_FUNCTION, "kmdf unknown ioctl", msg);
    }
    CloseHandle(h);
    if (g_fail) { printf("kmdf: %d check(s) FAILED\n", g_fail); return 1; }
    printf("kmdf: all checks passed\n");
    return 0;
}
