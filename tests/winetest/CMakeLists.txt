# Wine's conformance units through the layer, as gated ctest entries (docs/TESTING.md).
#
# integration.winetest_advapi32: wine-pe/tests/advapi32.sh builds dlls/advapi32/tests from
# wine-pe/wine-src into <build>/winetest/advapi32 and runs the units against the baseline in
# wine-pe/tests/advapi32.baseline: a unit above its failure budget, a recorded "completes" unit that
# no longer completes, or zero executed assertions fail the test. With MSVC the guest cannot be
# built at all, so the test is not registered (as tests/wine-tests does).
#
# integration.winetest_ntdll: ntdll_test.exe (all of dlls/ntdll/tests as one guest) run by
# run_ntdll.cmake against wine-pe/tests/ntdll.allowlist. The guest is built OUTSIDE cmake
# (wine-pe/tests/ntdll.sh build --out <build>/winetest/ntdll: a ci.yml step of the Windows jobs, whose
# MSVC cannot build it) and the test is registered where it exists, as the real programs are. It runs on
# the builtin-only machine folder (the console's DLL set, as the GUI tests do): on a Windows runner the
# shared one holds the runner's own advapi32/sechost/rpcrt4/ucrtbase, which the guest's imports pull in
# and which die on Wine's 0x80000100 import stubs.
set(_ntdll_exe ${CMAKE_BINARY_DIR}/winetest/ntdll/ntdll_test.exe)
if(EXISTS ${_ntdll_exe})
  set(_emu "")
  if(CMAKE_CROSSCOMPILING_EMULATOR)
    list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 _emu)
  endif()
  add_test(NAME integration.winetest_ntdll
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox> -DMACHINE=${CMAKE_BINARY_DIR}/bin/machine-builtin -DEXE=${_ntdll_exe}
      -DALLOWLIST=${CMAKE_SOURCE_DIR}/wine-pe/tests/ntdll.allowlist
      -DPARITY=${CMAKE_SOURCE_DIR}/wine-pe/tests/ntdll.parity
      -DRESULTS_DIR=${CMAKE_BINARY_DIR}/winetest/ntdll/results
      "-DEMULATOR=${_emu}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ntdll.cmake)
  set_tests_properties(integration.winetest_ntdll PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "winetest_ntdll"
    TIMEOUT 1500)
endif()

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
set(_media_timeout_gdiplus 900)
set(_media_timeout_dwrite 400)
# gdiplus region takes ~15 s: a 20 s cap is too tight on a loaded runner.
set(_media_cap_propsys 20)
set(_media_cap_windowscodecs 20)
set(_media_cap_mlang 20)
set(_media_cap_gdiplus 40)
set(_media_cap_dwrite 20)
foreach(_dll propsys windowscodecs mlang gdiplus dwrite)
  add_test(NAME integration.winetest_${_dll}
    COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/media.sh all
      --dll ${_dll}
      --out ${CMAKE_BINARY_DIR}/winetest/${_dll}
      --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
      --host $<TARGET_FILE:win10xbox>
      --machine ${WIN10XBOX_MACHINE_DIR}
      --emulator "${_media_emu}"
      --timeout ${_media_cap_${_dll}})
  set_tests_properties(integration.winetest_${_dll} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "wine_conformance"
    TIMEOUT ${_media_timeout_${_dll}})
endforeach()

# Wine's kernel32 conformance units through the layer (docs/WINETEST.md):
# wine-pe/tests/kernel32.sh builds every dlls/kernel32/tests source into one
# kernel32_test.exe and runs it unit by unit against wine-pe/tests/kernel32.baseline
# (0-failure allowlist, "run <max>" / crash / timeout for the rest). Same gate and
# MSVC rule as advapi32 above.
# Under bin/ so the linux-cross-bin artifact carries kernel32.tsv and the per-unit logs.
set(_out ${CMAKE_BINARY_DIR}/bin/winetest-kernel32)
add_test(NAME integration.winetest_kernel32
  COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/kernel32.sh all
    --out ${_out}
    --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
    --host $<TARGET_FILE:win10xbox>
    --machine ${WIN10XBOX_MACHINE_DIR}
    --emulator "${_emu}"
    --timeout 30)
set_tests_properties(integration.winetest_kernel32 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "wine_conformance"
  TIMEOUT 1500)

# Wine's shell32 conformance units (dlls/shell32/tests, 19 units, 30 s each).
# Units needing a desktop, COM servers or explorer may crash or time out; the
# baseline in wine-pe/tests/shell32.baseline records what was measured.
set(_out ${CMAKE_BINARY_DIR}/winetest/shell32)
add_test(NAME integration.winetest_shell32
  COMMAND ${WIN10XBOX_BASH} ${CMAKE_SOURCE_DIR}/wine-pe/tests/shell32.sh all
    --out ${_out}
    --gen-include ${WIN10XBOX_WINE_GEN_INCLUDE_DIR}
    --host $<TARGET_FILE:win10xbox>
    --machine ${WIN10XBOX_MACHINE_DIR}
    --emulator "${_emu}"
    --timeout 30)
set_tests_properties(integration.winetest_shell32 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "wine_conformance"
  TIMEOUT 900)

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
