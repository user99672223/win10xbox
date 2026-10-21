# Guest test programs: real Windows PE executables/DLLs that the layer loads.
# They must not link to the host's core; they are plain Win32 programs built
# with the same toolchain (MSVC on CI, llvm-mingw locally) and placed in
# bin/guest/. Owned by the loader work; see tests/guest/README.md.
