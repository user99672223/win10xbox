/*
 * drv_client: guest program for integration.driver_ioctl. A plain Win32 program that opens
 * \\.\OurTestDevice with CreateFile and talks to it with DeviceIoControl / ReadFile / WriteFile.
 * Runs through host-win32 with OurTestDevice.sys loaded by the driver host. MIT.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include "our_test_device.h"

static int g_fail;

static void report(int ok, const char* name, const char* detail) {
    printf("%s %s%s%s\n", ok ? "OK  " : "FAIL", name, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) ++g_fail;
    fflush(stdout);
}

int main(void) {
    char msg[160];
    HANDLE h = CreateFileW(L"\\\\.\\OurTestDevice", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("FAIL open \\\\.\\OurTestDevice: error %lu\n", GetLastError());
        return 2;
    }
    report(1, "open", "");

    /* 1. METHOD_BUFFERED round trip: out = in ^ 0x5A */
    {
        unsigned char in[] = "Hello, driver! 0123456789", out[sizeof(in)] = {0};
        DWORD ret = 0, i;
        int same = 1;
        BOOL ok = DeviceIoControl(h, IOCTL_OTD_XOR, in, sizeof(in), out, sizeof(out), &ret, NULL);
        for (i = 0; i < sizeof(in); ++i) if (out[i] != (unsigned char)(in[i] ^ OTD_XOR_KEY)) same = 0;
        snprintf(msg, sizeof msg, "ok=%d ret=%lu error=%lu", ok, ret, GetLastError());
        report(ok && ret == sizeof(in) && same, "buffered xor", msg);
        /* the answer printed so the log shows the transformation */
        printf("     out[0..4] = %02x %02x %02x %02x %02x\n", out[0], out[1], out[2], out[3], out[4]);
    }

    /* 2. METHOD_OUT_DIRECT: the driver fills the MDL-described buffer */
    {
        enum { N = 9000 };
        static unsigned char buf[N];
        unsigned char pat = 0x7E;
        DWORD ret = 0, i;
        int all = 1;
        BOOL ok;
        memset(buf, 0, sizeof buf);
        ok = DeviceIoControl(h, IOCTL_OTD_FILL, &pat, 1, buf, N, &ret, NULL);
        for (i = 0; i < N; ++i) if (buf[i] != pat) { all = 0; break; }
        snprintf(msg, sizeof msg, "ok=%d ret=%lu error=%lu", ok, ret, GetLastError());
        report(ok && ret == N && all, "out_direct fill", msg);
    }

    /* 3. METHOD_IN_DIRECT: the driver reads the MDL buffer */
    {
        unsigned char data[1000];
        DWORD ret = 0, i, sum = 0;
        BOOL ok;
        unsigned char dummy = 0;
        for (i = 0; i < sizeof data; ++i) { data[i] = (unsigned char)(i * 7 + 3); sum += data[i]; }
        ok = DeviceIoControl(h, IOCTL_OTD_SUM, &dummy, 1, data, sizeof data, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d sum=%lu expected=%lu", ok, ret, sum);
        report(ok && ret == sum, "in_direct sum", msg);
    }

    /* 4. METHOD_NEITHER */
    {
        DWORD in = 41, out = 0, ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_OTD_NEITHER, &in, sizeof in, &out, sizeof out, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d out=%lu", ok, out);
        report(ok && out == 42, "neither", msg);
    }

    /* 5. STATUS_PENDING: completed later from a DPC on the host's worker thread */
    {
        OTD_DEFERRED_OUT d;
        DWORD ret = 0;
        BOOL ok;
        memset(&d, 0, sizeof d);
        ok = DeviceIoControl(h, IOCTL_OTD_DEFERRED, NULL, 0, &d, sizeof d, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d ret=%lu magic=%08lx dispatch_irql=%lu dpc_irql=%lu dpc_tid=%lu caller_tid=%lu mine=%lu",
                 ok, ret, d.Magic, d.DispatchIrql, d.DpcIrql, d.DpcThreadId, d.CallerThreadId, GetCurrentThreadId());
        report(ok && ret == sizeof d && d.Magic == OTD_DEFERRED_MAGIC && d.DispatchIrql == 0 && d.DpcIrql == 2 &&
                   d.DpcThreadId != d.CallerThreadId && d.CallerThreadId == GetCurrentThreadId(),
               "pending completed from DPC", msg);
    }

    /* 6. kernel timer + DPC */
    {
        DWORD ms = 120, elapsed = 0, ret = 0;
        DWORD t0 = GetTickCount();
        BOOL ok = DeviceIoControl(h, IOCTL_OTD_TIMER, &ms, sizeof ms, &elapsed, sizeof elapsed, &ret, NULL);
        DWORD wall = GetTickCount() - t0;
        snprintf(msg, sizeof msg, "ok=%d driver_ms=%lu wall_ms=%lu", ok, elapsed, wall);
        report(ok && elapsed >= 100 && elapsed < 5000 && wall >= 100, "kernel timer", msg);
    }

    /* 7. the driver exercises ntoskrnl from kernel mode */
    {
        OTD_SELFTEST_OUT r;
        DWORD ret = 0;
        BOOL ok;
        memset(&r, 0, sizeof r);
        ok = DeviceIoControl(h, IOCTL_OTD_SELFTEST, NULL, 0, &r, sizeof r, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d assertions=%lu failed=%lu skipped=%lu", ok, r.Executed, r.Failed, r.Skipped);
        report(ok && ret == sizeof r && r.Executed >= OTD_SELFTEST_MIN_ASSERTIONS && r.Failed == 0, "in-kernel api self test", msg);
    }

    /* 7b. the driver executes privileged instructions; the host VEH emulates them */
    {
        OTD_PRIV_OUT p;
        DWORD ret = 0;
        BOOL ok;
        memset(&p, 0, sizeof p);
        ok = DeviceIoControl(h, IOCTL_OTD_PRIV, NULL, 0, &p, sizeof p, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d cr8=%lu msr=%lu portio=%lu clisti=%lu", ok, p.Cr8RoundTrip, p.MsrRoundTrip,
                 p.PortIoOk, p.CliStiOk);
        report(ok && ret == sizeof p && p.AllOk, "privileged-instruction emulation", msg);
    }

    /* 8. what the driver sees of the caller */
    {
        OTD_INFO info;
        DWORD ret = 0;
        static const WCHAR expect[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\OurTestDevice";
        BOOL ok;
        memset(&info, 0, sizeof info);
        ok = DeviceIoControl(h, IOCTL_OTD_INFO, NULL, 0, &info, sizeof info, &ret, NULL);
        snprintf(msg, sizeof msg, "ok=%d irql=%lu prevmode=%lu pid=%lu/%lu tid=%lu/%lu opens=%lu reglen=%lu", ok, info.Irql,
                 info.PreviousMode, info.ProcessId, GetCurrentProcessId(), info.ThreadId, GetCurrentThreadId(),
                 info.OpenCount, info.RegistryPathChars);
        report(ok && info.Irql == 0 && info.PreviousMode == 1 && info.ProcessId == GetCurrentProcessId() &&
                   info.ThreadId == GetCurrentThreadId() && info.OpenCount == 1 &&
                   info.RegistryPathChars == (sizeof(expect) / sizeof(WCHAR)) - 1 &&
                   memcmp(info.RegistryPath, expect, sizeof(expect) - sizeof(WCHAR)) == 0,
               "caller context", msg);
    }

    /* 9. WriteFile / ReadFile (DO_BUFFERED_IO) */
    {
        const char text[] = "stored by the driver";
        char back[64] = {0};
        DWORD w = 0, r = 0;
        BOOL okw = WriteFile(h, text, sizeof text, &w, NULL);
        BOOL okr;
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        okr = ReadFile(h, back, sizeof back, &r, NULL);
        snprintf(msg, sizeof msg, "write=%d/%lu read=%d/%lu '%s'", okw, w, okr, r, back);
        report(okw && okr && w == sizeof text && r == sizeof text && strcmp(back, text) == 0, "write/read", msg);
    }

    /* 10. an IOCTL the driver does not know */
    {
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, CTL_CODE(OTD_DEVICE_TYPE, 0xBAD, METHOD_BUFFERED, FILE_ANY_ACCESS), NULL, 0, NULL, 0, &ret, NULL);
        DWORD err = GetLastError();
        snprintf(msg, sizeof msg, "ok=%d error=%lu (expect ERROR_INVALID_FUNCTION=1)", ok, err);
        report(!ok && err == ERROR_INVALID_FUNCTION, "unknown ioctl fails", msg);
    }

    /* 11. buffer too small */
    {
        unsigned char in[16] = {0}, out[4];
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_OTD_XOR, in, sizeof in, out, sizeof out, &ret, NULL);
        DWORD err = GetLastError();
        snprintf(msg, sizeof msg, "ok=%d error=%lu (expect ERROR_INSUFFICIENT_BUFFER=122)", ok, err);
        report(!ok && err == ERROR_INSUFFICIENT_BUFFER, "driver status maps to Win32 error", msg);
    }

    CloseHandle(h);

    /* 12. overlapped I/O: the request stays pending and signals the event on completion */
    {
        HANDLE ho = CreateFileW(L"\\\\.\\OurTestDevice", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, NULL);
        if (ho == INVALID_HANDLE_VALUE) {
            report(0, "overlapped open", "CreateFile failed");
        } else {
            OVERLAPPED ov;
            OTD_DEFERRED_OUT d;
            DWORD ret = 0;
            BOOL ok;
            memset(&ov, 0, sizeof ov);
            memset(&d, 0, sizeof d);
            ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            ok = DeviceIoControl(ho, IOCTL_OTD_DEFERRED, NULL, 0, &d, sizeof d, &ret, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(ho, &ov, &ret, TRUE);
            snprintf(msg, sizeof msg, "ok=%d ret=%lu magic=%08lx dpc_irql=%lu", ok, ret, d.Magic, d.DpcIrql);
            report(ok && ret == sizeof d && d.Magic == OTD_DEFERRED_MAGIC && d.DpcIrql == 2, "overlapped ioctl", msg);
            CloseHandle(ov.hEvent);
            CloseHandle(ho);
        }
    }

    /* 13. a name nobody owns is still the dispatcher's business */
    {
        HANDLE hn = CreateFileW(L"\\\\.\\NoSuchOurDevice", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        DWORD err = GetLastError();
        snprintf(msg, sizeof msg, "handle=%p error=%lu", hn, err);
        report(hn == INVALID_HANDLE_VALUE, "unknown device name fails", msg);
        if (hn != INVALID_HANDLE_VALUE) CloseHandle(hn);
    }

    /* 14. the device is free again after the handle was closed (exclusive-style reopen) */
    {
        HANDLE h2 = CreateFileW(L"\\\\.\\OurTestDevice", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        OTD_INFO info;
        DWORD ret = 0;
        BOOL ok = FALSE;
        memset(&info, 0, sizeof info);
        if (h2 != INVALID_HANDLE_VALUE) {
            ok = DeviceIoControl(h2, IOCTL_OTD_INFO, NULL, 0, &info, sizeof info, &ret, NULL);
            CloseHandle(h2);
        }
        snprintf(msg, sizeof msg, "reopen=%d opens=%lu (expect 1: the first handles were closed)", h2 != INVALID_HANDLE_VALUE, info.OpenCount);
        report(ok && info.OpenCount == 1, "close/reopen", msg);
    }

    if (g_fail) {
        printf("driver_ioctl: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("driver_ioctl: all checks passed\n");
    return 0;
}
