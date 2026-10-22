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

# The input guest (tests/guest/window_input.c): logs the input messages the host injects with
# WIN10XBOX_TEST_INJECT_INPUT. It cannot do anything useful without that injection, so there is no native smoke test.
add_executable(guest_window_input window_input.c)
target_link_libraries(guest_window_input PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_input)
# The text guest: TextOutW/DrawTextW with the stock system font, glyph pixels read back with GetPixel
# (tests/guest/window_text.c). Its integration test is DISABLED until the text rendering slice lands.
add_executable(guest_window_text window_text.c)
target_link_libraries(guest_window_text PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_text)

# The clipboard guest (tests/guest/clipboard.c): user32 clipboard API -> win32u syscalls -> core/user/win_clipboard.cpp.
add_executable(guest_clipboard clipboard.c)
target_link_libraries(guest_clipboard PRIVATE user32)
win10xbox_add_guest(guest_clipboard)

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

# Native smoke of the GUI guest: meaningful on a Windows runner (real win32k); under Wine on a display-less
# Linux box CreateWindowExW fails, so it is not registered for the emulator build.
if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
  add_test(NAME guest.window COMMAND guest_window)
  set_tests_properties(guest.window PROPERTIES PASS_REGULAR_EXPRESSION "window: done")
  add_test(NAME guest.window2 COMMAND guest_window2)
  set_tests_properties(guest.window2 PROPERTIES PASS_REGULAR_EXPRESSION "window2: done")
  add_test(NAME guest.window_text COMMAND guest_window_text)
  set_tests_properties(guest.window_text PROPERTIES PASS_REGULAR_EXPRESSION "text: done")
endif()
