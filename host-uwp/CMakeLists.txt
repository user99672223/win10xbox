# host-uwp: the REAL win10xbox UWP host (milestone M5).
#
# A C++/WinRT CoreApplication shell (main.cpp) around the Host of host.h, packaged as a
# signed .msix for an Xbox in Dev Mode. The Host drives the public core API on a worker
# thread: launch.json -> machine folder staging in LocalState -> Initialize ->
# process::Launch of the configured guest -> verdict in status.txt. It replaces the throwaway
# host-uwp-lite (an on-console validator for M1/M2); the two packages differ in identity,
# exe name and WinRT namespace, so they install side by side.
#
# STANDALONE project, built the way host-uwp-lite is (the CI-proven recipe): it
# add_subdirectory()s the repo's third_party/, core/ and winexbox/ itself, so the top-level
# CMakeLists.txt is not involved. (The top-level WIN10XBOX_BUILD_UWP option therefore cannot
# add this directory; see the check below.)
#
# Build (x64 Native Tools prompt, or CI after ilammy/msvc-dev-cmd):
#   cmake -S host-uwp -B build-uwp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
#         -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
#   cmake --build build-uwp
#   ctest --test-dir build-uwp --output-on-failure      (the launch.json selftest, runs anywhere)
#   host-uwp\package.cmd build-uwp                      -> build-uwp\AppPackages\*.msix + .cer
# (A Visual Studio generator with -A x64 should work too: nothing here is generator
# specific, but CI only exercises Ninja, like the main ci.yml.)
#
# Helper scripts: sdk.cmake (finds midl/makeappx/makepri/signtool and the platform
# metadata), stage.cmake (assembles the package layout), package.cmd (packs + signs).
#
# What gets built:
#   host_uwp           win10xbox-uwp.exe  the AppContainer UWP host (/APPCONTAINER, static CRT;
#                                         main.cpp + App.cpp are the only C++/WinRT sources,
#                                         host*.cpp + launch_config.cpp the Host, crash.cpp the
#                                         crash capture with dbghelp delay-loaded)
#   window_guest       window.exe         GUI guest, the packaged default (tests/guest/window.c when it
#                                         exists, else the placeholder guest/window.c)
#   hello_guest        hello.exe          sample guest, /SUBSYSTEM:WINDOWS
#   hello_guest_cui    hello_cui.exe      same source, /SUBSYSTEM:CONSOLE (A/B variant)
#   guest_hello, guest_cpuid, guest_sleep, guest_identity, guest_dll   tests/guest/*.c (console; guest_dll.dll)
#   smoke_guest_cui    smoke_cui.exe      host-uwp-lite/smoke_guest.c (console, exits 42)
#   drv_client         drv_client.exe     tests/drivers/drv_client.c (console; needs the driver host)
#   host_uwp_selftest  host_uwp_selftest.exe  console program, no layer / WinRT / package:
#                                         off-device checks of the launch.json parser (ctest)
#   host_uwp_blittest  host_uwp_blittest.exe  console program: the presenter's D3D11 upload + copy
#                                         on a WARP device, read back and checked (ctest)
#   uwp_winmd          Win10XboxUwp.winmd midl output of App.idl (the Xbox launcher wants it)
#   stage_appx         <build>/AppX/      the package layout that package.cmd packs
cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "host-uwp is a standalone CMake project: it brings its own copy of the global build settings "
                      "and add_subdirectory()s third_party/, core/ and winexbox/ itself, so it cannot be added to "
                      "the top-level tree (-DWIN10XBOX_BUILD_UWP=ON is not supported). Configure it directly: "
                      "cmake -S host-uwp -B build-uwp -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl")
endif()

project(win10xbox_uwp LANGUAGES C CXX VERSION 0.0.1)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "host-uwp builds with MSVC on Windows only (it needs the Windows SDK, midl.exe and the "
                      "C++/WinRT headers). Use an x64 Native Tools prompt, or CI's ilammy/msvc-dev-cmd.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 only (use -A x64 with Visual Studio generators, or an x64 environment with Ninja)")
endif()

# ---------------------------------------------------------------------------
# Global build settings: a copy of the top-level CMakeLists.txt's, because core/ and
# third_party/ are built in this project's scope and must come out exactly as in the main
# tree (static CRT, same warning/definition set).
# ---------------------------------------------------------------------------
# /Z7 (embedded debug info) instead of /Zi: parallel cl.exe + sccache cannot
# share a .pdb, and sccache cannot cache /Zi objects at all.
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Scope C/C++ flags to those languages: ml64 (ASM_MASM) rejects /W4, /EHsc, ...
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/permissive-;/Zc:__cplusplus;/utf-8;/EHsc>")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
# Static CRT everywhere: the loader patches code in mapped images, the guest must
# not share a runtime DLL with the host, and an AppContainer package with a static
# CRT needs no Microsoft.VCLibs framework dependency installed on the console.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)

enable_language(ASM_MASM)   # core's asm syscall entry stub
enable_language(RC)         # guest_gdi_bitmap's BITMAP resources

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
get_filename_component(WIN10XBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT EXISTS "${WIN10XBOX_ROOT}/core/CMakeLists.txt")
  message(FATAL_ERROR "host-uwp must live inside the win10xbox tree (expected ${WIN10XBOX_ROOT}/core)")
endif()

set(HOST_UWP_PACKAGE_VERSION "1.0.0.0" CACHE STRING
    "Identity Version of the package, a.b.c.d (CI passes 1.0.0.<run number> so each build installs over the last)")
set(HOST_UWP_BUILTIN_DIR "${WIN10XBOX_ROOT}/wine-pe/out/builtin" CACHE PATH
    "Wine PE builtin DLLs to bundle in the package (the wine-pe-builtin artifact). Optional: without them the package is still produced and the owner drops the DLLs into LocalState\\machine\\Windows\\builtin")
option(HOST_UWP_SKIP_WINMD "Skip midl (the package then lacks the WinRT metadata the Xbox launcher wants; CI debugging only)" OFF)
# core/CMakeLists.txt reads this (PUBLIC define). It only changes the default of
# Config::trace_default_on, which host.cpp sets itself (quiet: err + fixme; launch.json
# "trace" refines), so OFF like the top-level default.
option(WIN10XBOX_TRACE_DEFAULT_ON "Tracing on by default (always on in Debug)" OFF)

# ---------------------------------------------------------------------------
# The layer: third_party/ + core/ + winexbox/ (static libraries)
#
# core/registry/sources.cmake does add_subdirectory(${CMAKE_SOURCE_DIR}/machine/hivex)
# unless a target named w10x_mkfixture already exists. That fixture-hive generator
# is a test tool and ${CMAKE_SOURCE_DIR} is host-uwp here, so predefine an
# empty target of that name and core skips it without being edited.
#
# winexbox (the display / input backend interface and its headless implementation) is what
# Host links for winexbox::SelectBackend; it needs only win10xbox_core, so it comes after it.
# Its unit tests are attached only under WIN10XBOX_BUILD_TESTS, which is not defined here.
# ---------------------------------------------------------------------------
add_custom_target(w10x_mkfixture)
add_subdirectory(${WIN10XBOX_ROOT}/third_party third_party)
add_subdirectory(${WIN10XBOX_ROOT}/core core)
add_subdirectory(${WIN10XBOX_ROOT}/winexbox winexbox)

# ---------------------------------------------------------------------------
# The UWP host exe: AppContainer PE, CoreApplication view, headless display backend.
#   C++/WinRT sources (include pch.h):  main.cpp App.cpp launcher.cpp (the gamepad)
#   everything else is plain Win32 + the win10xbox public API:
#     host.cpp          the Host: worker flow from launch.json to the verdict (host.h)
#     host_diag.cpp     trace.log, status.txt, crash filter (host_diag.h)
#     host_stage.cpp    machine folder staging in LocalState (host_stage.h)
#     host_util.cpp     small Win32 helpers (host_util.h)
#     launch_config.cpp launch.json parser (launch_config.h; portable, also in the selftest)
#     host_shell.cpp    crash capture wired from the shell, first-run files (host_shell.h)
#     crash.cpp         crash.txt / crash.dmp / entry-reached.txt: a copy of host-uwp-lite's
#     presenter.cpp     D3D11 device thread + CoreWindow swap chain + screen.bmp (presenter.h)
#     launcher.cpp      the on-console launcher: bitmap-font list + result screen, gamepad/keyboard (launcher.h)
#     blit.cpp          the D3D11 upload + copy of the composed frame (blit.h; also in the blit test)
# ---------------------------------------------------------------------------
add_executable(host_uwp WIN32
  main.cpp
  App.cpp
  host.cpp
  host_diag.cpp
  host_stage.cpp
  host_util.cpp
  host_shell.cpp
  launch_config.cpp
  crash.cpp
  presenter.cpp
  blit.cpp
  launcher.cpp
  App.h
  host.h
  host_diag.h
  host_shell.h
  host_stage.h
  host_util.h
  launch_config.h
  crash.h
  presenter.h
  blit.h
  launcher.h
  pch.h)
set_target_properties(host_uwp PROPERTIES OUTPUT_NAME "win10xbox-uwp")
target_include_directories(host_uwp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(host_uwp PRIVATE /bigobj)
target_link_libraries(host_uwp PRIVATE
  winexbox          # SelectBackend / the headless display backend (pulls win10xbox_core with it)
  win10xbox_core
  WindowsApp        # umbrella import lib for the AppContainer-permitted exports (RoInitialize, ...)
  runtimeobject
  d3d11             # D3D11CreateDevice (presenter.cpp, on its own thread)
  dxgi
  dbghelp           # MiniDumpWriteDump for crash.dmp (crash.cpp) ...
  delayimp)         # ... delay-loaded, see the link options below
# AppContainer PE flag + WINDOWS subsystem + the wide entry (the link flags the
# probe's MSBuild project produced). /MANIFEST:NO like MSBuild UWP projects: no
# embedded Win32 manifest in a UWP exe. No /OPT:ICF on purpose: core keeps tables
# of its own function addresses, and ci.yml's configurations (/DEBUG implies
# /OPT:NOICF) never fold functions either.
# /DELAYLOAD:dbghelp.dll: crash.cpp binds it at startup (__HrLoadAllImportsForDll) and
# treats a failure as "no crash.dmp". A hard import would stop the whole package from
# starting if the console could not load dbghelp.dll inside the AppContainer.
# /MAP + /MAPINFO:EXPORTS so a crash address from the console can be turned into a function name offline, the way
# host-win32 does it (host-win32/symbolize_crash.py reads this format). An on-hardware crash record can only give
# "win10xbox-uwp.exe+0xNNNN": without the map and the pdb that offset is unattributable, which cost a full debugging
# session once already. The map is produced by the same link as the exe, so they always match.
target_link_options(host_uwp PRIVATE
  /APPCONTAINER /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO /INCREMENTAL:NO
  /DELAYLOAD:dbghelp.dll
  /MAP:${CMAKE_BINARY_DIR}/bin/win10xbox-uwp.map /MAPINFO:EXPORTS)

# ---------------------------------------------------------------------------
# The guest programs (plain Win32, NOT AppContainer; static CRT via the global setting)
# ---------------------------------------------------------------------------
add_executable(hello_guest WIN32 guest/hello.c)
target_compile_definitions(hello_guest PRIVATE HELLO_GUI=1)
set_target_properties(hello_guest PROPERTIES OUTPUT_NAME "hello")

add_executable(hello_guest_cui guest/hello.c)
set_target_properties(hello_guest_cui PROPERTIES OUTPUT_NAME "hello_cui")

# The GUI guest the packaged launch.json runs: the core/user work's tests/guest/window.c (a
# console program that prints what it checks) once it exists in the tree, until then the
# placeholder guest/window.c (/SUBSYSTEM:WINDOWS). Both exit with 42 when they succeed.
if(EXISTS "${WIN10XBOX_ROOT}/tests/guest/window.c")
  add_executable(window_guest "${WIN10XBOX_ROOT}/tests/guest/window.c")
else()
  add_executable(window_guest WIN32 guest/window.c)
endif()
target_link_libraries(window_guest PRIVATE user32 gdi32)
set_target_properties(window_guest PROPERTIES OUTPUT_NAME "window")

# Every other guest of the repo, so the package carries them all to the console (HANDOFF.md
# "Standing testing rule"). Console programs; output names as in the main tree.
foreach(_g hello cpuid sleep spawn xinput identity)
  add_executable(guest_${_g} "${WIN10XBOX_ROOT}/tests/guest/${_g}.c")
endforeach()
# Our own builtin DLLs (guest-dlls/: xinput1_4.dll and its copies xinput1_3.dll / xinput9_1_0.dll, staged into
# payload\builtin next to Wine's) and the XInput guest that imports one.
set(WIN10XBOX_GUEST_DLL_DIR ${CMAKE_BINARY_DIR}/bin/guest-dlls)
add_subdirectory(${WIN10XBOX_ROOT}/guest-dlls guest-dlls)
target_link_libraries(guest_xinput PRIVATE xinput1_4 user32)
# guest_identity: COM / WMI / registry, and the Windows 10 compatibility manifest the version checks need.
target_sources(guest_identity PRIVATE "${WIN10XBOX_ROOT}/tests/guest/identity.manifest")
target_link_libraries(guest_identity PRIVATE ole32 oleaut32 advapi32)
add_library(guest_dll SHARED "${WIN10XBOX_ROOT}/tests/guest/dll.c")
set_target_properties(guest_dll PROPERTIES PREFIX "")
add_executable(smoke_guest_cui "${WIN10XBOX_ROOT}/host-uwp-lite/smoke_guest.c")
set_target_properties(smoke_guest_cui PROPERTIES OUTPUT_NAME "smoke_cui")
# drv_client talks to \\.\OurTestDevice, which only exists when the driver host (tests/drivers
# drv_host, not part of host-uwp) has loaded OurTestDevice.sys: under host-uwp it reports that
# the device cannot be opened (exit 2). Bundled so it is on the console for that later step.
add_executable(drv_client "${WIN10XBOX_ROOT}/tests/drivers/drv_client.c")
target_include_directories(drv_client PRIVATE "${WIN10XBOX_ROOT}/tests/drivers")

# The remaining GUI guests of tests/guest (same output names as ci.yml's bin/guest), and the GUI
# smoke variant, so the package carries every guest ci.yml builds (HANDOFF.md "Package contents").
foreach(_g window2 window_input window_input2 window_text window_hooks window_nc window_scroll clipboard window_menu cursor)
  add_executable(guest_${_g} "${WIN10XBOX_ROOT}/tests/guest/${_g}.c")
  target_link_libraries(guest_${_g} PRIVATE user32 gdi32)
endforeach()
# The dialog guest carries a DIALOG resource (tests/guest/dialog.rc).
enable_language(RC)
add_executable(guest_dialog "${WIN10XBOX_ROOT}/tests/guest/dialog.c" "${WIN10XBOX_ROOT}/tests/guest/dialog.rc")
target_link_libraries(guest_dialog PRIVATE user32 gdi32)
# The common-controls guest (tests/guest/comctl.c) links comctl32.
add_executable(guest_comctl "${WIN10XBOX_ROOT}/tests/guest/comctl.c")
target_link_libraries(guest_comctl PRIVATE user32 gdi32 comctl32)
# The exception-handling guest and the DLL it throws across (tests/guest/seh.cpp, seh_dll.cpp).
add_library(guest_seh_dll SHARED "${WIN10XBOX_ROOT}/tests/guest/seh_dll.cpp")
set_target_properties(guest_seh_dll PROPERTIES PREFIX "")
add_executable(guest_seh "${WIN10XBOX_ROOT}/tests/guest/seh.cpp")
target_link_libraries(guest_seh PRIVATE guest_seh_dll)
# guest_gdi_bitmap: the same sources as tests/guest/CMakeLists.txt (a generated .rc with absolute .bmp paths, msimg32).
set(W10X_BMP_DIR "${WIN10XBOX_ROOT}/tests/guest/gdi_bitmap_res")
configure_file("${WIN10XBOX_ROOT}/tests/guest/gdi_bitmap.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/gdi_bitmap.rc" @ONLY)
add_executable(guest_gdi_bitmap "${WIN10XBOX_ROOT}/tests/guest/gdi_bitmap.c" "${CMAKE_CURRENT_BINARY_DIR}/gdi_bitmap.rc")
target_link_libraries(guest_gdi_bitmap PRIVATE user32 gdi32 msimg32)
add_executable(smoke_guest WIN32 "${WIN10XBOX_ROOT}/host-uwp-lite/smoke_guest.c")
target_compile_definitions(smoke_guest PRIVATE SMOKE_GUI=1)
set_target_properties(smoke_guest PROPERTIES OUTPUT_NAME "smoke")

set(HOST_UWP_GUESTS window_guest hello_guest hello_guest_cui guest_hello guest_cpuid guest_sleep guest_dll
    smoke_guest_cui drv_client guest_window2 guest_window_input guest_window_input2 guest_window_text smoke_guest
    guest_spawn guest_window_nc guest_dialog guest_comctl guest_seh guest_seh_dll guest_window_scroll guest_clipboard
    guest_gdi_bitmap guest_window_hooks guest_window_menu guest_xinput guest_identity guest_cursor)

# ---------------------------------------------------------------------------
# The selftest: the launch.json parser (launch_config.cpp has no Windows or layer
# dependency) exercised by selftest/selftest.cpp, a console program. Built for the build
# machine like the rest and run by ctest / the workflow before the package is made.
# enable_testing() comes after the add_subdirectory() calls on purpose: the tests of the
# third-party projects are not registered here, only this one.
# ---------------------------------------------------------------------------
add_executable(host_uwp_selftest selftest/selftest.cpp launch_config.cpp launch_config.h)
# The presenter's frame path (blit.cpp, the exact code the console runs) on a WARP device.
add_executable(host_uwp_blittest selftest/blit_test.cpp blit.cpp blit.h)
target_include_directories(host_uwp_blittest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(host_uwp_blittest PRIVATE d3d11)
enable_testing()
add_test(NAME host_uwp.selftest COMMAND host_uwp_selftest "${CMAKE_CURRENT_SOURCE_DIR}/payload")
add_test(NAME host_uwp.blit COMMAND host_uwp_blittest)

# ---------------------------------------------------------------------------
# Windows SDK tools and platform metadata (sdk.cmake)
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/sdk.cmake)

# ---------------------------------------------------------------------------
# WinRT metadata: midl turns App.idl into Win10XboxUwp.winmd. The command line is
# the one MSBuild + the C++/WinRT NuGet issued for the probe (tools/probe/build-uwp.log),
# with the platform contract winmds handed over in a response file.
# ---------------------------------------------------------------------------
set(HOST_UWP_WINMD_DIR "${CMAKE_BINARY_DIR}/winmd")
set(HOST_UWP_WINMD "${HOST_UWP_WINMD_DIR}/Win10XboxUwp.winmd")
set(HOST_UWP_WINMD_TARGET "")
if(NOT HOST_UWP_SKIP_WINMD)
  if(NOT HOST_UWP_MIDL)
    message(FATAL_ERROR "midl.exe not found. Run from an x64 Native Tools prompt (or ilammy/msvc-dev-cmd), or pass "
                        "-DHOST_UWP_SKIP_WINMD=ON to build a package WITHOUT WinRT metadata (debugging only).")
  endif()

  if(NOT HOST_UWP_REF_WINMDS)
    message(FATAL_ERROR "No Windows platform metadata (*.winmd) found under '${HOST_UWP_KITS}' for SDK "
                        "'${HOST_UWP_SDK_VER}'. Install the Windows SDK with its UWP/C++ components.")
  endif()

  list(LENGTH HOST_UWP_REF_WINMDS _n_refs)
  set(_rsp "")
  foreach(_w IN LISTS HOST_UWP_REF_WINMDS)
    file(TO_NATIVE_PATH "${_w}" _wn)
    string(APPEND _rsp "/reference \"${_wn}\"\n")
  endforeach()
  set(HOST_UWP_MIDL_RSP "${HOST_UWP_WINMD_DIR}/midl.rsp")
  file(MAKE_DIRECTORY "${HOST_UWP_WINMD_DIR}")
  file(WRITE "${HOST_UWP_MIDL_RSP}" "${_rsp}")

  set(_midl_metadata_args)
  if(HOST_UWP_METADATA_DIR)
    file(TO_NATIVE_PATH "${HOST_UWP_METADATA_DIR}" _md)
    set(_midl_metadata_args /metadata_dir "${_md}")
  endif()
  file(TO_NATIVE_PATH "${HOST_UWP_WINMD}" _winmd_native)
  file(TO_NATIVE_PATH "${HOST_UWP_MIDL_RSP}" _rsp_native)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/App.idl" _idl_native)

  add_custom_command(
    OUTPUT "${HOST_UWP_WINMD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HOST_UWP_WINMD_DIR}"
    COMMAND "${HOST_UWP_MIDL}" ${_midl_metadata_args}
            /winrt /W1 /nologo /char signed /env x64
            /winmd "${_winmd_native}"
            /h nul /dlldata nul /iid nul /proxy nul /notlb /client none /server none
            /enum_class /ns_prefix /target NT60 /nomidl
            "@${_rsp_native}" "${_idl_native}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/App.idl" "${HOST_UWP_MIDL_RSP}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "midl: App.idl -> Win10XboxUwp.winmd (Windows SDK ${HOST_UWP_SDK_VER}, ${_n_refs} reference winmds)"
    VERBATIM)
  add_custom_target(uwp_winmd ALL DEPENDS "${HOST_UWP_WINMD}")
  set(HOST_UWP_WINMD_TARGET uwp_winmd)
endif()

# ---------------------------------------------------------------------------
# Package layout: <build>/AppX (what package.cmd packs)
#   AppxManifest.xml  win10xbox-uwp.exe  Win10XboxUwp.winmd  Assets\*.png
#   payload\machine.json            repo machine/machine.default.json (22H2 defaults)
#   payload\launch.json             host-uwp/payload: the packaged default launch configuration
#   payload\launch.example.json     host-uwp/payload: every key documented (published to LocalState)
#   payload\guest\*                 every guest of HOST_UWP_GUESTS (window.exe is the default)
#   payload\builtin\*.dll           Wine PE builtins (optional, see HOST_UWP_BUILTIN_DIR) + our own (guest-dlls/)
# resources.pri is made by package.cmd (makepri).
# ---------------------------------------------------------------------------
set(HOST_UWP_STAGE_DIR "${CMAKE_BINARY_DIR}/AppX")
configure_file(Package.appxmanifest "${CMAKE_BINARY_DIR}/AppxManifest.xml" @ONLY)

set(_guest_files "")
foreach(_g IN LISTS HOST_UWP_GUESTS)
  list(APPEND _guest_files "$<TARGET_FILE:${_g}>")
endforeach()
# Passed as one -D argument, comma-separated (a ";" would not survive the command line).
string(REPLACE ";" "," _guest_files "${_guest_files}")

set(_winmd_for_stage "${HOST_UWP_WINMD}")
if(HOST_UWP_SKIP_WINMD)
  set(_winmd_for_stage "")
endif()
add_custom_target(stage_appx ALL
  COMMAND ${CMAKE_COMMAND}
          "-DSTAGE=${HOST_UWP_STAGE_DIR}"
          "-DEXE=$<TARGET_FILE:host_uwp>"
          "-DGUESTS=${_guest_files}"
          "-DWINMD=${_winmd_for_stage}"
          "-DMANIFEST=${CMAKE_BINARY_DIR}/AppxManifest.xml"
          "-DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "-DPAYLOAD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/payload"
          "-DBUILTIN_DIR=${HOST_UWP_BUILTIN_DIR}"
          "-DGUEST_DLL_DIR=${WIN10XBOX_GUEST_DLL_DIR}"
          "-DMACHINE_JSON=${WIN10XBOX_ROOT}/machine/machine.default.json"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/stage.cmake"
  COMMENT "Staging the AppX layout in ${HOST_UWP_STAGE_DIR}"
  VERBATIM)
add_dependencies(stage_appx host_uwp xinput1_4 ${HOST_UWP_GUESTS})
if(HOST_UWP_WINMD_TARGET)
  add_dependencies(stage_appx ${HOST_UWP_WINMD_TARGET})
endif()

# Tool locations for package.cmd (it falls back to PATH when this file is missing).
file(TO_NATIVE_PATH "${HOST_UWP_MAKEAPPX}" _t_makeappx)
file(TO_NATIVE_PATH "${HOST_UWP_MAKEPRI}" _t_makepri)
file(TO_NATIVE_PATH "${HOST_UWP_SIGNTOOL}" _t_signtool)
set(_tools "")
if(HOST_UWP_MAKEAPPX)
  string(APPEND _tools "set \"MAKEAPPX=${_t_makeappx}\"\r\n")
endif()
if(HOST_UWP_MAKEPRI)
  string(APPEND _tools "set \"MAKEPRI=${_t_makepri}\"\r\n")
endif()
if(HOST_UWP_SIGNTOOL)
  string(APPEND _tools "set \"SIGNTOOL=${_t_signtool}\"\r\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/sdk_tools.cmd" "${_tools}")

message(STATUS "host-uwp: Windows SDK ${HOST_UWP_SDK_VER} at '${HOST_UWP_KITS}'")
message(STATUS "host-uwp: midl='${HOST_UWP_MIDL}' makeappx='${HOST_UWP_MAKEAPPX}' makepri='${HOST_UWP_MAKEPRI}' signtool='${HOST_UWP_SIGNTOOL}'")
message(STATUS "host-uwp: package version ${HOST_UWP_PACKAGE_VERSION}, Wine builtins from '${HOST_UWP_BUILTIN_DIR}'")
if(NOT EXISTS "${HOST_UWP_BUILTIN_DIR}/ntdll.dll")
  message(STATUS "host-uwp: NO Wine PE builtins in '${HOST_UWP_BUILTIN_DIR}' - the package will be built without "
                 "them (download the wine-pe-builtin artifact there to bundle them)")
endif()
