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

# ---------------------------------------------------------------------------
# Small Wine suites (slice wt-misc): one ctest entry per DLL, all driven by
# wine-pe/tests/misc.sh --dll <dll> (same build/run/baseline scheme as advapi32
# above; baselines in wine-pe/tests/<dll>.baseline). Listed in priority order;
# each entry is "<dll> <per-unit timeout s> <ctest TIMEOUT s>".
# msxml3 is not registered: its xmlparser.idl needs widl, which linux-cross
# does not have (docs/slices/wt-misc.md).
# ---------------------------------------------------------------------------
foreach(_s IN ITEMS
    "psapi 20 240" "userenv 20 240" "cabinet 20 300" "xmllite 20 300" "mpr 20 240"
    "bcrypt 20 240" "setupapi 20 480" "dbghelp 20 360" "wintrust 20 360"
    "ncrypt 20 240" "crypt32 20 600")
  string(REPLACE " " ";" _s "${_s}")
  list(GET _s 0 _dll)
  list(GET _s 1 _unit_timeout)
  list(GET _s 2 _ctest_timeout)
  add_test(NAME integration.winetest_${_dll}
    COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/misc.sh all
      --dll ${_dll}
      --out ${CMAKE_BINARY_DIR}/winetest/${_dll}
      --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
      --host $<TARGET_FILE:win10xbox>
      --machine ${WIN10XBOX_MACHINE_DIR}
      --emulator "${_emu}"
      --timeout ${_unit_timeout})
  set_tests_properties(integration.winetest_${_dll} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "wine_conformance"
    TIMEOUT ${_ctest_timeout})
endforeach()
