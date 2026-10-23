# DLLs of our own that guests load as builtins: they are staged into <machine>\Windows\builtin next to the Wine
# builtins (tests/integration/prepare_machine.cmake for the machine folders, host-uwp/stage.cmake for the package).
# Plain Win32 DLLs, built by MSVC (/MT) and llvm-mingw like tests/guest/dll.c; they never link against win10xbox_core.
# The caller (top-level CMakeLists.txt, host-uwp/CMakeLists.txt) sets WIN10XBOX_GUEST_DLL_DIR: a variable set in
# here would not reach the tests/ and the staging that read it.
if(NOT WIN10XBOX_GUEST_DLL_DIR)
  message(FATAL_ERROR "guest-dlls: WIN10XBOX_GUEST_DLL_DIR is not set by the caller")
endif()
add_subdirectory(xinput)
