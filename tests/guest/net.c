/* guest_net: Wine's builtin ws2_32 end to end over TCP loopback. ws2_32 opens \Device\Afd and issues its own ioctl set
 * (served by core/afd on host Winsock), and calls its unix side for names (gethostname, getaddrinfo). The program
 * starts an echo server thread on 127.0.0.1, connects to it as a client, checks select, ioctlsocket(FIONBIO) and
 * the error of a non-blocking recv, and compares the echo. Exit code 42 on success; any other value names the step
 * that failed (printed as "net: FAILED <step>"). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

static const char kMsg[] = "hello from the guest over loopback";

static int fail(int code, const char* what) {
    printf("net: FAILED %s (wsa error %d)\n", what, WSAGetLastError());
    fflush(stdout);
    return code;
}

/* The echo server: accepts one connection and sends back everything it receives until the peer closes. */
static DWORD WINAPI server_thread(LPVOID arg) {
    SOCKET listener = (SOCKET)(ULONG_PTR)arg, c;
    char buf[256];
    int n;
    c = accept(listener, NULL, NULL);
    if (c == INVALID_SOCKET) { printf("net: server accept failed (wsa error %d)\n", WSAGetLastError()); return 1; }
    for (;;) {
        n = recv(c, buf, sizeof buf, 0);
        if (n == 0) break;
        if (n < 0) { printf("net: server recv failed (wsa error %d)\n", WSAGetLastError()); closesocket(c); return 2; }
        for (int off = 0; off < n;) {
            int w = send(c, buf + off, n - off, 0);
            if (w <= 0) { printf("net: server send failed (wsa error %d)\n", WSAGetLastError()); closesocket(c); return 3; }
            off += w;
        }
    }
    closesocket(c);
    return 0;
}

int main(void) {
    WSADATA wsa;
    char host[256], buf[128];
    struct addrinfo hints, *res = NULL;
    struct sockaddr_in addr, peer;
    SOCKET listener, s;
    HANDLE thread;
    DWORD exit_code = 99;
    fd_set rfds, wfds;
    struct timeval tv;
    u_long nb;
    int len, got = 0, n;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail(2, "WSAStartup");
    printf("net: WSAStartup ok, version %d.%d\n", LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));

    if (gethostname(host, sizeof host) != 0 || !host[0]) return fail(3, "gethostname");
    printf("net: gethostname ok (%u chars)\n", (unsigned)strlen(host));

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo("localhost", "0", &hints, &res) != 0 || !res) return fail(4, "getaddrinfo(localhost)");
    if (((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return fail(4, "localhost is not 127.0.0.1");
    freeaddrinfo(res);
    printf("net: getaddrinfo localhost -> 127.0.0.1\n");

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return fail(5, "socket(listener)");
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr*)&addr, sizeof addr) != 0) return fail(5, "bind");
    len = sizeof addr;
    if (getsockname(listener, (struct sockaddr*)&addr, &len) != 0 || addr.sin_port == 0) return fail(5, "getsockname");
    if (listen(listener, 1) != 0) return fail(5, "listen");
    printf("net: listening on a loopback port\n");

    thread = CreateThread(NULL, 0, server_thread, (LPVOID)(ULONG_PTR)listener, 0, NULL);
    if (!thread) return fail(6, "CreateThread");

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return fail(7, "socket(client)");
    if (connect(s, (struct sockaddr*)&addr, sizeof addr) != 0) return fail(7, "connect");
    len = sizeof peer;
    if (getpeername(s, (struct sockaddr*)&peer, &len) != 0 || peer.sin_port != addr.sin_port)
        return fail(7, "getpeername");
    printf("net: connected\n");

    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    tv.tv_sec = 5; tv.tv_usec = 0;
    if (select(0, NULL, &wfds, NULL, &tv) != 1 || !FD_ISSET(s, &wfds)) return fail(8, "select(write)");

    /* Non-blocking mode: nothing has been sent, so recv must fail with WSAEWOULDBLOCK; select times out. */
    nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) != 0) return fail(9, "ioctlsocket(FIONBIO, 1)");
    if (recv(s, buf, sizeof buf, 0) != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
        return fail(9, "non-blocking recv did not fail with WSAEWOULDBLOCK");
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = 0; tv.tv_usec = 50000;
    if (select(0, &rfds, NULL, NULL, &tv) != 0) return fail(9, "select(read) on an idle socket");
    nb = 0;
    if (ioctlsocket(s, FIONBIO, &nb) != 0) return fail(9, "ioctlsocket(FIONBIO, 0)");
    printf("net: FIONBIO ok (WSAEWOULDBLOCK, select timeout)\n");

    n = send(s, kMsg, (int)sizeof kMsg, 0);
    if (n != (int)sizeof kMsg) return fail(10, "send");
    printf("net: sent %d bytes\n", n);

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = 5; tv.tv_usec = 0;
    if (select(0, &rfds, NULL, NULL, &tv) != 1 || !FD_ISSET(s, &rfds)) return fail(11, "select(read)");
    while (got < (int)sizeof kMsg) {
        n = recv(s, buf + got, (int)sizeof buf - got, 0);
        if (n <= 0) return fail(11, "recv");
        got += n;
    }
    buf[got < (int)sizeof buf ? got : (int)sizeof buf - 1] = 0;
    printf("net: received %d bytes: %s\n", got, buf);
    if (got != (int)sizeof kMsg || memcmp(buf, kMsg, sizeof kMsg) != 0) return fail(12, "echo mismatch");

    if (closesocket(s) != 0) return fail(13, "closesocket(client)");
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return fail(13, "server thread did not finish");
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    if (exit_code != 0) return fail(13, "server thread failed");
    if (closesocket(listener) != 0) return fail(13, "closesocket(listener)");
    if (WSACleanup() != 0) return fail(13, "WSACleanup");
    printf("net: done\n");
    fflush(stdout);
    return 42;
}
