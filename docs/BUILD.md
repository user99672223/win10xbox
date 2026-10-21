# Building

## Windows (source of truth)

Requirements: Visual Studio 2022 (Desktop C++), CMake >= 3.20, Ninja.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build
# Populate the machine folder (the guest C:\) with your own Windows 10 22H2 DLLs:
.\machine\import_windows_dlls.ps1 -Source C:\Windows\System32 -Machine build\bin\machine
ctest --test-dir build --output-on-failure
```

`win10xbox.exe --machine <dir> [--debug +relay,+loader] program.exe args...`

The UWP host (`-DWIN10XBOX_BUILD_UWP=ON`) additionally needs the Windows 10
SDK with C++/WinRT; see `host-uwp/README.md`.

## Linux (fast iteration only)

`tools/dev/build-mingw.sh [test]` cross-compiles with llvm-mingw
(`/opt/llvm-mingw` or `$LLVM_MINGW_ROOT`) and runs the tests under Wine.
Wine-on-Linux results are never treated as truth for the layer: CI on
`windows-latest` is.

## Wine PE DLL set

`wine-pe/build.sh` fetches Wine at the pinned tag (`third_party/PINS.md`) and
builds the builtin DLL set with llvm-mingw. The `wine-pe` workflow does this
on CI and publishes the set as an artifact that the Windows job downloads.

## Toolchain pins

- llvm-mingw `20250910` (clang 21), ucrt
- Wine `wine-11.9`, ReactOS commit in `third_party/PINS.md`
