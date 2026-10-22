# integration.winetest_kernel32: Wine's kernel32 conformance tests (dlls/kernel32/tests) built as one PE x64
# program by wine-pe/tests/kernel32.sh and run unit by unit through win10xbox. Gated on the recorded per-unit
# baseline (wine-pe/tests/kernel32.baseline): a unit with more failures than recorded, or a unit that no longer
# completes, fails the test, as does a run with no assertion executed at all. Numbers: docs/WINETEST.md.
#
# Registered only for llvm-mingw on a Unix host (the guests run under Wine there): MSVC cannot build Wine's
# tests, like tests/wine-tests. Without wine-pe/wine-src, the Wine builtins or the NLS data the test SKIPs (77),
# which CI's tools/ci/check_ctest_run.py counts as a failure.
if(MSVC OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_HOST_UNIX)
  message(STATUS "winetest: integration.winetest_kernel32 needs llvm-mingw on a Unix host; not registered")
  return()
endif()
find_program(W10X_BASH bash)
if(NOT W10X_BASH)
  message(STATUS "winetest: bash not found; integration.winetest_kernel32 not registered")
  return()
endif()

set(_out ${CMAKE_BINARY_DIR}/winetest-kernel32)
add_test(NAME integration.winetest_kernel32
  COMMAND ${W10X_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/kernel32.sh
    --cc ${CMAKE_C_COMPILER}
    --out ${_out}
    --host $<TARGET_FILE:win10xbox>
    --machine ${WIN10XBOX_MACHINE_DIR}
    --emulator "${CMAKE_CROSSCOMPILING_EMULATOR}"
    --baseline ${CMAKE_SOURCE_DIR}/wine-pe/tests/kernel32.baseline)
set_tests_properties(integration.winetest_kernel32 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  LABELS "winetest"
  TIMEOUT 900)
