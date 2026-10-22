/* guest_tcp_echo: the networking path end to end. Wine's builtin ws2_32 opens \Device\Afd and issues its own ioctl set;
 * core/afd serves them on host Winsock. The program resolves "localhost" with getaddrinfo, connects to the echo server
 * the test started (port in argv[1]), sends a string, reads it back, and checks WSAPoll and select on the way.
 * Exit code 42 on success, 1..9 name the step that failed. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(int code, const char* what) {
    printf("tcp_echo: FAILED %s (wsa error %d)\n", what, WSAGetLastError());
    fflush(stdout);
    return code;
}

int main(int argc, char** argv) {
    static const char msg[] = "hello from the guest over AFD";
    char buf[128];
    WSADATA wsa;
    struct addrinfo hints, *res = NULL;
    struct sockaddr_in local, peer;
    SOCKET s;
    int got = 0, n, len;
    fd_set rfds, wfds;
    struct timeval tv;
    WSAPOLLFD pfd;

    if (argc < 2) return fail(1, "usage: guest_tcp_echo <port>");
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail(2, "WSAStartup");

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo("localhost", argv[1], &hints, &res) != 0 || !res) return fail(3, "getaddrinfo(localhost)");
    printf("tcp_echo: localhost -> %s\n", inet_ntoa(((struct sockaddr_in*)res->ai_addr)->sin_addr));

    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) return fail(4, "socket");
    printf("tcp_echo: socket created\n");

    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(s, (struct sockaddr*)&local, sizeof local) != 0) return fail(5, "bind");
    len = sizeof local;
    if (getsockname(s, (struct sockaddr*)&local, &len) != 0 || local.sin_port == 0) return fail(5, "getsockname");
    printf("tcp_echo: bound to a local port\n");

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) return fail(6, "connect");
    len = sizeof peer;
    if (getpeername(s, (struct sockaddr*)&peer, &len) != 0 || ntohs(peer.sin_port) != atoi(argv[1]))
        return fail(6, "getpeername");
    printf("tcp_echo: connected\n");
    freeaddrinfo(res);

    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    tv.tv_sec = 5; tv.tv_usec = 0;
    if (select(0, NULL, &wfds, NULL, &tv) != 1 || !FD_ISSET(s, &wfds)) return fail(7, "select(write)");

    n = send(s, msg, (int)sizeof msg, 0);
    if (n != (int)sizeof msg) return fail(7, "send");
    printf("tcp_echo: sent %d bytes\n", n);

    pfd.fd = s; pfd.events = POLLRDNORM; pfd.revents = 0;
    if (WSAPoll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLRDNORM)) return fail(8, "WSAPoll(read)");

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = 5; tv.tv_usec = 0;
    if (select(0, &rfds, NULL, NULL, &tv) != 1) return fail(8, "select(read)");

    while (got < (int)sizeof msg) {
        n = recv(s, buf + got, (int)sizeof buf - got, 0);
        if (n <= 0) return fail(8, "recv");
        got += n;
    }
    buf[got < (int)sizeof buf ? got : (int)sizeof buf - 1] = 0;
    printf("tcp_echo: received %d bytes: %s\n", got, buf);
    if (got != (int)sizeof msg || memcmp(buf, msg, sizeof msg) != 0) return fail(9, "echo mismatch");

    if (closesocket(s) != 0) return fail(9, "closesocket");
    WSACleanup();
    printf("tcp_echo: done\n");
    fflush(stdout);
    return 42;
}
