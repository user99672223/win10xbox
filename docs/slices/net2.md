status: wip

HANDOFF (implement agent, net2: UDP + TLS for guests). Head: see `git log -1`; first and only push so far; ci.yml not yet run.
1. UDP: core/afd/afd.cpp - an overlapped IOCTL_AFD_WINE_RECVMSG (force_async) on a socket with nothing to read now returns
   STATUS_PENDING and a host thread completes it (iosb, event, APC) once the socket is readable; before it blocked in the host recv.
2. TLS: core/dispatch/unix_schannel.cpp registers the `secur32` unix lib (enum schan_funcs, 22 entries) on the HOST's SSPI
   SChannel; Wine's PE secur32 hands whole TLS records over in SecBuffers, no sockets. Host creds are SCH_CRED_MANUAL_CRED_VALIDATION
   (chain validation stays with the guest/crypt32). Not supported, answered with an error: server credentials, client certificates,
   unique channel bindings, DTLS; protocol set is the host default. PrimeSchannelHost() runs before the PEB take-over (like afd::PrimeHost).
3. wine-pe/dlls.txt: + schannel (secur32 loads schannel.dll as the provider module; without it AcquireCredentialsHandle gives SEC_E_SECPKG_NOT_FOUND). Needs the wine-pe.yml run.
4. Gate: tests/guest/net2.c -> integration.guest_net2 (builtin-only folder). Fixture integration.net2_tls_server_start/_stop runs
   tests/integration/net2_tls_server.py (python3/python, openssl or `cryptography` for the self-signed cert, port 47583).
5. Local result (Wine 9 host): guest_net2 green, prints `net2: udp ok`, `net2: tls handshake ok, protocol TLS 1.2, peer certificate 781 bytes`, `net2: tls ok (TLS 1.2)`.
6. UNTESTED on Windows: the Windows SSPI host (Server 2022 may negotiate TLS 1.3: the test accepts 1.2 or 1.3), host-uwp (guest_net2 added to the
   console foreach, HOST_UWP_GUESTS and the host-uwp.yml list; links secur32/crypt32 into win10xbox_core), openssl discovery on the runners.
7. Commands: `ctest --test-dir build-mingw -R 'guest_net2|net2_tls'`; `wine-pe/build.sh --only schannel` for the new DLL.
8. Syscalls: none changed (no policy change, SYSCALLS.md untouched).

## What fails
(pending CI)
