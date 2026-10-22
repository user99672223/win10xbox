/* tcp_echo_runner: host-side helper of integration.guest_tcp_echo. Portable (plain Win32 + Winsock, built with the
 * same toolchain as everything else, so it also runs under Wine on Linux and needs no Python).
 *
 *   tcp_echo_runner <expected-exit> -- <command> [args...]
 *
 * Opens a TCP echo server on 127.0.0.1 with a free port, runs the command with every "{PORT}" argument replaced by
 * that port, waits for it, and succeeds when the command exits with <expected-exit> and the server really echoed
 * bytes. The server is the "outside world" the guest talks to, so it must not depend on the layer under test. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET g_listen = INVALID_SOCKET;
static volatile LONG g_echoed = 0;

static DWORD WINAPI server(void* arg) {
    char buf[1024];
    SOCKET c;
    (void)arg;
    c = accept(g_listen, NULL, NULL);
    if (c == INVALID_SOCKET) return 0;
    for (;;) {
        int n = recv(c, buf, sizeof buf, 0), off = 0;
        if (n <= 0) break;
        while (off < n) {
            int w = send(c, buf + off, n - off, 0);
            if (w <= 0) { n = 0; break; }
            off += w;
        }
        InterlockedAdd(&g_echoed, off);
    }
    closesocket(c);
    return 0;
}

int main(int argc, char** argv) {
    WSADATA wsa;
    struct sockaddr_in a;
    int len = sizeof a, i, expect, port;
    char cmd[4096] = "", portstr[16];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1, w;
    HANDLE thread;

    if (argc < 4 || strcmp(argv[2], "--") != 0) {
        fprintf(stderr, "usage: tcp_echo_runner <expected-exit> -- <command> [args...]\n");
        return 2;
    }
    expect = atoi(argv[1]);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_listen == INVALID_SOCKET || bind(g_listen, (struct sockaddr*)&a, sizeof a) != 0 ||
        listen(g_listen, 1) != 0 || getsockname(g_listen, (struct sockaddr*)&a, &len) != 0) {
        fprintf(stderr, "tcp_echo_runner: cannot open the echo server (wsa error %d)\n", WSAGetLastError());
        return 2;
    }
    port = ntohs(a.sin_port);
    sprintf(portstr, "%d", port);
    printf("tcp_echo_runner: echo server on 127.0.0.1:%d\n", port);
    fflush(stdout);

    for (i = 3; i < argc; ++i) {
        const char* arg = strcmp(argv[i], "{PORT}") == 0 ? portstr : argv[i];
        if (strlen(cmd) + strlen(arg) + 4 >= sizeof cmd) { fprintf(stderr, "command line too long\n"); return 2; }
        if (i > 3) strcat(cmd, " ");
        if (strchr(arg, ' ')) { strcat(cmd, "\""); strcat(cmd, arg); strcat(cmd, "\""); }
        else strcat(cmd, arg);
    }

    thread = CreateThread(NULL, 0, server, NULL, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "tcp_echo_runner: CreateProcess failed (%lu): %s\n", GetLastError(), cmd);
        return 2;
    }
    w = WaitForSingleObject(pi.hProcess, 120000);
    if (w != WAIT_OBJECT_0) { fprintf(stderr, "tcp_echo_runner: command timed out\n"); TerminateProcess(pi.hProcess, 3); }
    GetExitCodeProcess(pi.hProcess, &rc);
    closesocket(g_listen);   /* unblocks accept() if the command never connected */
    WaitForSingleObject(thread, 5000);
    printf("tcp_echo_runner: command exited with %lu, server echoed %ld bytes\n", rc, (long)g_echoed);
    if ((int)rc != expect) { printf("tcp_echo_runner: FAILED expected exit code %d\n", expect); return 1; }
    if (g_echoed <= 0) { printf("tcp_echo_runner: FAILED the server never saw the guest's data\n"); return 1; }
    printf("tcp_echo_runner: PASS\n");
    return 0;
}
