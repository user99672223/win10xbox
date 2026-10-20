# win32-host: desktop baseline executable (environment 1).
# Configure this directory as its own CMake project so the shared probe-core is
# compiled with ordinary desktop flags (classic CRT, full Win32 API surface).
cmake_minimum_required(VERSION 3.20)
project(xbox_probe_win32 CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "build x64: use -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../probe-core probe-core-build)

add_executable(probe_win32 main.cpp)
target_link_libraries(probe_win32 PRIVATE
    probe_core
    advapi32 ole32 oleaut32 ws2_32 shlwapi
)
set_target_properties(probe_win32 PROPERTIES OUTPUT_NAME "xbox-probe-win32")
