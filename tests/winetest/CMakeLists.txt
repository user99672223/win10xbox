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

# --- slice wt-media: Wine's propsys, windowscodecs, mlang, gdiplus, dwrite suites -------------------
# One entry per suite, all run by wine-pe/tests/media.sh --dll <dll> (units from the suite's
# Makefile.in, baseline wine-pe/tests/<dll>.baseline). Per-unit cap 20 s; ctest TIMEOUT covers
# the build plus units x cap in the worst case (docs/slices/wt-media.md has the measured seconds).
set(_media_emu "")
if(CMAKE_CROSSCOMPILING_EMULATOR)
  list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 _media_emu)
endif()
set(_media_timeout_propsys 300)
set(_media_timeout_windowscodecs 700)
set(_media_timeout_mlang 300)
set(_media_timeout_gdiplus 700)
set(_media_timeout_dwrite 400)
foreach(_dll propsys windowscodecs mlang gdiplus dwrite)
  add_test(NAME integration.winetest_${_dll}
    COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/media.sh all
      --dll ${_dll}
      --out ${CMAKE_BINARY_DIR}/winetest/${_dll}
      --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
      --host $<TARGET_FILE:win10xbox>
      --machine ${WIN10XBOX_MACHINE_DIR}
      --emulator "${_media_emu}"
      --timeout 20)
  set_tests_properties(integration.winetest_${_dll} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "wine_conformance"
    TIMEOUT ${_media_timeout_${_dll}})
endforeach()
