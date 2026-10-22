# Integration tests: real EXEs run through host-win32.
add_test(NAME integration.host_hello COMMAND win10xbox)
set_tests_properties(integration.host_hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from host-win32")
add_test(NAME integration.host_version COMMAND win10xbox --version)
set_tests_properties(integration.host_version PROPERTIES PASS_REGULAR_EXPRESSION "^win10xbox [0-9]")

# ---- Guest programs through the full bootstrap (process::Launch) ------------
# Machine folder: <bin>/machine, prepared by prepare_machine.cmake (layout in
# that file and docs/DISPATCHER.md). Wine builtins come from
# WIN10XBOX_BUILTIN_DIR (default: the local wine-pe build output); CI points
# it at the downloaded wine-pe artifact. Without builtins the guest tests SKIP.
set(WIN10XBOX_MACHINE_DIR ${CMAKE_BINARY_DIR}/bin/machine CACHE PATH "Machine folder for integration tests")
set(WIN10XBOX_BUILTIN_DIR ${CMAKE_SOURCE_DIR}/wine-pe/out/builtin CACHE PATH "Wine PE builtin DLLs for integration tests")
# The GUI tests run on a second folder with the console's DLL set (builtins + NLS, "*": "builtin,native"); see
# prepare_machine.cmake (BUILTIN_ONLY_MACHINE) and docs/TESTING.md for why.
set(WIN10XBOX_BUILTIN_MACHINE_DIR ${CMAKE_BINARY_DIR}/bin/machine-builtin)
set(_default_json ${CMAKE_SOURCE_DIR}/machine/machine.default.json)
set(_prepare_args
  -DMACHINE=${WIN10XBOX_MACHINE_DIR}
  -DBUILTIN_ONLY_MACHINE=${WIN10XBOX_BUILTIN_MACHINE_DIR}
  -DBUILTIN_DIR=${WIN10XBOX_BUILTIN_DIR}
  -DGUEST_DIR=${CMAKE_BINARY_DIR}/bin/guest
  -DDEFAULT_JSON=${_default_json}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/prepare_machine.cmake)

add_custom_target(prepare_machine COMMAND ${CMAKE_COMMAND} ${_prepare_args}
  COMMENT "Preparing integration machine folder ${WIN10XBOX_MACHINE_DIR}" VERBATIM)
if(TARGET guest_hello)
  add_dependencies(prepare_machine guest_hello guest_cpuid guest_sleep guest_window guest_window2 guest_window_input
                   guest_window_input2 guest_window_text)
endif()
add_test(NAME integration.prepare_machine COMMAND ${CMAKE_COMMAND} ${_prepare_args})
set_tests_properties(integration.prepare_machine PROPERTIES FIXTURES_SETUP w10x_machine)

# Vendor the guest must see: machine.json "cpu.vendor" (DECISIONS.md D10).
file(READ ${_default_json} _json)
string(REGEX MATCH "\"vendor\"[ \t]*:[ \t]*\"([^\"]+)\"" _m "${_json}")
set(_vendor "${CMAKE_MATCH_1}")
if(NOT _vendor)
  set(_vendor "[A-Za-z]+")
endif()

# The machine folder is WIN10XBOX_MACHINE_DIR unless the caller sets _guest_machine.
function(win10xbox_guest_test name program exit_code)
  # On the builtin-only folder, also prove from the modules trace that no native system DLL was mapped.
  set(_require_builtin OFF)
  if(NOT _guest_machine)
    set(_guest_machine ${WIN10XBOX_MACHINE_DIR})
  elseif(_guest_machine STREQUAL WIN10XBOX_BUILTIN_MACHINE_DIR)
    set(_require_builtin ON)
  endif()
  add_test(NAME integration.${name}
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox>
      -DMACHINE=${_guest_machine}
      -DREQUIRE_BUILTIN_DLLS=${_require_builtin}
      -DPROGRAM=${program}
      -DEXPECT_EXIT=${exit_code}
      "-DEXPECT_REGEX=${ARGN}"
      -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
  set_tests_properties(integration.${name} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "guest_e2e"
    TIMEOUT 180)
endfunction()

win10xbox_guest_test(guest_hello "C:\\guest\\guest_hello.exe" 42 "hello from guest")
# CPUID.1:ECX bit 31 (hypervisor present) clear: first hex digit 0-7.
win10xbox_guest_test(guest_cpuid "C:\\guest\\guest_cpuid.exe" 0
  "cpuid\\.1\\.ecx=[0-7][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]" "vendor=${_vendor}")

# Regression guard for the Xbox kernel32-forwarder bug. WIN10XBOX_TEST_NO_HOST_PATHS
# makes HostPathOfHandle fail the way GetFinalPathNameByHandleW does inside the
# console's AppContainer, so image identity falls to SameImageFile's content
# comparison instead of host-path equality. Before the fix, empty==empty aliased
# every Wine builtin to ntdll, kernelbase never really loaded, and the guest died
# with 92 unresolved KERNEL32 imports; this proves kernelbase still loads and the
# guest runs. Reproduces the on-console failure condition under Wine, so it gates here.
# Regression guard for the lazily-resolved host export. After the PEB take-over
# GetModuleHandleW(L"ntdll.dll") answers with the GUEST ntdll, so the clock
# module's "host" NtDelayExecution used to resolve to the guest's own rewritten
# syscall stub and Clk_NtDelayExecution recursed until the stack ran out. No
# other guest sleeps, so nothing exercised it.
win10xbox_guest_test(guest_sleep "C:\\guest\\guest_sleep.exe" 7 "guest slept")

# The host-uwp-lite smoke guest (built in tests/guest, MSVC only): the program
# that ran on the console, in both subsystems. It prints facts and returns 42.
if(TARGET guest_smoke_cui)
  win10xbox_guest_test(guest_smoke_cui "C:\\guest\\guest_smoke_cui.exe" 42 "hello from guest" "guest: done, returning 42")
  win10xbox_guest_test(guest_smoke_gui "C:\\guest\\guest_smoke.exe" 42 "hello from guest" "guest: done, returning 42")
endif()

# The first pixels: a GUI guest (tests/guest/window.c) creates a window, paints it solid blue with a red square
# through Wine's user32/gdi32 and the win32u syscalls emulated by core/user, reads its own pixels back with
# GetPixel, and exits 42. The host writes the composed virtual screen to a .bmp (WIN10XBOX_SCREENSHOT), and
# run_guest.cmake checks two pixels of that file too: the window at (100,100)-(500,400) must show the blue at
# screen (110,110) and the red at (130,130), as 0xAARRGGBB read from the BMP's B,G,R,A bytes (RGB(0,80,220) is
# ff0050dc, RGB(255,0,0) is ffff0000). The screenshot is the artifact to look at when this fails.
# Both GUI tests run on the builtin-only machine folder (the console's DLL set).
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
win10xbox_guest_test(guest_window "C:\\guest\\guest_window.exe" 42
  "window: class registered" "window: created hwnd=" "window: painted"
  "window: pixel\\(10,10\\)=00dc5000 pixel\\(30,30\\)=000000ff" "window: done")
set_tests_properties(integration.guest_window PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=110,110,ff0050dc|130,130,ffff0000")

# The second GUI guest (tests/guest/window2.c): an overlapped window with a frame, double-buffered painting
# through a memory DC, a timer that repaints and then destroys the window, a PeekMessage loop on
# MsgWaitForMultipleObjectsEx. It checks itself (exit 42); the screenshot is the last frame it painted: the
# client area at screen (58,91) shows the green and the yellow square, the caption bar the active caption colour.
win10xbox_guest_test(guest_window2 "C:\\guest\\guest_window2.exe" 42
  "window2: client=320x200" "window2: text=win10xbox two"
  "window2: pixel\\(5,5\\)=0000ff00 pixel\\(20,20\\)=0000ffff"
  "window2: timer=2 destroy=1 ncdestroy=1 iswindow=0" "window2: done")
set_tests_properties(integration.guest_window2 PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window2.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=63,96,ff00ff00|78,111,ffffff00|100,75,ff99b4d1")

# The input path (tests/guest/window_input.c): the host injects a scripted mouse and keyboard (WIN10XBOX_TEST_INJECT_INPUT,
# host-win32/main.cpp), and the guest logs each message it receives and exits 42 once the whole sequence has arrived in
# order. Input delivery is part of core/user (win_message.cpp), so this gates.
win10xbox_guest_test(guest_window_input "C:\\guest\\guest_window_input.exe" 42
  "input: WM_LBUTTONDOWN x=50 y=50" "input: WM_CHAR ch=a" "input: WM_MOUSEWHEEL delta=120")
set_tests_properties(integration.guest_window_input PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_INJECT_INPUT=sleep 1500\\;move 150,150\\;ldown\\;lup\\;key 0x41\\;wheel 120")
# Input semantics (tests/guest/window_input2.c): the host script covers the driver path (move, double click, right
# click, wheel, key; the guest compares every message with its hit test / WM_SETCURSOR companions), the guest the rest with
# SendInput. It prints PASS/FAIL per case and exits 42 only when every case passed.
win10xbox_guest_test(guest_window_input2 "C:\\guest\\guest_window_input2.exe" 42
  "PASS host-injected move, double click" "PASS caps lock on" "PASS click on an inactive window" "PASS capture: a click outside"
  "PASS child: click" "PASS US layout: VkKeyScan" "window_input2: [0-9]+ passed, 0 failed")
set_tests_properties(integration.guest_window_input2 PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_INJECT_INPUT=sleep 3000\\;move 150,150\\;ldown\\;lup\\;ldown\\;lup\\;rdown\\;rup\\;wheel -120\\;key 0x42"
  FAIL_REGULAR_EXPRESSION "FAIL ")
# The text GUI guest (tests/guest/window_text.c): a 400x300 WS_POPUP window at (100,100), client origin = screen
# origin (100,100). It draws "HI" at client (20,20) and "Hello/Xbox" into (20,60)-(380,120) with the stock system
# font, then reads the glyph pixels back with GetPixel and exits 42. The screenshot checks three of them at screen
# coordinates: H(1,3) black at (121,123), H(3,3) white at (123,123), I(3,3) black at (131,123) (I's cell starts at
# x offset 8).
win10xbox_guest_test(guest_window_text "C:\\guest\\guest_window_text.exe" 42
  "text: class registered" "text: created hwnd=" "text: painted"
  "text: pixels H\\(1,3\\)=00000000 H\\(6,3\\)=00000000 H\\(3,3\\)=00ffffff I\\(3,3\\)=00000000 bg=00ffffff"
  "text: done")
set_tests_properties(integration.guest_window_text PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window_text.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=121,123,ff000000|123,123,ffffffff|131,123,ff000000")
unset(_guest_machine)

win10xbox_guest_test(guest_hello_appcontainer_sim "C:\\guest\\guest_hello.exe" 42 "hello from guest")
set_tests_properties(integration.guest_hello_appcontainer_sim PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_NO_HOST_PATHS=1")

# ---- TEB.TlsSlots: our ThreadState slot vs the guest's TLS allocator --------
# TEB.TlsSlots is an inline array in the TEB, so unlike ThreadLocalStoragePointer
# and FlsData it cannot be given separate host and guest copies: our ThreadState
# pointer and the guest's TlsAlloc share it. These tests assert through the real
# takeover path (process::Launch -> loader_init -> guest entry) rather than
# unit-testing a helper. See "The TLS slot we hold" in docs/DISPATCHER.md.
#   name / program / exit code / WIN10XBOX_DEBUG spec / extra env /
#   trace regexes to require (list) / trace regex to forbid
function(win10xbox_guest_tls_test name exit_code debug_spec env expect_trace forbid_trace)
  add_test(NAME integration.${name}
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox>
      -DMACHINE=${WIN10XBOX_MACHINE_DIR}
      -DPROGRAM=C:\\guest\\guest_hello.exe
      -DEXPECT_EXIT=${exit_code}
      "-DEXPECT_REGEX=hello from guest"
      "-DEXPECT_TRACE_REGEX=${expect_trace}"
      "-DFORBID_TRACE_REGEX=${forbid_trace}"
      -DDEBUG_SPEC=${debug_spec}
      -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
  set_tests_properties(integration.${name} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "guest_e2e"
    TIMEOUT 180)
  if(env)
    set_tests_properties(integration.${name} PROPERTIES ENVIRONMENT "${env}")
  endif()
endfunction()

# 1. The slot survives the whole bootstrap. Requires that the index is logged at
# all (it was not before: a collision was undiagnosable from any host log), that
# the index is a safe one, that the reservation against the guest's allocator
# ran, and that the slot still holds our ThreadState once Wine's loader_init is
# done and the guest is about to start.
win10xbox_guest_tls_test(guest_tls_slot_intact 42 "+dispatch,+modules" ""
  "ThreadState in TEB\\.TlsSlots\\[[0-9]+\\];guest TLS bitmap .* slot [0-9]+ already in use;guest entry: TEB\\.TlsSlots\\[[0-9]+\\] still holds our ThreadState"
  "UNSAFE|no longer holds our ThreadState|was overwritten")

# 2. A guest that takes our slot is detected, not trusted. WIN10XBOX_TEST_TLS_CLOBBER
# writes a 4-byte 0 through &TEB.TlsSlots[index] at guest entry, byte for byte
# what `errno = 0` through Wine's _errno() does (wine-src/dlls/ntdll/thread.c:445).
# That leaves our pointer NON-NULL with its low dword gone, which the old
# `test rbx,rbx` in core/asm passed straight through: without the slot-tag check
# this run segfaults (exit 139, the signature reported from real Windows) with no
# trace output after the guest-entry line. With it, the clobber is counted,
# reported and repaired, and the guest still runs to completion.
win10xbox_guest_tls_test(guest_tls_slot_clobber 42 "+dispatch,+modules" "WIN10XBOX_TEST_TLS_CLOBBER=1"
  "simulating a guest errno store;was overwritten \\([0-9]+ times\\): the guest is using our slot; restored it"
  "")

# 3. The index guard. WIN10XBOX_TEST_TLS_INDEX asks for NTDLL_TLS_ERRNO (16),
# which Wine's loader_init claims by fiat rather than through the shared PEB
# bitmap (wine-src/dlls/ntdll/loader.c:4455); Initialize must refuse it and stay
# on a safe slot. Under Wine the host ntdll IS Wine's, so its own loader_init
# reserved 16 before our TlsAlloc ran and TlsAlloc can never return it -- the
# hook is the only way to exercise the case a real-Windows host can reach.
win10xbox_guest_tls_test(guest_tls_reserved_index 42 "+dispatch,+modules" "WIN10XBOX_TEST_TLS_INDEX=16"
  "WIN10XBOX_TEST_TLS_INDEX=16 rejected.*staying on slot [0-9]+"
  "ThreadState in TEB\\.TlsSlots\\[16\\]")

# ---- The guest-view window -------------------------------------------------
# Between LeaveToGuestView() and the guest resuming, the TEB shows the GUEST's
# ThreadLocalStoragePointer, which has no block for the host image. Host code
# touching ANY thread-local there faults -- and under MSVC that includes code
# with no thread_local in sight, because a function-local `static` emits an init
# guard that reads _Init_thread_epoch out of the host module's TLS block. That
# is how `static State s;` in dispatcher.cpp, reached through Host() from
# Xl_NtContinue, crashed real Windows while this job stayed green: mingw's
# __cxa_guard touches no TLS, so the FAULT is unreachable under Wine.
#
# The ORDERING is reachable under Wine, though, which is what these two gate.
# This is the only defence linux-cross can offer against this class.
win10xbox_guest_tls_test(guest_view_clean 42 "+dispatch,+modules" ""
  "guest entry reached"
  "host state reached under the guest TLS view")

win10xbox_guest_tls_test(guest_view_violation_detected 42 "+dispatch,+modules"
  "WIN10XBOX_TEST_GUEST_VIEW_VIOLATION=1"
  "host state reached under the guest TLS view [0-9]+ times \\(last: dispatch::ExportCache\\), [1-9][0-9]* trace lines dropped"
  "")

# ---- Real program: curl's curl.exe (docs/TESTING.md, "Real programs") ---------------------------------------------
# Built from the release tarball by tools/ci/build_curl.ps1 (CMake + MSVC, no TLS; a named step of ci.yml's windows
# job, which leaves it at <machine>/guest/curl.exe before ctest runs). Registered only where that step exists (MSVC) or
# with -DWIN10XBOX_REAL_CURL=ON after putting a curl.exe into the machine folder; never on linux-cross's skip floor.
# integration.real_curl: --version and the file:// handler (tests/integration/realcurl.cmake).
# integration.real_curl_http: HTTP against a host-side server (tests/integration/realcurl_http.py); needs the guest's
# ws2_32 -> AFD path, so it is registered only with -DWIN10XBOX_REAL_CURL_HTTP=ON until that lands.
option(WIN10XBOX_REAL_CURL "Register integration.real_curl (curl.exe must be in <machine>/guest)" OFF)
option(WIN10XBOX_REAL_CURL_HTTP "Register integration.real_curl_http (guest sockets through the layer)" OFF)
if(MSVC OR WIN10XBOX_REAL_CURL)
  add_test(NAME integration.real_curl
    COMMAND ${CMAKE_COMMAND} -DHOST=$<TARGET_FILE:win10xbox> -DMACHINE=${WIN10XBOX_MACHINE_DIR}
      -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR} -P ${CMAKE_CURRENT_SOURCE_DIR}/realcurl.cmake)
  set_tests_properties(integration.real_curl PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    LABELS "guest_e2e;realcurl"
    TIMEOUT 900)
  if(WIN10XBOX_REAL_CURL_HTTP)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_curl_emu "")
    if(CMAKE_CROSSCOMPILING_EMULATOR)
      set(_curl_emu --emulator ${CMAKE_CROSSCOMPILING_EMULATOR})
    endif()
    add_test(NAME integration.real_curl_http
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/realcurl_http.py
        --host $<TARGET_FILE:win10xbox> --machine ${WIN10XBOX_MACHINE_DIR} ${_curl_emu})
    set_tests_properties(integration.real_curl_http PROPERTIES
      FIXTURES_REQUIRED w10x_machine
      LABELS "guest_e2e;realcurl"
      TIMEOUT 1500)
  endif()
endif()
