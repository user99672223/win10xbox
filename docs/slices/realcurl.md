status: wip

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
