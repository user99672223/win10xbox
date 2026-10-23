# DLLs of our own that guests load as builtins: they are staged into <machine>\Windows\builtin next to the Wine
# builtins (tests/integration/prepare_machine.cmake for the machine folders, host-uwp/stage.cmake for the package).
# Plain Win32 DLLs, built by MSVC (/MT) and llvm-mingw like tests/guest/dll.c; they never link against win10xbox_core.
set(WIN10XBOX_GUEST_DLL_DIR ${CMAKE_BINARY_DIR}/bin/guest-dlls)
add_subdirectory(xinput)
