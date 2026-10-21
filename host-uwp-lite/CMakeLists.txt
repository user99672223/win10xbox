# host-uwp-lite: THROWAWAY on-console validator for M1 (loader) + M2 (dispatcher).
#
# A minimal UWP C++/WinRT AppContainer app that, when launched on the console,
# runs the host-win32 smoke sequence (Initialize -> process::Launch of a bundled
# guest exe -> Shutdown) on a worker thread and leaves result.txt / stdout.txt /
# trace.log in its LocalState, packaged as a .msix. It only DRIVES the public
# core API; the real host-uwp stays milestone M5.
#
# STANDALONE project: it add_subdirectory()s the repo's third_party/ and core/
# exactly the way the capability probe's tools/probe/uwp-host/CMakeLists.txt
# add_subdirectory()s probe-core, so the top-level CMakeLists.txt is not touched.
#
# Build (x64 Native Tools prompt, or CI after ilammy/msvc-dev-cmd):
#   cmake -S host-uwp-lite -B build-uwp-lite -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
#         -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
#   cmake --build build-uwp-lite
#   host-uwp-lite\package.cmd build-uwp-lite       -> build-uwp-lite\AppPackages\*.msix + .cer
# (A Visual Studio generator with -A x64 should work too: nothing here is generator
# specific, but CI only exercises Ninja, like the main ci.yml.)
#
# Helper scripts: sdk.cmake (finds midl/makeappx/makepri/signtool and the platform
# metadata), stage.cmake (assembles the package layout), package.cmd (packs + signs).
#
# What gets built:
#   host_uwp_lite      win10xbox-uwp-lite.exe  the AppContainer UWP host (/APPCONTAINER, static CRT;
#                                              smoke.cpp harness + crash.cpp crash capture, dbghelp delay-loaded)
#   smoke_guest        smoke.exe               guest, /SUBSYSTEM:WINDOWS (the default guest)
#   smoke_guest_cui    smoke_cui.exe           same source, /SUBSYSTEM:CONSOLE (A/B variant)
#   uwp_lite_winmd     Win10XboxLite.winmd     midl output of App.idl (the Xbox launcher wants it)
#   stage_appx         <build>/AppX/           the package layout that package.cmd packs
cmake_minimum_required(VERSION 3.25)
project(win10xbox_uwp_lite LANGUAGES C CXX VERSION 0.0.1)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "host-uwp-lite builds with MSVC on Windows only (it needs the Windows SDK, midl.exe and the "
                      "C++/WinRT headers). Use an x64 Native Tools prompt, or CI's ilammy/msvc-dev-cmd.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 only (use -A x64 with Visual Studio generators, or an x64 environment with Ninja)")
endif()

# ---------------------------------------------------------------------------
# Global build settings: a copy of the top-level CMakeLists.txt's, because core/
# and third_party/ are built in this project's scope and must come out exactly as
# in the main tree (static CRT, same warning/definition set).
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

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
get_filename_component(WIN10XBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT EXISTS "${WIN10XBOX_ROOT}/core/CMakeLists.txt")
  message(FATAL_ERROR "host-uwp-lite must live inside the win10xbox tree (expected ${WIN10XBOX_ROOT}/core)")
endif()

set(UWP_LITE_PACKAGE_VERSION "1.0.0.0" CACHE STRING
    "Identity Version of the package, a.b.c.d (CI passes 1.0.0.<run number> so each build installs over the last)")
set(UWP_LITE_BUILTIN_DIR "${WIN10XBOX_ROOT}/wine-pe/out/builtin" CACHE PATH
    "Wine PE builtin DLLs to bundle in the package (the wine-pe-builtin artifact). Optional: without them the package is still produced and the owner drops the DLLs into LocalState\\machine\\Windows\\builtin")
option(UWP_LITE_SKIP_WINMD "Skip midl (the package then lacks the WinRT metadata the Xbox launcher wants; CI debugging only)" OFF)
# Tracing on by default in the layer (core/CMakeLists.txt reads this; PUBLIC define)
option(WIN10XBOX_TRACE_DEFAULT_ON "Tracing on by default (always on in Debug)" ON)

# ---------------------------------------------------------------------------
# The layer: third_party/ + core/ (static libraries)
#
# core/registry/sources.cmake does add_subdirectory(${CMAKE_SOURCE_DIR}/machine/hivex)
# unless a target named w10x_mkfixture already exists. That fixture-hive generator
# is a test tool and ${CMAKE_SOURCE_DIR} is host-uwp-lite here, so predefine an
# empty target of that name and core skips it without being edited.
# ---------------------------------------------------------------------------
add_custom_target(w10x_mkfixture)
add_subdirectory(${WIN10XBOX_ROOT}/third_party third_party)
add_subdirectory(${WIN10XBOX_ROOT}/core core)

# ---------------------------------------------------------------------------
# The UWP host exe: AppContainer PE, CoreApplication view, no renderer
# ---------------------------------------------------------------------------
add_executable(host_uwp_lite WIN32
  main.cpp
  App.cpp
  smoke.cpp
  crash.cpp
  App.h
  smoke.h
  crash.h
  pch.h)
set_target_properties(host_uwp_lite PROPERTIES OUTPUT_NAME "win10xbox-uwp-lite")
target_include_directories(host_uwp_lite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(host_uwp_lite PRIVATE /bigobj)
target_link_libraries(host_uwp_lite PRIVATE
  win10xbox_core
  WindowsApp        # umbrella import lib for the AppContainer-permitted exports (RoInitialize, ...)
  runtimeobject
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
target_link_options(host_uwp_lite PRIVATE
  /APPCONTAINER /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO /INCREMENTAL:NO
  /DELAYLOAD:dbghelp.dll)

# ---------------------------------------------------------------------------
# The guest programs (plain Win32, NOT AppContainer; static CRT via the global setting)
# ---------------------------------------------------------------------------
add_executable(smoke_guest WIN32 smoke_guest.c)
target_compile_definitions(smoke_guest PRIVATE SMOKE_GUI=1)
set_target_properties(smoke_guest PROPERTIES OUTPUT_NAME "smoke")

add_executable(smoke_guest_cui smoke_guest.c)
set_target_properties(smoke_guest_cui PROPERTIES OUTPUT_NAME "smoke_cui")

# ---------------------------------------------------------------------------
# Windows SDK tools and platform metadata (sdk.cmake)
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/sdk.cmake)

# ---------------------------------------------------------------------------
# WinRT metadata: midl turns App.idl into Win10XboxLite.winmd. The command line is
# the one MSBuild + the C++/WinRT NuGet issued for the probe (tools/probe/build-uwp.log),
# with the platform contract winmds handed over in a response file.
# ---------------------------------------------------------------------------
set(UWP_LITE_WINMD_DIR "${CMAKE_BINARY_DIR}/winmd")
set(UWP_LITE_WINMD "${UWP_LITE_WINMD_DIR}/Win10XboxLite.winmd")
set(UWP_LITE_WINMD_TARGET "")
if(NOT UWP_LITE_SKIP_WINMD)
  if(NOT UWP_LITE_MIDL)
    message(FATAL_ERROR "midl.exe not found. Run from an x64 Native Tools prompt (or ilammy/msvc-dev-cmd), or pass "
                        "-DUWP_LITE_SKIP_WINMD=ON to build a package WITHOUT WinRT metadata (debugging only).")
  endif()

  if(NOT UWP_LITE_REF_WINMDS)
    message(FATAL_ERROR "No Windows platform metadata (*.winmd) found under '${UWP_LITE_KITS}' for SDK "
                        "'${UWP_LITE_SDK_VER}'. Install the Windows SDK with its UWP/C++ components.")
  endif()

  list(LENGTH UWP_LITE_REF_WINMDS _n_refs)
  set(_rsp "")
  foreach(_w IN LISTS UWP_LITE_REF_WINMDS)
    file(TO_NATIVE_PATH "${_w}" _wn)
    string(APPEND _rsp "/reference \"${_wn}\"\n")
  endforeach()
  set(UWP_LITE_MIDL_RSP "${UWP_LITE_WINMD_DIR}/midl.rsp")
  file(MAKE_DIRECTORY "${UWP_LITE_WINMD_DIR}")
  file(WRITE "${UWP_LITE_MIDL_RSP}" "${_rsp}")

  set(_midl_metadata_args)
  if(UWP_LITE_METADATA_DIR)
    file(TO_NATIVE_PATH "${UWP_LITE_METADATA_DIR}" _md)
    set(_midl_metadata_args /metadata_dir "${_md}")
  endif()
  file(TO_NATIVE_PATH "${UWP_LITE_WINMD}" _winmd_native)
  file(TO_NATIVE_PATH "${UWP_LITE_MIDL_RSP}" _rsp_native)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/App.idl" _idl_native)

  add_custom_command(
    OUTPUT "${UWP_LITE_WINMD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UWP_LITE_WINMD_DIR}"
    COMMAND "${UWP_LITE_MIDL}" ${_midl_metadata_args}
            /winrt /W1 /nologo /char signed /env x64
            /winmd "${_winmd_native}"
            /h nul /dlldata nul /iid nul /proxy nul /notlb /client none /server none
            /enum_class /ns_prefix /target NT60 /nomidl
            "@${_rsp_native}" "${_idl_native}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/App.idl" "${UWP_LITE_MIDL_RSP}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "midl: App.idl -> Win10XboxLite.winmd (Windows SDK ${UWP_LITE_SDK_VER}, ${_n_refs} reference winmds)"
    VERBATIM)
  add_custom_target(uwp_lite_winmd ALL DEPENDS "${UWP_LITE_WINMD}")
  set(UWP_LITE_WINMD_TARGET uwp_lite_winmd)
endif()

# ---------------------------------------------------------------------------
# Package layout: <build>/AppX (what package.cmd packs)
#   AppxManifest.xml  win10xbox-uwp-lite.exe  Win10XboxLite.winmd  Assets\*.png
#   payload\machine.json            repo machine/machine.default.json (22H2 defaults)
#   payload\guest\smoke.exe, smoke_cui.exe
#   payload\builtin\*.dll           Wine PE builtins (optional, see UWP_LITE_BUILTIN_DIR)
# resources.pri is made by package.cmd (makepri).
# ---------------------------------------------------------------------------
set(UWP_LITE_STAGE_DIR "${CMAKE_BINARY_DIR}/AppX")
configure_file(Package.appxmanifest "${CMAKE_BINARY_DIR}/AppxManifest.xml" @ONLY)

set(_winmd_for_stage "${UWP_LITE_WINMD}")
if(UWP_LITE_SKIP_WINMD)
  set(_winmd_for_stage "")
endif()
add_custom_target(stage_appx ALL
  COMMAND ${CMAKE_COMMAND}
          "-DSTAGE=${UWP_LITE_STAGE_DIR}"
          "-DEXE=$<TARGET_FILE:host_uwp_lite>"
          "-DGUEST_GUI=$<TARGET_FILE:smoke_guest>"
          "-DGUEST_CUI=$<TARGET_FILE:smoke_guest_cui>"
          "-DWINMD=${_winmd_for_stage}"
          "-DMANIFEST=${CMAKE_BINARY_DIR}/AppxManifest.xml"
          "-DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "-DBUILTIN_DIR=${UWP_LITE_BUILTIN_DIR}"
          "-DMACHINE_JSON=${WIN10XBOX_ROOT}/machine/machine.default.json"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/stage.cmake"
  COMMENT "Staging the AppX layout in ${UWP_LITE_STAGE_DIR}"
  VERBATIM)
add_dependencies(stage_appx host_uwp_lite smoke_guest smoke_guest_cui)
if(UWP_LITE_WINMD_TARGET)
  add_dependencies(stage_appx ${UWP_LITE_WINMD_TARGET})
endif()

# Tool locations for package.cmd (it falls back to PATH when this file is missing).
file(TO_NATIVE_PATH "${UWP_LITE_MAKEAPPX}" _t_makeappx)
file(TO_NATIVE_PATH "${UWP_LITE_MAKEPRI}" _t_makepri)
file(TO_NATIVE_PATH "${UWP_LITE_SIGNTOOL}" _t_signtool)
set(_tools "")
if(UWP_LITE_MAKEAPPX)
  string(APPEND _tools "set \"MAKEAPPX=${_t_makeappx}\"\r\n")
endif()
if(UWP_LITE_MAKEPRI)
  string(APPEND _tools "set \"MAKEPRI=${_t_makepri}\"\r\n")
endif()
if(UWP_LITE_SIGNTOOL)
  string(APPEND _tools "set \"SIGNTOOL=${_t_signtool}\"\r\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/sdk_tools.cmd" "${_tools}")

message(STATUS "host-uwp-lite: Windows SDK ${UWP_LITE_SDK_VER} at '${UWP_LITE_KITS}'")
message(STATUS "host-uwp-lite: midl='${UWP_LITE_MIDL}' makeappx='${UWP_LITE_MAKEAPPX}' makepri='${UWP_LITE_MAKEPRI}' signtool='${UWP_LITE_SIGNTOOL}'")
message(STATUS "host-uwp-lite: package version ${UWP_LITE_PACKAGE_VERSION}, Wine builtins from '${UWP_LITE_BUILTIN_DIR}'")
if(NOT EXISTS "${UWP_LITE_BUILTIN_DIR}/ntdll.dll")
  message(STATUS "host-uwp-lite: NO Wine PE builtins in '${UWP_LITE_BUILTIN_DIR}' - the package will be built without "
                 "them (download the wine-pe-builtin artifact there to bundle them)")
endif()
