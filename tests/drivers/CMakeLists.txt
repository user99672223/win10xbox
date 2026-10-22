# Tests of the driver host (drivers/). Added from drivers/CMakeLists.txt.
#
#   OurTestDevice.sys     WDM test driver: \Device\OurTestDevice, \??\OurTestDevice, every IOCTL buffering method,
#                         read/write, pending IRPs completed from DPCs and timers, an in-kernel API self test
#   OurStubProbe.sys      calls an unimplemented ntoskrnl API (must hit the loud STATUS_NOT_IMPLEMENTED stub)
#   OurFailDriver.sys     DriverEntry fails after creating a device and a link (rollback)
#   drv_client.exe        guest program: CreateFile("\\.\OurTestDevice") + DeviceIoControl ... (runs under the layer)
#   drv_host.exe          win10xbox host + driver host: loads the .sys, then launches the guest
#   test_drivers.exe      unit tests that drive the drivers through the Drv_* hooks (ctest drivers.unit)
#   layout_check          ntk.h layouts against Microsoft's kernel headers (ctest drivers.layout): the mingw-w64
#                         ddk headers with llvm-mingw, the WDK's km headers with MSVC
#
# ctest: drivers.unit, drivers.layout (label `drivers`), integration.driver_ioctl (label `driver_e2e`).
enable_testing()

# The test drivers are freestanding .sys images that link /NODEFAULTLIB. MSVC's
# Debug /RTC1 runtime checks emit calls to _RTC_InitBase/_RTC_Shutdown/
# _RTC_CheckStackVars that the CRT would resolve, so with no CRT they are
# unresolved externals (LNK2019). Strip /RTC* from the C Debug flags for this
# directory (CMake resolves these at generate time from directory scope, so a
# per-target or function-local set() does not work). Only C targets here are the
# .sys drivers and the drv_client guest exe; the C++ test exes keep /RTC1.
if(MSVC)
  string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

set(WIN10XBOX_MACHINE_DIR ${CMAKE_BINARY_DIR}/bin/machine CACHE PATH "Machine folder for integration tests")
set(WIN10XBOX_DRIVER_OUT ${CMAKE_BINARY_DIR}/bin/drivers)
include(${CMAKE_SOURCE_DIR}/drivers/import_lib.cmake)

# ---- import library the drivers link against (ntoskrnl.exe) ----------------------------------------------
win10xbox_import_lib(_ntoskrnl_implib ntoskrnl_host ${CMAKE_CURRENT_SOURCE_DIR}/ntoskrnl_imports.def ntoskrnl.exe)

# win10xbox_add_test_driver(<name> <sources...> [DEFINES ...]): a native-subsystem image named <name>.sys
function(win10xbox_add_test_driver name)
  cmake_parse_arguments(A "" "" "SOURCES;DEFINES" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  set_target_properties(${name} PROPERTIES
    OUTPUT_NAME ${name} SUFFIX ".sys" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${WIN10XBOX_DRIVER_OUT}
    C_STANDARD 11)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/drivers/include ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE ${A_DEFINES})
  if(MSVC)
    target_compile_options(${name} PRIVATE /GS- /Zl /Gy)
    target_link_options(${name} PRIVATE /NODEFAULTLIB /ENTRY:DriverEntry /SUBSYSTEM:NATIVE /DYNAMICBASE /INCREMENTAL:NO)
    target_link_libraries(${name} PRIVATE ${_ntoskrnl_implib})
  else()
    target_compile_options(${name} PRIVATE -ffreestanding -fno-stack-protector -fms-extensions -Wno-unused-function)
    target_link_options(${name} PRIVATE -nostdlib -nostartfiles -Wl,--entry,DriverEntry -Wl,--subsystem,native
      -Wl,--dynamicbase -Wl,--nxcompat -Qunused-arguments)
    target_link_libraries(${name} PRIVATE ${_ntoskrnl_implib})
  endif()
  add_dependencies(${name} ntoskrnl_host_implib)
endfunction()

win10xbox_add_test_driver(OurTestDevice SOURCES OurTestDevice.c)
win10xbox_add_test_driver(OurStubProbe SOURCES OurStubProbe.c)
win10xbox_add_test_driver(OurFailDriver SOURCES OurStubProbe.c DEFINES PROBE_FAIL=1)

# ---- Microsoft's kernel headers (ntddk.h) for the KMDF driver and the layout check -----------------------------------
# llvm-mingw: the mingw-w64 ddk headers (<toolchain>/include/ddk). MSVC: the WDK's km headers, which the WDK installs
# next to the Windows SDK (Windows Kits\10\Include\<version>\km); the SDK version in use (WindowsSDKVersion, set by
# vcvars) is preferred, else the newest version that has them. Without them OurKmdfDevice.sys and layout_check are not
# built, and ci.yml's ctest gate fails on the missing tests (docs/TESTING.md).
if(MSVC)
  if(NOT WIN10XBOX_DDK_INCLUDE)
    set(_kits "$ENV{WindowsSdkDir}")
    if(NOT _kits)
      set(_kits "C:/Program Files (x86)/Windows Kits/10")
    endif()
    file(TO_CMAKE_PATH "${_kits}" _kits)
    string(REGEX REPLACE "/+$" "" _kits "${_kits}")
    file(TO_CMAKE_PATH "$ENV{WindowsSDKVersion}" _sdkver)
    string(REGEX REPLACE "/+$" "" _sdkver "${_sdkver}")
    set(_km "")
    if(_sdkver AND EXISTS "${_kits}/Include/${_sdkver}/km/ntddk.h")
      set(_km "${_kits}/Include/${_sdkver}/km")
    else()
      file(GLOB _km_cands "${_kits}/Include/*/km/ntddk.h")
      list(SORT _km_cands COMPARE NATURAL ORDER DESCENDING)
      if(_km_cands)
        list(GET _km_cands 0 _km)
        get_filename_component(_km "${_km}" DIRECTORY)
      endif()
    endif()
    if(_km)
      set(WIN10XBOX_DDK_INCLUDE "${_km}" CACHE PATH "Directory holding Microsoft's ntddk.h (the WDK's km headers)" FORCE)
    endif()
  endif()
else()
  find_path(WIN10XBOX_DDK_INCLUDE ntddk.h PATH_SUFFIXES ddk)
endif()
if(WIN10XBOX_DDK_INCLUDE)
  message(STATUS "tests/drivers: kernel headers (ntddk.h) from ${WIN10XBOX_DDK_INCLUDE}")
else()
  message(STATUS "tests/drivers: no ntddk.h (mingw-w64 ddk / WDK km headers) found")
endif()

# ---- KMDF driver: Microsoft's public KMDF headers on top of those kernel headers -------------------------------------
if(WIN10XBOX_DDK_INCLUDE AND EXISTS ${WIN10XBOX_WDF_INCLUDE}/wdf.h)
  win10xbox_import_lib(_wdfldr_implib wdfldr_host ${CMAKE_CURRENT_SOURCE_DIR}/wdfldr_imports.def wdfldr.sys)
  add_executable(OurKmdfDevice OurKmdfDevice.c kmdf_stub.c)
  set_target_properties(OurKmdfDevice PROPERTIES OUTPUT_NAME OurKmdfDevice SUFFIX ".sys" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${WIN10XBOX_DRIVER_OUT} C_STANDARD 11)
  target_include_directories(OurKmdfDevice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(OurKmdfDevice SYSTEM PRIVATE ${WIN10XBOX_DDK_INCLUDE} ${WIN10XBOX_WDF_INCLUDE}
    ${WIN10XBOX_KMDF_COMPAT_DIR})
  target_compile_definitions(OurKmdfDevice PRIVATE _AMD64_ KMDF_VERSION_MAJOR=1 KMDF_VERSION_MINOR=33)
  if(MSVC)
    # Same freestanding image as win10xbox_add_test_driver, entry point FxDriverEntry (kmdf_stub.c).
    target_compile_options(OurKmdfDevice PRIVATE /GS- /Zl /Gy)
    target_link_options(OurKmdfDevice PRIVATE /NODEFAULTLIB /ENTRY:FxDriverEntry /SUBSYSTEM:NATIVE /DYNAMICBASE
      /INCREMENTAL:NO)
  else()
    target_compile_options(OurKmdfDevice PRIVATE -ffreestanding -fno-stack-protector -Wno-unused-function
      -Wno-unknown-attributes)   # WDF_DECLARE_CONTEXT_TYPE uses __declspec(allocate)
    target_link_options(OurKmdfDevice PRIVATE -nostdlib -nostartfiles -Wl,--entry,FxDriverEntry -Wl,--subsystem,native
      -Wl,--dynamicbase -Wl,--nxcompat -Qunused-arguments)
  endif()
  target_link_libraries(OurKmdfDevice PRIVATE ${_ntoskrnl_implib} ${_wdfldr_implib})
  add_dependencies(OurKmdfDevice ntoskrnl_host_implib wdfldr_host_implib)
  set(WIN10XBOX_HAVE_KMDF_DRIVER ON)
else()
  message(STATUS "tests/drivers: kernel or KMDF headers missing, OurKmdfDevice.sys not built")
endif()

# ---- guest client + host -----------------------------------------------------------------------------------
add_executable(drv_client drv_client.c)
set_target_properties(drv_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/guest PREFIX "")
target_include_directories(drv_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(drv_client PRIVATE _CRT_SECURE_NO_WARNINGS)

if(NOT MSVC)
  target_link_options(drv_client PRIVATE -Qunused-arguments)
endif()
if(WIN10XBOX_HAVE_KMDF_DRIVER)
  add_executable(drv_kmdf_client drv_kmdf_client.c)
  set_target_properties(drv_kmdf_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/guest PREFIX "")
  target_include_directories(drv_kmdf_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(drv_kmdf_client PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(NOT MSVC)
    target_link_options(drv_kmdf_client PRIVATE -Qunused-arguments)
  endif()
endif()

add_executable(drv_host drv_host.cpp)
target_link_libraries(drv_host PRIVATE win10xbox_drivers)
if(NOT MSVC)
  target_link_options(drv_host PRIVATE -municode)
endif()

# ---- unit tests ----------------------------------------------------------------------------------------------
add_executable(test_drivers test_drivers.cpp)
target_link_libraries(test_drivers PRIVATE win10xbox_drivers)
target_include_directories(test_drivers PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit ${CMAKE_SOURCE_DIR}/drivers
  ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
  target_compile_options(test_drivers PRIVATE -Wno-invalid-offsetof)
endif()
add_dependencies(test_drivers OurTestDevice OurStubProbe OurFailDriver)
if(WIN10XBOX_HAVE_KMDF_DRIVER)
  add_dependencies(test_drivers OurKmdfDevice)
  target_compile_definitions(test_drivers PRIVATE WIN10XBOX_HAVE_KMDF_DRIVER=1)
  target_include_directories(test_drivers PRIVATE ${WIN10XBOX_GENERATED_DIR})
endif()
add_test(NAME drivers.unit COMMAND test_drivers ${WIN10XBOX_DRIVER_OUT})
set_tests_properties(drivers.unit PROPERTIES LABELS "drivers" TIMEOUT 180)

# ---- layout check against Microsoft's kernel headers (mingw-w64 ddk / WDK km) -------------------------------------
if(WIN10XBOX_DDK_INCLUDE)
  if(MSVC)
    set(_quiet "/w")
    set(_no_unused "")
  else()
    set(_quiet "-w")
    set(_no_unused "-Wno-unused-function")
  endif()
  add_executable(layout_check layout_main.c layout_ntk.c layout_ddk.c)
  if(NOT MSVC)
    target_link_options(layout_check PRIVATE -Qunused-arguments)
  endif()
  target_include_directories(layout_check PRIVATE ${CMAKE_SOURCE_DIR}/drivers/include ${CMAKE_CURRENT_SOURCE_DIR})
  # The WDK's headers need the target architecture spelled out (_AMD64_); the mingw-w64 ddk headers infer it.
  set_source_files_properties(layout_ddk.c PROPERTIES INCLUDE_DIRECTORIES "${WIN10XBOX_DDK_INCLUDE}"
    COMPILE_DEFINITIONS "$<$<C_COMPILER_ID:MSVC>:_AMD64_>" COMPILE_OPTIONS "${_quiet}")
  if(WIN10XBOX_HAVE_KMDF_DRIVER)
    target_sources(layout_check PRIVATE layout_wdf_shim.c layout_wdf_real.c)
    target_include_directories(layout_check PRIVATE ${CMAKE_SOURCE_DIR}/drivers)
    target_compile_definitions(layout_check PRIVATE HAVE_WDF=1)
    set_source_files_properties(layout_wdf_real.c PROPERTIES
      INCLUDE_DIRECTORIES "${WIN10XBOX_DDK_INCLUDE};${WIN10XBOX_WDF_INCLUDE};${WIN10XBOX_KMDF_COMPAT_DIR}"
      COMPILE_DEFINITIONS "_AMD64_" COMPILE_OPTIONS "${_quiet}")
  endif()
  if(_no_unused)
    set_source_files_properties(layout_ntk.c PROPERTIES COMPILE_OPTIONS "${_no_unused}")
  endif()
  # ntk.h is compiled without the ddk path; ddk's own (different) type names stay out of that TU.
  add_test(NAME drivers.layout COMMAND layout_check)
  set_tests_properties(drivers.layout PROPERTIES LABELS "drivers" TIMEOUT 60)
else()
  message(STATUS "tests/drivers: no kernel headers, drivers.layout not built")
endif()

# ---- end-to-end: a guest process talks to the driver through the dispatcher -----------------------------------------
add_test(NAME integration.driver_ioctl
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:drv_host>
    -DMACHINE=${WIN10XBOX_MACHINE_DIR}
    -DDRIVER=$<TARGET_FILE:OurTestDevice>
    -DCLIENT=$<TARGET_FILE:drv_client>
    -DGUEST_NAME=drv_client.exe
    -DEXPECT_EXIT=0
    "-DEXPECT_REGEX=driver loaded: .Driver.OurTestDevice;driver_ioctl: all checks passed;OK   in-kernel api self test;OK   privileged-instruction emulation;OK   pending completed from DPC;OK   overlapped ioctl"
    "-DFORBID_REGEX=FAIL "
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_driver_e2e.cmake)
set_tests_properties(integration.driver_ioctl PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "driver_e2e"
  TIMEOUT 240)
add_dependencies(drv_host OurTestDevice drv_client)

if(WIN10XBOX_HAVE_KMDF_DRIVER)
  add_test(NAME integration.driver_kmdf_ioctl
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:drv_host>
      -DMACHINE=${WIN10XBOX_MACHINE_DIR}
      -DDRIVER=$<TARGET_FILE:OurKmdfDevice>
      -DCLIENT=$<TARGET_FILE:drv_kmdf_client>
      -DGUEST_NAME=drv_kmdf_client.exe
      -DEXPECT_EXIT=0
      "-DEXPECT_REGEX=driver loaded: .Driver.OurKmdfDevice;kmdf: all checks passed;OK   kmdf request parameters and contexts"
      "-DFORBID_REGEX=FAIL "
      -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_driver_e2e.cmake)
  set_tests_properties(integration.driver_kmdf_ioctl PROPERTIES
    FIXTURES_REQUIRED w10x_machine SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "SKIP:" LABELS "driver_e2e" TIMEOUT 240)
  add_dependencies(drv_host OurKmdfDevice drv_kmdf_client)
endif()
