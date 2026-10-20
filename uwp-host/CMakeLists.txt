# uwp-host: C++/WinRT CoreApplication UWP for the capability probe
# (environments 2 & 3 - laptop UWP and Xbox Series S Dev Mode).
#
# We build the exe directly - no AppXPackage MSBuild integration (that would
# need the UWP VS workload, which is not installed; this install has the Native
# Desktop C++ workload only). The compiler/linker produce an AppContainer PE
# that uses the store CRT; packaging is finished by package.cmd using the
# command-line SDK tools (makeappx + makepri + signtool), all of which ARE
# present in this install (confirmed in C:\Program Files (x86)\Windows Kits\10\
# bin\10.0.26100.0\x64).
#
# To build:   cmake -S uwp-host -B build-uwp -A x64
#             cmake --build build-uwp --config Debug
#             uwp-host\package.cmd  (produces AppX\xbox-probe-uwp.msix + .cer)
cmake_minimum_required(VERSION 3.20)
project(xbox_probe_uwp CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "build x64: use -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WINAPI_FAMILY_DESKTOP_APP keeps header-level API visibility (we need to
# RESOLVE and sometimes directly call APIs the WINAPI_FAMILY_APP partition
# hides, e.g. AddVectoredExceptionHandler, mmdeviceapi types). AppContainer
# enforcement at RUNTIME is the point of the probe anyway, and that comes from
# the PE's /APPCONTAINER flag + the manifest + WindowsApp.lib - not from
# header-level visibility. A header-hidden API that is unreachable in the
# sandbox will simply return a runtime error, which the probe records as
# DENIED/MISSING - exactly what we want to measure.
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
    _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

# The shared probe-core builds with the same compile flags (because we
# add_subdirectory'd it under this project's flag scope). Its /EHa and other
# invariants are kept inside its own CMakeLists.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../probe-core probe-core-build)

add_executable(probe_uwp main.cpp)
target_link_libraries(probe_uwp PRIVATE
    probe_core
    WindowsApp     # umbrella import lib for all AppContainer-permitted exports
    d3d11 d2d1 dwrite dxguid
    runtimeobject  # Ro* ABI entry points
)
# AppContainer PE flag + WIN32 subsystem + CoreApplication-friendly entry.
# Scoped to the EXE only - a shared library would reject /ENTRY:wWinMainCRTStartup.
target_link_options(probe_uwp PRIVATE
    /APPCONTAINER /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
)
set_target_properties(probe_uwp PROPERTIES
    OUTPUT_NAME "xbox-probe-uwp"
)

# Payload DLL shipped inside the package, used by category A for the
# out-of-package LoadLibrary + SEC_IMAGE tests. Also store-CRT, AppContainer.
add_library(probe_payload_uwp SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../probe-core/probe_payload.cpp)
target_link_libraries(probe_payload_uwp PRIVATE WindowsApp)
set_target_properties(probe_payload_uwp PROPERTIES
    OUTPUT_NAME "probe_payload"
    LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:probe_uwp>
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:probe_uwp>
)
add_dependencies(probe_uwp probe_payload_uwp)
