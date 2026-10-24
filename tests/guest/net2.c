/* guest_net2: UDP and TLS from a guest, on Wine's ws2_32 (over core/afd) and Wine's secur32/schannel (over the host's
 * SSPI). UDP: bind to port 0 + getsockname, sendto/recvfrom on loopback, WSARecvFrom overlapped with an event.
 * TLS: connect to the 127.0.0.1:<port> server of tests/integration/net2_tls_server.py (self-signed certificate),
 * SChannel with SCH_CRED_MANUAL_CRED_VALIDATION (the handshake and the encrypted echo are gated, the chain is not).
 * Exit code 42 on success; "net2: FAILED <step>" names a failed step. */
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fail(int code, const char* what) {
    printf("net2: FAILED %s (wsa error %d)\n", what, WSAGetLastError());
    fflush(stdout);
    return code;
}

static int udp_test(void) {
    SOCKET rx, tx;
    struct sockaddr_in a, from;
    int len = sizeof a, flen;
    char buf[64];
    int n;
    static const char m1[] = "udp datagram one", m2[] = "udp datagram two";
    WSABUF wb;
    WSAOVERLAPPED ov;
    DWORD got = 0, flags = 0;

    rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx == INVALID_SOCKET || tx == INVALID_SOCKET) return fail(10, "socket(SOCK_DGRAM)");
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(rx, (struct sockaddr*)&a, sizeof a) != 0) return fail(11, "bind(port 0)");
    if (getsockname(rx, (struct sockaddr*)&a, &len) != 0 || a.sin_port == 0) return fail(11, "getsockname");
    printf("net2: udp bound to a loopback port\n");

    if (sendto(tx, m1, sizeof m1, 0, (struct sockaddr*)&a, sizeof a) != (int)sizeof m1) return fail(12, "sendto");
    flen = sizeof from;
    n = recvfrom(rx, buf, sizeof buf, 0, (struct sockaddr*)&from, &flen);
    if (n != (int)sizeof m1 || memcmp(buf, m1, sizeof m1) != 0) return fail(13, "recvfrom");
    if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return fail(13, "recvfrom source address");

    memset(&ov, 0, sizeof ov);
    ov.hEvent = WSACreateEvent();
    wb.buf = buf;
    wb.len = sizeof buf;
    flen = sizeof from;
    if (WSARecvFrom(rx, &wb, 1, &got, &flags, (struct sockaddr*)&from, &flen, &ov, NULL) == 0) {
        /* completed at once: the datagram can not be there yet */
        return fail(14, "WSARecvFrom completed before any send");
    }
    if (WSAGetLastError() != WSA_IO_PENDING) return fail(14, "WSARecvFrom");
    if (sendto(tx, m2, sizeof m2, 0, (struct sockaddr*)&a, sizeof a) != (int)sizeof m2) return fail(12, "sendto 2");
    if (WSAWaitForMultipleEvents(1, &ov.hEvent, TRUE, 10000, FALSE) != WSA_WAIT_EVENT_0) return fail(15, "event wait");
    if (!WSAGetOverlappedResult(rx, &ov, &got, FALSE, &flags) || got != sizeof m2 || memcmp(buf, m2, sizeof m2) != 0)
        return fail(15, "WSAGetOverlappedResult");
    WSACloseEvent(ov.hEvent);
    closesocket(rx);
    closesocket(tx);
    printf("net2: udp ok\n");
    fflush(stdout);
    return 0;
}

/* ---- TLS: SChannel (Wine's secur32) against the local echo server --------------------------------------------- */
static int send_all(SOCKET s, const char* p, int n) {
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0) return -1;
        p += w; n -= w;
    }
    return 0;
}

static const char* proto_name(DWORD p) {
    if (p & SP_PROT_TLS1_3_CLIENT) return "TLS 1.3";
    if (p & SP_PROT_TLS1_2_CLIENT) return "TLS 1.2";
    if (p & SP_PROT_TLS1_1_CLIENT) return "TLS 1.1";
    if (p & SP_PROT_TLS1_0_CLIENT) return "TLS 1.0";
    return "unknown";
}

static int tls_test(int port) {
    static char inbuf[65536];
    int inlen = 0, calls = 0;
    SOCKET s;
    struct sockaddr_in a;
    SCHANNEL_CRED sc;
    CredHandle cred;
    CtxtHandle ctx;
    TimeStamp ts;
    SECURITY_STATUS ss;
    BOOL have_ctx = FALSE;
    DWORD attr;
    static const char msg[] = "hello over tls";
    SecPkgContext_StreamSizes sizes;
    SecPkgContext_ConnectionInfo info;
    PCCERT_CONTEXT peer = NULL;
    char* wire;
    int n;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr*)&a, sizeof a) != 0) return fail(20, "tls connect");

    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    ss = AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &cred, &ts);
    if (ss != SEC_E_OK) { printf("net2: AcquireCredentialsHandle %08lx\n", (unsigned long)ss); return fail(21, "AcquireCredentialsHandle"); }

    for (;;) {
        SecBuffer ib[2], ob[1];
        calls++;
        SecBufferDesc id, od;
        id.cBuffers = 0;
        ob[0].BufferType = SECBUFFER_TOKEN; ob[0].cbBuffer = 0; ob[0].pvBuffer = NULL;
        od.ulVersion = SECBUFFER_VERSION; od.cBuffers = 1; od.pBuffers = ob;
        if (have_ctx) {
            ib[0].BufferType = SECBUFFER_TOKEN; ib[0].cbBuffer = inlen; ib[0].pvBuffer = inbuf;
            ib[1].BufferType = SECBUFFER_EMPTY; ib[1].cbBuffer = 0; ib[1].pvBuffer = NULL;
            id.ulVersion = SECBUFFER_VERSION; id.cBuffers = 2; id.pBuffers = ib;
        }
        ss = InitializeSecurityContextA(&cred, have_ctx ? &ctx : NULL, (SEC_CHAR*)"localhost",
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
            ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION,
            0, 0, have_ctx ? &id : NULL, 0, &ctx, &od, &attr, &ts);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            n = recv(s, inbuf + inlen, (int)sizeof inbuf - inlen, 0);
            if (n <= 0) return fail(22, "tls handshake recv");
            inlen += n;
            continue;
        }
        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) {
            printf("net2: InitializeSecurityContext %08lx (call %d, %d bytes in: %02x %02x %02x %02x %02x %02x)\n",
                   (unsigned long)ss, calls, inlen, (unsigned char)inbuf[0], (unsigned char)inbuf[1],
                   (unsigned char)inbuf[2], (unsigned char)inbuf[3], (unsigned char)inbuf[4], (unsigned char)inbuf[5]);
            return fail(22, "InitializeSecurityContext");
        }
        have_ctx = TRUE;
        if (ob[0].cbBuffer && ob[0].pvBuffer) {
            int bad = send_all(s, ob[0].pvBuffer, (int)ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (bad) return fail(22, "tls handshake send");
        }
        /* consume what the handshake took; keep SECBUFFER_EXTRA */
        if (id.cBuffers == 2 && ib[1].BufferType == SECBUFFER_EXTRA) {
            memmove(inbuf, inbuf + inlen - ib[1].cbBuffer, ib[1].cbBuffer);
            inlen = (int)ib[1].cbBuffer;
        } else if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK) {
            inlen = 0;
        }
        if (ss == SEC_E_OK) break;
        n = recv(s, inbuf + inlen, (int)sizeof inbuf - inlen, 0);
        if (n <= 0) return fail(22, "tls handshake recv");
        inlen += n;
    }

    if (QueryContextAttributes(&ctx, SECPKG_ATTR_CONNECTION_INFO, &info) != SEC_E_OK) return fail(23, "CONNECTION_INFO");
    if (QueryContextAttributes(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) return fail(23, "STREAM_SIZES");
    if (QueryContextAttributes(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer) != SEC_E_OK || !peer || !peer->cbCertEncoded)
        return fail(23, "REMOTE_CERT_CONTEXT");
    printf("net2: tls handshake ok, protocol %s, peer certificate %lu bytes\n", proto_name(info.dwProtocol),
           (unsigned long)peer->cbCertEncoded);
    CertFreeCertificateContext(peer);

    {   /* encrypt "hello over tls" and send it */
        SecBuffer b[4];
        SecBufferDesc d;
        int len = (int)sizeof msg - 1;
        wire = (char*)malloc(sizes.cbHeader + len + sizes.cbTrailer);
        memcpy(wire + sizes.cbHeader, msg, len);
        b[0].BufferType = SECBUFFER_STREAM_HEADER; b[0].cbBuffer = sizes.cbHeader; b[0].pvBuffer = wire;
        b[1].BufferType = SECBUFFER_DATA; b[1].cbBuffer = len; b[1].pvBuffer = wire + sizes.cbHeader;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER; b[2].cbBuffer = sizes.cbTrailer; b[2].pvBuffer = wire + sizes.cbHeader + len;
        b[3].BufferType = SECBUFFER_EMPTY; b[3].cbBuffer = 0; b[3].pvBuffer = NULL;
        d.ulVersion = SECBUFFER_VERSION; d.cBuffers = 4; d.pBuffers = b;
        ss = EncryptMessage(&ctx, 0, &d, 0);
        if (ss != SEC_E_OK) { printf("net2: EncryptMessage %08lx\n", (unsigned long)ss); return fail(24, "EncryptMessage"); }
        if (send_all(s, wire, (int)(b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))) return fail(24, "tls send");
        free(wire);
        printf("net2: tls sent %d bytes\n", len);
    }

    /* read records until the echo is complete */
    for (;;) {
        SecBuffer b[4];
        SecBufferDesc d;
        if (inlen) {
            b[0].BufferType = SECBUFFER_DATA; b[0].cbBuffer = inlen; b[0].pvBuffer = inbuf;
            b[1].BufferType = b[2].BufferType = b[3].BufferType = SECBUFFER_EMPTY;
            b[1].cbBuffer = b[2].cbBuffer = b[3].cbBuffer = 0;
            b[1].pvBuffer = b[2].pvBuffer = b[3].pvBuffer = NULL;
            d.ulVersion = SECBUFFER_VERSION; d.cBuffers = 4; d.pBuffers = b;
            ss = DecryptMessage(&ctx, &d, 0, NULL);
            if (ss == SEC_E_OK) {
                int i;
                for (i = 0; i < 4; i++) {
                    if (b[i].BufferType != SECBUFFER_DATA) continue;
                    if (b[i].cbBuffer == sizeof msg - 1 && memcmp(b[i].pvBuffer, msg, sizeof msg - 1) == 0) {
                        printf("net2: tls received %lu bytes: %.*s\n", (unsigned long)b[i].cbBuffer, (int)b[i].cbBuffer,
                               (char*)b[i].pvBuffer);
                        printf("net2: tls ok (%s)\n", proto_name(info.dwProtocol));
                        closesocket(s);
                        return 0;
                    }
                    return fail(25, "tls echo differs");
                }
                inlen = 0;   /* a record without application data (e.g. a session ticket) */
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                printf("net2: DecryptMessage %08lx\n", (unsigned long)ss);
                return fail(25, "DecryptMessage");
            }
        }
        n = recv(s, inbuf + inlen, (int)sizeof inbuf - inlen, 0);
        if (n <= 0) return fail(25, "tls recv");
        inlen += n;
    }
}

int main(int argc, char** argv) {
    WSADATA wsa;
    int rc;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail(2, "WSAStartup");
    rc = udp_test();
    if (rc) return rc;
    if (argc > 1) {
        rc = tls_test(atoi(argv[1]));
        if (rc) return rc;
    }
    printf("net2: done\n");
    fflush(stdout);
    return 42;
}
