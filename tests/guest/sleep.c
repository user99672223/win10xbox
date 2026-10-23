/* guest_sleep: a guest that sleeps, i.e. one that reaches NtDelayExecution. The
 * clock module forwards that to the host ntdll, and it used to resolve the
 * "host" export lazily, on the first guest call -- by which time the PEB
 * take-over has pointed GetModuleHandleW(L"ntdll.dll") at the GUEST ntdll,
 * whose NtDelayExecution is a stub rewritten to jump into the dispatcher.
 * Clk_NtDelayExecution then called itself until the stack ran out. The other
 * guests never sleep, which is why nothing caught it. The lookup then failed under Wine instead (GetProcAddress on the
 * latched module returns NULL once the PEB is the guest's), and every Sleep() returned at once without waiting: the
 * run must also take the time it asked for. Exit code 7, 1 when Sleep did not wait. */
#include <stdio.h>
#include <windows.h>

int main(void) {
    LARGE_INTEGER f, a, b;
    LONGLONG ms;
    int i;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < 4; ++i) Sleep(25);   /* 100 ms in all */
    QueryPerformanceCounter(&b);
    ms = (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart;

    if (ms < 90) {
        printf("guest did not sleep: 4 x Sleep(25) took %lld ms\n", ms);
        fflush(stdout);
        return 1;
    }
    printf("guest slept, elapsed>=90 ms\n");
    fflush(stdout);
    return 7;
}
