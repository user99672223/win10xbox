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

add_library(guest_dll SHARED dll.c)
win10xbox_add_guest(guest_dll)

# Smoke-run the guests natively (under Wine for the llvm-mingw cross build).
add_test(NAME guest.hello COMMAND guest_hello)
set_tests_properties(guest.hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from guest")
add_test(NAME guest.cpuid COMMAND guest_cpuid)
set_tests_properties(guest.cpuid PROPERTIES PASS_REGULAR_EXPRESSION "cpuid.1.ecx=[0-9a-f]+")
