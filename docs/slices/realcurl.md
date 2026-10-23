status: ready

# Slice: realcurl

curl 8.11.1 for Windows x64 (`curl.exe`, built in CI by `tools/ci/build_curl.ps1` from the pinned release tarball, no TLS)
through the layer, gated by `integration.real_curl_*` (registered under `if(MSVC OR WIN10XBOX_REAL_PROGRAMS)`).

Pass condition: `curl --version` lists protocols; `curl file:///C:/...` reads a local file. No network.

## Log

- Branch merged with main (ws2_32 is a Wine builtin there); tests re-registered like the Lua ones with
  `win10xbox_guest_test`; the HTTP-server test was removed from this slice.
- Earlier attempt (run 39848075368): `curl --version` ended with exit code -2147483392 after mapping the guest's
  system DLLs; ws2_32.dll was taken from System32 there, not from Windows\builtin.
- Run 39876191044 (push 1): all non-curl tests green; the four curl tests died with 0x80000100 (EXCEPTION_WINE_STUB,
  `api-ms-win-core-crt-l2-1-0.dll` `_initterm_e`) because ws2_32 was the runner's native System32 DLL
  (native advapi32/sechost/rpcrt4 chain). Same cause as the GUI guests (docs/TESTING.md): the curl tests now run on the
  builtin-only machine folder, where ws2_32 is Wine's; `run_guest.cmake` got `-DNO_USER32=ON` for a console program.
- Run 39877475822 (push 2): only the four curl tests failed; curl.exe imports `bcrypt.dll` (libcurl's `BCryptGenRandom`),
  which the builtin-only folder did not have (`err:wine:module:import_dll Library bcrypt.dll ... not found`, c0000135).
  Wine's bcrypt.dll is now in `wine-pe/dlls.txt`: its DllMain tolerates a missing unix lib and `BCryptGenRandom` with the
  system RNG goes through `RtlGenRandom`, so no unix call is needed for curl.

## Result

Run: https://github.com/user99672223/win10xbox/actions/runs/39881262411 (head 6d6449d; linux-cross, windows (Debug) and
windows (RelWithDebInfo) green, gate "51 registered, 51 ran and passed" on both Windows jobs). linux-cross failed once on
`integration.guest_window_input2` ("another thread's window: delivered on that thread", a Wine window test unrelated to
this slice) and passed on a re-run of the failed job, no code change.

What runs through the layer: `curl.exe` 8.11.1 (MSVC/CMake build of the pinned tarball, no TLS) on the builtin-only
machine folder, `integration.real_curl_version|file|file_copy|file_missing` (output copied from ctest-junit.xml of the
Debug job):

    C:\guest\curl.exe --version
    curl 8.11.1 (Windows) libcurl/8.11.1
    Release-Date: 2024-12-11
    Protocols: dict file ftp gopher http imap ipfs ipns mqtt pop3 rtsp smb smtp telnet tftp ws
    Features: alt-svc AsynchDNS IPv6 Largefile NTLM threadsafe UnixSockets

    C:\guest\curl.exe -sS file:///C:/Users/User/curl_in.txt
    hello from a guest file, read by curl
    second line

    C:\guest\curl.exe -sS -o C:\Users\User\curl_out.bin -w size=%{size_download} file:///C:/Users/User/curl_big.bin
    size=320000        (320000-byte copy compared byte for byte by the test)

`file_missing` expects exit code 37 (CURLE_FILE_COULDNT_READ_FILE). System DLLs mapped, all from Windows\builtin:
kernel32 kernelbase ws2_32 ucrtbase bcrypt advapi32 sechost msvcrt cryptbase iphlpapi dnsapi nsi.

Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: `bcrypt`. Guest programs: curl.exe (built in CI by
tools/ci/build_curl.ps1, not a tests/guest target).

What still fails: no TLS (https absent from Protocols on purpose); no network test (networking is another slice).
host-uwp.yml fails at configure on this head ("No target guest_spawn", CMakeLists.txt:347) and did so on main's
46936f1 and c2345df runs too; this slice adds no guest target and nothing to host-uwp.
