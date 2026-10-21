# Toolchain for local cross-compilation on Linux with llvm-mingw.
# Only for fast iteration (compile checks and Wine smoke tests); CI on
# windows-latest with MSVC is the source of truth.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}" CACHE PATH "llvm-mingw install root")
if(NOT LLVM_MINGW_ROOT)
  set(LLVM_MINGW_ROOT /opt/llvm-mingw)
endif()
set(CMAKE_C_COMPILER   ${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_ASM_MASM_COMPILER ${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW_ROOT}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(WIN10XBOX_MINGW ON)
# Run test binaries under Wine (ctest honours this for add_test commands).
find_program(WINE_EXECUTABLE NAMES wine64 wine)
if(WINE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
endif()
