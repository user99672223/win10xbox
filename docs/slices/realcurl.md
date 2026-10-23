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
