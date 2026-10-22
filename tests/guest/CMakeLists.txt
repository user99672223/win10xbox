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

# The first GUI guest: user32/gdi32 -> win32u syscalls -> core/user (docs/GUI.md).
# A console-subsystem program on purpose: its stdout is the test's evidence.
add_executable(guest_window window.c)
target_link_libraries(guest_window PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window)

# The second GUI guest: an overlapped window, double-buffered painting, timers, PeekMessage, DestroyWindow.
add_executable(guest_window2 window2.c)
target_link_libraries(guest_window2 PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window2)

# Smoke-run the guests natively (under Wine for the llvm-mingw cross build).
add_test(NAME guest.hello COMMAND guest_hello)
set_tests_properties(guest.hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from guest")
add_test(NAME guest.cpuid COMMAND guest_cpuid)
set_tests_properties(guest.cpuid PROPERTIES PASS_REGULAR_EXPRESSION "cpuid.1.ecx=[0-9a-f]+")
add_test(NAME guest.sleep COMMAND guest_sleep)
set_tests_properties(guest.sleep PROPERTIES PASS_REGULAR_EXPRESSION "guest slept")
# Native smoke of the GUI guest: meaningful on a Windows runner (real win32k); under Wine on a display-less
# Linux box CreateWindowExW fails, so it is not registered for the emulator build.
if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
  add_test(NAME guest.window COMMAND guest_window)
  set_tests_properties(guest.window PROPERTIES PASS_REGULAR_EXPRESSION "window: done")
  add_test(NAME guest.window2 COMMAND guest_window2)
  set_tests_properties(guest.window2 PROPERTIES PASS_REGULAR_EXPRESSION "window2: done")
endif()
