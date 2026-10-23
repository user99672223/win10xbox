status: wip

# Slice: Winsock for Wine's ws2_32 (guest_net, PsInfo64)

Run: (pending, first push)

## Mechanism

Wine 11.9's ws2_32 uses **both** surfaces:

- sockets: `NtOpenFile("\Device\Afd")` and `NtDeviceIoControlFile` (real AFD codes BIND, LISTEN, POLL, GETSOCKNAME plus
  Wine's `IOCTL_AFD_WINE_*`), served by `core/afd/afd.cpp` on host Winsock `SOCKET`s (docs/DECISIONS.md, N1, N2);
- names: five calls through `__wine_unix_call` (`getaddrinfo`, `gethostbyaddr`, `gethostbyname`, `gethostname`,
  `getnameinfo`), the `ws2_32` unix lib registered in `core/dispatch/unix_calls.cpp`; `gethostname` answers the layer's
  computer name, the resolver calls are `core/dispatch/unix_ws2_32.cpp` on host Winsock. Real host errors are returned.

## Tests

- `integration.guest_net` (tests/guest/net.c, builtin-only machine folder): WSAStartup, gethostname,
  getaddrinfo("localhost"), listen/accept on a server thread, connect, select, ioctlsocket(FIONBIO) with the
  WSAEWOULDBLOCK of a non-blocking recv, send/recv echo, closesocket; exit 42.
- `integration.psinfo` (MSVC jobs, registered under the same condition as the Lua tests): Sysinternals PsInfo64.exe
  -accepteula prints `System information for`, `Uptime:`, `Kernel version:`.

## Fixes found on the way

- wine-pe/patches/0003: the guest ntdll's `heap_thread_detach` dereferenced its NULL `process_heap` (the heap is the
  host's), so the first guest thread that returned killed the process (an echo server thread is the first guest to do so).

## Syscalls / ioctls added

None new in the dispatcher; the AFD ioctl set is the one in core/afd (see DECISIONS N1).

## What fails

(pending)
