# Status

## M0 - repo skeleton (tag `m0`)

Layout created per `docs/ARCHITECTURE.md`; the capability probe moved to
`tools/probe/` untouched. Eleven third-party projects pinned as shallow
submodules, Wine and ReactOS as pinned-tag fetches. CMake builds `core/`
(static lib with config + tracing), `host-win32` ("hello"), and the unit/
integration test harness with both MSVC and llvm-mingw. CI: `linux-cross`
(llvm-mingw + Wine smoke) and `windows` (MSVC Debug/RelWithDebInfo, sccache,
machine folder populated from the runner's System32 at test time).
