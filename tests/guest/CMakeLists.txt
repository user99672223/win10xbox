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

# The machine identity through every path a program can read it from (D18): CPUID, registry, SMBIOS, WMI, version.
# It carries a Windows 10 compatibility manifest, or GetVersionExW and VerifyVersionInfoW answer 6.2.9200 as they do for
# any unmanifested program on Windows 10: MSVC merges the .manifest file, windres compiles it from identity.rc.
if(MSVC)
  add_executable(guest_identity identity.c identity.manifest)
else()
  enable_language(RC)
  add_executable(guest_identity identity.c identity.rc)
endif()
target_link_libraries(guest_identity PRIVATE ole32 oleaut32 advapi32)
win10xbox_add_guest(guest_identity)

add_executable(guest_sleep sleep.c)
win10xbox_add_guest(guest_sleep)

# CreateProcess through the layer (NtCreateUserProcess): runs guest_hello.exe and itself as children.
add_executable(guest_spawn spawn.c)
win10xbox_add_guest(guest_spawn)

# Registry writes outlive the process and reach a child (docs/DISPATCHER.md "Registry persistence"): run twice.
add_executable(guest_regpersist regpersist.c)
target_link_libraries(guest_regpersist PRIVATE advapi32)
win10xbox_add_guest(guest_regpersist)

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
# The non-client guest (tests/guest/window_nc.c): frame geometry, hit testing, caption drag and the close button,
# driven by the host's injected input.
add_executable(guest_window_nc window_nc.c)
target_link_libraries(guest_window_nc PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_nc)
# Input semantics beyond the plain sequence (tests/guest/window_input2.c): hit test / cursor / activation
# sequencing, double clicks, capture, children, the keyboard state and the US layout. Host-injected (the same hook) plus
# SendInput from the guest.
add_executable(guest_window_input2 window_input2.c)
target_link_libraries(guest_window_input2 PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_input2)
# Hooks and timers (tests/guest/window_hooks.c): thread-local WH_MOUSE / WH_KEYBOARD / WH_GETMESSAGE hooks fed by
# host-injected input (WIN10XBOX_TEST_INJECT_INPUT), and SetTimer cadence measured with GetTickCount64.
add_executable(guest_window_hooks window_hooks.c)
target_link_libraries(guest_window_hooks PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_hooks)
# The text guest: TextOutW/DrawTextW with the stock system font, glyph pixels read back with GetPixel
# (tests/guest/window_text.c). Its integration test is DISABLED until the text rendering slice lands.
add_executable(guest_window_text window_text.c)
target_link_libraries(guest_window_text PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_text)

# msctf.dll stand-in for the native-GUI machine folder (tests/guest/msctf_stub.c); staged by prepare_machine.cmake into
# its Windows/System32 only, so no other machine sees it.
add_library(msctf_stub SHARED msctf_stub.c)
set_target_properties(msctf_stub PROPERTIES OUTPUT_NAME msctf PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/native-gui-stubs
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/native-gui-stubs
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/guest)

# GDI bitmap operations (tests/guest/gdi_bitmap.c): DIB sections, BitBlt/StretchBlt between memory and window DCs,
# AlphaBlend/TransparentBlt through msimg32, LoadImage of the BITMAP resources of gdi_bitmap.rc (a generated copy of
# gdi_bitmap.rc.in whose .bmp paths are absolute: windres and rc.exe resolve relative names differently).
set(W10X_BMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gdi_bitmap_res)
configure_file(gdi_bitmap.rc.in ${CMAKE_CURRENT_BINARY_DIR}/gdi_bitmap.rc @ONLY)
add_executable(guest_gdi_bitmap gdi_bitmap.c ${CMAKE_CURRENT_BINARY_DIR}/gdi_bitmap.rc)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/gdi_bitmap.rc PROPERTIES
  OBJECT_DEPENDS "${W10X_BMP_DIR}/rgb24.bmp;${W10X_BMP_DIR}/pal8.bmp")
target_link_libraries(guest_gdi_bitmap PRIVATE user32 gdi32 msimg32)
win10xbox_add_guest(guest_gdi_bitmap)
# Menus (tests/guest/window_menu.c): the menu API, the menu bar of a window, popup menus and TrackPopupMenu, driven by
# host-injected clicks (WIN10XBOX_TEST_INJECT_INPUT, tests/integration/CMakeLists.txt). Like window_input it cannot do
# anything useful without that injection, so there is no native smoke test.
add_executable(guest_window_menu window_menu.c)
target_link_libraries(guest_window_menu PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_menu)
# Cursors and icons (tests/guest/cursor.c): LoadCursor of Wine's own IDC_ARROW / IDC_IBEAM, GetIconInfo, GetCursorInfo,
# SetCursor, ShowCursor, DrawIconEx, and the cursor composed onto the screen, which the host looks at with screenshots
# taken by the inject script. Nothing to see without that injection, so there is no native smoke test.
add_executable(guest_cursor cursor.c)
target_link_libraries(guest_cursor PRIVATE user32 gdi32)
win10xbox_add_guest(guest_cursor)
# The XInput guest (tests/guest/xinput.c): imports xinput1_4.dll (guest-dlls/xinput, our own builtin DLL) and reads the
# pad state the host holds, which host-win32's WIN10XBOX_TEST_INJECT_INPUT "pad" command sets. Nothing to see without
# that injection, so there is no native smoke test.
add_executable(guest_xinput xinput.c)
target_link_libraries(guest_xinput PRIVATE xinput1_4 user32)
win10xbox_add_guest(guest_xinput)
# Exception handling coverage (seh.cpp + the DLL it throws across): C++ throw/catch, SEH, vectored handlers.
# __try/__except is native on MSVC. For clang's mingw target it is used when a configure-time compile+link of a
# __try/__except program with -fms-extensions succeeds (SEH_NATIVE); otherwise seh.cpp falls back to a vectored
# handler that restores a captured context.
add_library(guest_seh_dll SHARED seh_dll.cpp)
win10xbox_add_guest(guest_seh_dll)
add_executable(guest_seh seh.cpp)
target_link_libraries(guest_seh PRIVATE guest_seh_dll)
win10xbox_add_guest(guest_seh)
if(NOT MSVC)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fms-extensions -g")
  # The probe mirrors seh.cpp's SehRun (a filter that calls a function, the result passed back by value) so a
  # compiler that cannot handle the construct is detected here and not in the guest build.
  check_cxx_source_compiles("
    #include <windows.h>
    struct R { int a; unsigned long b; };
    static R* g_cur; static bool g_flag;
    static LONG filt(EXCEPTION_POINTERS* ep) { g_cur->b = ep->ExceptionRecord->ExceptionCode; return EXCEPTION_EXECUTE_HANDLER; }
    static R run(void (*fn)(void*), void* arg, bool flag) {
      R r = {}; g_cur = &r; g_flag = flag;
      __try { fn(arg); } __except (filt(GetExceptionInformation())) { }
      g_cur = 0; return r;
    }
    static void thrower(void*) { RaiseException(1, 0, 0, 0); }
    int main() { R r = run(thrower, 0, false); return r.b == 1 ? 0 : 1; }"
    WIN10XBOX_CLANG_SEH_TRY)
  unset(CMAKE_REQUIRED_FLAGS)
  if(WIN10XBOX_CLANG_SEH_TRY)
    target_compile_options(guest_seh PRIVATE -fms-extensions)
    target_compile_definitions(guest_seh PRIVATE SEH_NATIVE=1)
  endif()
endif()
# The scroll bar guest (tests/guest/window_scroll.c): SetScrollInfo/GetScrollInfo, non-client scroll bars, ScrollWindowEx,
# ScrollDC, and a click injected by the host on the vertical bar's down arrow (WM_VSCROLL SB_LINEDOWN).
add_executable(guest_window_scroll window_scroll.c)
target_link_libraries(guest_window_scroll PRIVATE user32 gdi32)
win10xbox_add_guest(guest_window_scroll)
# The clipboard guest (tests/guest/clipboard.c): user32 clipboard API -> win32u syscalls -> core/user/win_clipboard.cpp.
add_executable(guest_clipboard clipboard.c)
target_link_libraries(guest_clipboard PRIVATE user32)
win10xbox_add_guest(guest_clipboard)
# The dialog guest: DialogBoxParamW with a DIALOG resource (dialog.rc); the host clicks OK (tests/guest/dialog.c).
enable_language(RC)
add_executable(guest_dialog dialog.c dialog.rc)
target_link_libraries(guest_dialog PRIVATE user32 gdi32)
win10xbox_add_guest(guest_dialog)
# Wine's builtin comctl32 (ListView/TreeView/Progress/Trackbar/StatusBar) through the layer (tests/guest/comctl.c).
add_executable(guest_comctl comctl.c)
target_link_libraries(guest_comctl PRIVATE user32 gdi32 comctl32)
win10xbox_add_guest(guest_comctl)

# Wine __TRY frames (TEB.NtTib.ExceptionList) reached by an exception on the main thread and on worker threads
# (tests/guest/tebframe.c; core/dispatch/teb_frames.cpp).
add_executable(guest_tebframe tebframe.c)
win10xbox_add_guest(guest_tebframe)

# The networking guest (tests/guest/net.c): Wine's ws2_32 -> \\Device\\Afd -> core/afd -> host Winsock, over loopback.
add_executable(guest_net net.c)
target_link_libraries(guest_net PRIVATE ws2_32)
win10xbox_add_guest(guest_net)

# Smoke-run the guests natively (under Wine for the llvm-mingw cross build).
add_test(NAME guest.seh COMMAND guest_seh)
set_tests_properties(guest.seh PROPERTIES PASS_REGULAR_EXPRESSION "seh: done failures=0"
  FAIL_REGULAR_EXPRESSION "seh: [a-z_.]+ FAILED;seh: FAIL line")
add_test(NAME guest.tebframe COMMAND guest_tebframe)
set_tests_properties(guest.tebframe PROPERTIES PASS_REGULAR_EXPRESSION "tebframe: done failures=0"
  FAIL_REGULAR_EXPRESSION "tebframe: FAIL line")
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

# Guests from the SQLite shell bring-up (tests/guest/sqlite).
add_subdirectory(sqlite)
# Direct3D 11 off-screen (tests/guest/d3d11_tri.c, docs/slices/d3d.md): the guest loads the NATIVE d3d11/dxgi/d3dcompiler_47
# at run time (LoadLibrary), renders a triangle into a render-target texture and reads the pixels back. MSVC only (the
# Windows SDK's d3d11.h/dxguid.lib); run on the route-B + D3D machine folder (integration.guest_d3d11).
if(MSVC)
  add_executable(guest_d3d11_tri d3d11_tri.c)
  target_link_libraries(guest_d3d11_tri PRIVATE dxguid)
  win10xbox_add_guest(guest_d3d11_tri)
  # The control: the same program run natively on the runner, outside the layer (exit 42 = it renders). Not for the
  # emulator build; its stdout also lists the runner's modules, i.e. the DLL closure of d3d11 (docs/slices/d3d.md).
  if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
    add_test(NAME guest.d3d11 COMMAND guest_d3d11_tri)
    set_tests_properties(guest.d3d11 PROPERTIES PASS_REGULAR_EXPRESSION "d3d11: done" TIMEOUT 120)
  endif()
endif()

# Dear ImGui's example_win32_directx11, unmodified, as a guest of route B + Direct3D 11 (tests/guest/imgui_d3d11.cpp
# wraps it; docs/DECISIONS.md G1). The Dear ImGui release source is not vendored: CI fetches it on the Windows runner
# (ci.yml, "Fetch Dear ImGui") and passes its root as WIN10XBOX_IMGUI_DIR (environment or cache). MSVC only, like
# the example's own build_win32.bat; without the source the target and its test do not exist.
set(WIN10XBOX_IMGUI_DIR "$ENV{WIN10XBOX_IMGUI_DIR}" CACHE PATH "Dear ImGui release source root (guest_imgui_d3d11)")
if(MSVC AND WIN10XBOX_IMGUI_DIR AND EXISTS "${WIN10XBOX_IMGUI_DIR}/examples/example_win32_directx11/main.cpp")
  set(_im "${WIN10XBOX_IMGUI_DIR}")
  add_executable(guest_imgui_d3d11 imgui_d3d11.cpp
    ${_im}/imgui.cpp ${_im}/imgui_demo.cpp ${_im}/imgui_draw.cpp ${_im}/imgui_tables.cpp ${_im}/imgui_widgets.cpp
    ${_im}/backends/imgui_impl_dx11.cpp ${_im}/backends/imgui_impl_win32.cpp)
  target_include_directories(guest_imgui_d3d11 PRIVATE ${_im} ${_im}/backends)
  target_compile_definitions(guest_imgui_d3d11 PRIVATE UNICODE _UNICODE)
  target_compile_options(guest_imgui_d3d11 PRIVATE /W3)   # third-party code: not held to the project's /W4
  target_link_libraries(guest_imgui_d3d11 PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 imm32)
  win10xbox_add_guest(guest_imgui_d3d11)
  set(WIN10XBOX_HAVE_IMGUI_GUEST ON CACHE INTERNAL "guest_imgui_d3d11 is built")
  message(STATUS "guest_imgui_d3d11: Dear ImGui from ${_im}")
else()
  set(WIN10XBOX_HAVE_IMGUI_GUEST OFF CACHE INTERNAL "guest_imgui_d3d11 is built")
endif()
