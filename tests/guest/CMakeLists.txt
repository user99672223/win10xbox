# Guest test programs: real Windows PE executables/DLLs that the layer loads.
# They must not link to the host's core; they are plain Win32 programs built
# with the same toolchain (MSVC on CI, llvm-mingw locally) and placed in
# bin/guest/. The C runtime is linked statically where the toolchain allows it
# (MSVC /MT via CMAKE_MSVC_RUNTIME_LIBRARY in the top-level file; llvm-mingw
# -static covers libgcc only, its UCRT is imported as api-ms-win-crt-*.dll).
# Owned by the loader work; see tests/guest/README.md.
set(WIN10XBOX_GUEST_DIR ${CMAKE_BINARY_DIR}/bin/guest)

function(win10xbox_add_guest name)
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${WIN10XBOX_GUEST_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${WIN10XBOX_GUEST_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/guest
    PREFIX "")
endfunction()

add_executable(guest_hello hello.c)
win10xbox_add_guest(guest_hello)

add_executable(guest_cpuid cpuid.c)
win10xbox_add_guest(guest_cpuid)

add_executable(guest_sleep sleep.c)
win10xbox_add_guest(guest_sleep)

add_library(guest_dll SHARED dll.c)
win10xbox_add_guest(guest_dll)

# Smoke-run the guests natively (under Wine for the llvm-mingw cross build).
add_test(NAME guest.hello COMMAND guest_hello)
set_tests_properties(guest.hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from guest")
add_test(NAME guest.cpuid COMMAND guest_cpuid)
set_tests_properties(guest.cpuid PROPERTIES PASS_REGULAR_EXPRESSION "cpuid.1.ecx=[0-9a-f]+")
add_test(NAME guest.sleep COMMAND guest_sleep)
set_tests_properties(guest.sleep PROPERTIES PASS_REGULAR_EXPRESSION "guest slept")

# The host-uwp-lite smoke guest (host-uwp-lite/smoke_guest.c, the program that
# ran on the console), built here too so that every job that stages bin/guest
# runs it: guest_smoke_cui.exe is the console variant (smoke_cui.exe there),
# guest_smoke.exe the /SUBSYSTEM:WINDOWS one (smoke.exe). Both print facts and
# return 42 (integration.guest_smoke_* in tests/integration). MSVC only: the
# file uses SEH __try/__except, which clang does not accept for the mingw target.
if(MSVC)
  add_executable(guest_smoke_cui ${CMAKE_SOURCE_DIR}/host-uwp-lite/smoke_guest.c)
  win10xbox_add_guest(guest_smoke_cui)
  add_executable(guest_smoke WIN32 ${CMAKE_SOURCE_DIR}/host-uwp-lite/smoke_guest.c)
  target_compile_definitions(guest_smoke PRIVATE SMOKE_GUI=1)
  win10xbox_add_guest(guest_smoke)
endif()
