/* guest_tebframe: an exception raised on any guest thread reaches a Wine __TRY frame (core/dispatch/teb_frames.cpp).
 *
 * Wine's PE builtins protect code with __TRY/__EXCEPT_PAGE_FAULT frames chained through TEB.NtTib.ExceptionList, which
 * a Windows host's exception dispatcher does not walk. kernelbase's IsBadStringPtrA/W are such a frame: a bad pointer
 * raises an access violation inside the frame and the API must return TRUE instead of ending the thread. The same
 * dispatch carries sechost's device-notify thread (RPC_S_SERVER_UNAVAILABLE caught by rpcrt4's rpc_filter), which is
 * what the Direct3D gate needs. Exit 42 = every check passed, on the main thread and on worker threads.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("tebframe: FAIL line %d: %s\n", __LINE__, #cond);             \
            InterlockedIncrement(&g_failures);                                   \
        }                                                                        \
    } while (0)

static void bad_pointers(int rounds)
{
    static const char good[] = "readable";
    static const WCHAR goodw[] = L"readable";
    int i;
    for (i = 0; i < rounds; i++) {
        CHECK(IsBadStringPtrA((LPCSTR)(ULONG_PTR)0x10, 8));
        CHECK(IsBadStringPtrW((LPCWSTR)(ULONG_PTR)0x20, 8));
        /* a frame that was left and entered again, and one that did not fault */
        CHECK(!IsBadStringPtrA(good, sizeof(good)));
        CHECK(!IsBadStringPtrW(goodw, sizeof(goodw) / sizeof(WCHAR)));
    }
}

static HANDLE g_done[4];
static HANDLE g_never;

struct worker_arg { int index; int rounds; };

/* The worker reports and then parks: a worker that returns would run the guest's thread-exit path, which a Wine
 * development host does not support for threads it did not create (the process exit ends the parked ones). */
static DWORD WINAPI worker(void *arg)
{
    struct worker_arg *a = (struct worker_arg *)arg;
    bad_pointers(a->rounds);
    SetEvent(g_done[a->index]);
    WaitForSingleObject(g_never, INFINITE);
    return 0;
}

int main(int argc, char **argv)
{
    struct worker_arg args[4];
    int rounds = argc > 1 ? atoi(argv[1]) : 200;   /* a diagnostic knob: worker rounds */
    int i;

    g_never = CreateEventW(NULL, TRUE, FALSE, NULL);
    printf("tebframe: start\n");
    fflush(stdout);
    bad_pointers(50);
    printf("tebframe: main ok\n");
    fflush(stdout);

    for (i = 0; i < 4; i++) {
        HANDLE t;
        g_done[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        args[i].index = i;
        args[i].rounds = rounds;
        t = CreateThread(NULL, 0, worker, &args[i], 0, NULL);
        CHECK(t != NULL);
        if (t) CloseHandle(t);
    }
    for (i = 0; i < 4; i++)
        CHECK(WaitForSingleObject(g_done[i], 60000) == WAIT_OBJECT_0);
    printf("tebframe: workers ok\n");

    printf("tebframe: done failures=%ld\n", (long)g_failures);
    fflush(stdout);
    return g_failures ? 1 : 42;
}
