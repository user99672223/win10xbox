cmake_minimum_required(VERSION 3.25)
project(win10xbox LANGUAGES C CXX VERSION 0.0.1)

# /Z7 (embedded debug info) instead of /Zi: parallel cl.exe + sccache cannot
# share a .pdb, and sccache cannot cache /Zi objects at all.
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

if(NOT WIN32)
  message(FATAL_ERROR "win10xbox builds Windows x64 binaries only. On Linux use -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 only (use -A x64 with Visual Studio generators)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(WIN10XBOX_BUILD_TESTS   "Build unit + integration tests" ON)
option(WIN10XBOX_BUILD_UWP     "Build the UWP host (needs MSVC + Windows SDK)" OFF)
option(WIN10XBOX_BUILD_DRIVERS "Build the driver host (fake ntoskrnl, test drivers)" ON)
option(WIN10XBOX_TRACE_DEFAULT_ON "Tracing on by default (always on in Debug)" OFF)

if(MSVC)
  # Scope C/C++ flags to those languages: ml64 (ASM_MASM) rejects /W4, /EHsc,
  # /Zc:*, /permissive-, /utf-8 (warnings A4008/A4018) and then fails to emit
  # the .obj, breaking the link of the asm syscall entry stub.
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/permissive-;/Zc:__cplusplus;/utf-8;/EHsc>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # The loader patches code in mapped images; keep the CRT static so the host
  # has no dependency on a runtime DLL that the guest might also map.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers>")
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  # llvm-mingw: static libc++ and winpthreads so test binaries run under Wine
  # without shipping runtime DLLs next to them.
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)

if(MSVC)
  enable_language(ASM_MASM)
else()
  enable_language(ASM)
endif()

add_subdirectory(third_party)
add_subdirectory(core)
add_subdirectory(host-win32)
if(WIN10XBOX_BUILD_DRIVERS)
  add_subdirectory(drivers)
endif()
add_subdirectory(winexbox)
if(WIN10XBOX_BUILD_UWP)
  add_subdirectory(host-uwp)
endif()
if(WIN10XBOX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
