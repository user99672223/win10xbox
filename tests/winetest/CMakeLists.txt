# Wine's advapi32 conformance units through the layer, as one gated ctest entry
# (docs/WINETEST.md). wine-pe/tests/advapi32.sh builds dlls/advapi32/tests from
# wine-pe/wine-src into <build>/winetest/advapi32 and runs the units against the
# baseline in wine-pe/tests/advapi32.baseline: a unit above its failure budget,
# a recorded "completes" unit that no longer completes, or zero executed
# assertions fail the test. With MSVC the guest cannot be built at all, so the
# test is not registered (as tests/wine-tests does).
if(MSVC OR NOT CMAKE_HOST_UNIX)
  return()
endif()
find_program(WIN10XBOX_BASH bash)
if(NOT WIN10XBOX_BASH)
  return()
endif()
set(_out ${CMAKE_BINARY_DIR}/winetest/advapi32)
set(_emu "")
if(CMAKE_CROSSCOMPILING_EMULATOR)
  list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 _emu)
endif()
add_test(NAME integration.winetest_advapi32
  COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/advapi32.sh all
    --out ${_out}
    --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
    --host $<TARGET_FILE:win10xbox>
    --machine ${WIN10XBOX_MACHINE_DIR}
    --emulator "${_emu}"
    --timeout 120)
set_tests_properties(integration.winetest_advapi32 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "wine_conformance"
  TIMEOUT 600)

# Wine's msvcrt units (dlls/msvcrt/tests) through the layer, the same way: one
# guest msvcrt_test.exe linked against Wine's builtin msvcrt.dll, gated on
# wine-pe/tests/msvcrt.baseline. Per-unit cap 45 s (13 units fit the 600 s limit).
set(_out ${CMAKE_BINARY_DIR}/winetest/msvcrt)
add_test(NAME integration.winetest_msvcrt
  COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/msvcrt.sh all
    --out ${_out}
    --host $<TARGET_FILE:win10xbox>
    --machine ${WIN10XBOX_MACHINE_DIR}
    --emulator "${_emu}"
    --timeout 45)
set_tests_properties(integration.winetest_msvcrt PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "wine_conformance"
  TIMEOUT 600)
