status: blocked

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
BLOCKED on Windows: UDP passes (`net2: udp ok`); the TLS handshake fails on the runner.
First error: `net2: InitializeSecurityContext 80090330 (call 3, 242 bytes in: 16 03 03 00 ba 04)` = SEC_E_DECRYPT_FAILURE on the
third host ISC call (NewSessionTicket + CCS + Finished from the Python server, TLS 1.2), i.e. the server Finished does not verify.
Runs: https://github.com/user99672223/win10xbox/actions/runs/39978739313 and /39982124415 (the latter replays a handshake step
whose output did not fit the caller's buffer; no change in outcome). Untried ideas: SCH_CREDENTIALS instead of SCHANNEL_CRED, a
server that does not send a session ticket (ctx.options |= ssl.OP_NO_TICKET), no ISC_REQ_EXTENDED_ERROR, check that the guest
sends the whole call-2 token. Still unsupported: server credentials, client certificates, DTLS, channel bindings.

ROOT-CAUSE LOOP (run https://github.com/user99672223/win10xbox/actions/runs/39986040044 and /39989343996, both windows jobs fail the same way):
- The SSPI buffer contract is NOT the cause. Trace (`WIN10XBOX_DEBUG=-all,+schannel`, kept; guest_net2 runs with that spec) on the runner:
  step 1 in 0 -> out 143 [16/138]; step 2 in 1213 [16/65 16/791 16/333 16/4] consumed whole, extra 0 -> out 126 [16/70 14/1 16/40]
  (CKE+CCS+Finished, handed to the caller complete); step 3 in 242 [16/186 14/1 16/40], extra 0 -> 80090330. Local (Wine 9) has the same shape and passes.
- Control: integration.net2_native (same guest_net2.exe, NOT through the layer, the runner's own SChannel, same Python server) PASSES on Windows.
  So the server/SChannel pairing is fine and the fault is in how the layer drives the host SSPI or the bytes it carries (host-view thread state around
  ISC, or the 242 bytes the guest recv hands over; the receive now goes through the pending-recv host thread in afd.cpp TryPendRecv).
- Dropping ISC_REQ_EXTENDED_ERROR and SCH_CRED_NO_SERVERNAME_CHECK (now identical to the control): no change.
- Next (not done, pushes used): dump the step-3 input as hex in the +schannel trace and have the server log sha256 of the bytes it writes
  (ssl.MemoryBIO); try the native control with the receive forced through the pending path; run ISC on a thread that is not in host view.

THIRD LOOP (run https://github.com/user99672223/win10xbox/actions/runs/39998984615, head 1f4b58e, both windows jobs fail identically; linux-cross green):
- Step 1 of the plan: the bytes are NOT the cause as far as measured. Server relay (tests/integration/net2_tls_server.py `tap`) vs the +schannel hex trace:
  client step-1 output 143 bytes sha256 15c6d757.. == what the server read (c2s 143); server write of 1213 bytes sha256 fa77f579.. == step-2 input (1213).
  The server log of the later chunks (c2s 126, s2c 242) was cut by ctest's 1024-byte output limit for a passing test, so the 242-byte step-3 input is
  unverified against the server (the relay now prints one short line per chunk: direction, length, record types/lengths, sha256 prefix).
  The step-3 input is well formed: 16/186 (NewSessionTicket, 04 00 00 b6) 14/1 16/40 (AES-GCM Finished, explicit nonce 5f8e88bd5d06c0e4).
- Step 2 of the plan: ISC/DecryptMessage/EncryptMessage/QueryContextAttributes now run on one dedicated worker thread that is never in the guest view
  (unix_schannel.cpp RunOnWorker): SAME failure (80090330 at call 3). So guest-view TLS/FLS state of the calling thread is not the cause.
- What is left that differs from the native control is PROCESS-wide: the PEB take-over (core/dispatch/process.cpp SavePebFields) replaces Ldr, LoaderLock,
  FastPebLock, TlsBitmap, ActivationContextData, ProcessParameters, code page tables and the OS version fields for the whole run, and the host's SChannel
  loads providers lazily (LoadLibrary reads exactly those). Second push (this head): RunOnWorker swaps the host's PEB fields back in around every SSPI
  call (SwapHostPebFields, twice = back), and the +schannel trace prints, per step, the image mappings that appeared during it and the host's negotiated
  connection/cipher info. Not yet observed on Windows; if it still fails, the new trace names what the host loaded at step 3 and the relay log has the
  server's 242-byte hash (artifact windows-*-logs, ctest-junit.xml, test integration.net2_tls_server_stop).
- Still untried: SCH_CREDENTIALS vs SCHANNEL_CRED, ctx.options |= ssl.OP_NO_TICKET on the server, a full in-process loopback handshake in PrimeSchannelHost
  (loads the decrypt-side providers before the take-over).
