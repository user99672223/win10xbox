/* guest_sleep: a guest that sleeps, i.e. one that reaches NtDelayExecution. The
 * clock module forwards that to the host ntdll, and it used to resolve the
 * "host" export lazily, on the first guest call -- by which time the PEB
 * take-over has pointed GetModuleHandleW(L"ntdll.dll") at the GUEST ntdll,
 * whose NtDelayExecution is a stub rewritten to jump into the dispatcher.
 * Clk_NtDelayExecution then called itself until the stack ran out. The other
 * guests never sleep, which is why nothing caught it. Exit code 7. */
#include <stdio.h>
#include <windows.h>

int main(void) {
    ULONGLONG t0 = GetTickCount64();
    int i;

    for (i = 0; i < 4; ++i) Sleep(5);

    printf("guest slept, elapsed>=%d\n", GetTickCount64() >= t0 ? 1 : 0);
    fflush(stdout);
    return 7;
}
