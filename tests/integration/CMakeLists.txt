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
# Route B's folder: the same, plus the native GUI DLLs and "gui": "native" (prepare_machine.cmake NATIVE_GUI_MACHINE).
set(WIN10XBOX_NATIVE_GUI_MACHINE_DIR ${CMAKE_BINARY_DIR}/bin/machine-native-gui)
# Route B + Direct3D 11: the native-GUI set plus the native D3D runtime (prepare_machine.cmake D3D_MACHINE).
set(WIN10XBOX_D3D_MACHINE_DIR ${CMAKE_BINARY_DIR}/bin/machine-native-d3d)
set(_default_json ${CMAKE_SOURCE_DIR}/machine/machine.default.json)
set(_prepare_args
  -DMACHINE=${WIN10XBOX_MACHINE_DIR}
  -DBUILTIN_ONLY_MACHINE=${WIN10XBOX_BUILTIN_MACHINE_DIR}
  -DNATIVE_GUI_MACHINE=${WIN10XBOX_NATIVE_GUI_MACHINE_DIR}
  -DD3D_MACHINE=${WIN10XBOX_D3D_MACHINE_DIR}
  -DD3D_EXTRA_DIR=${CMAKE_BINARY_DIR}/bin/machine-d3d-extra
  -DBUILTIN_DIR=${WIN10XBOX_BUILTIN_DIR}
  -DGUEST_DIR=${CMAKE_BINARY_DIR}/bin/guest
  -DMSCTF_STUB=${CMAKE_BINARY_DIR}/bin/native-gui-stubs/msctf.dll
  -DGUEST_DLL_DIR=${WIN10XBOX_GUEST_DLL_DIR}
  -DDEFAULT_JSON=${_default_json}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/prepare_machine.cmake)

add_custom_target(prepare_machine COMMAND ${CMAKE_COMMAND} ${_prepare_args}
  COMMENT "Preparing integration machine folder ${WIN10XBOX_MACHINE_DIR}" VERBATIM)
if(TARGET guest_hello)
  add_dependencies(prepare_machine guest_hello guest_cpuid guest_sleep guest_nowine guest_spawn guest_named_objects
                   guest_window guest_window2 guest_window_input guest_window_input2 guest_window_text guest_window_nc
                   guest_dialog guest_comctl guest_seh guest_window_scroll guest_clipboard guest_net msctf_stub
                   guest_gdi_bitmap guest_window_hooks guest_window_menu guest_xinput xinput1_4 guest_identity
                   guest_cursor guest_regpersist guest_wasapi guest_xaudio2 guest_tebframe guest_rdrand)
  if(TARGET guest_d3d11_tri)
    add_dependencies(prepare_machine guest_d3d11_tri)
  endif()
  if(TARGET guest_d3d11_swap)
    add_dependencies(prepare_machine guest_d3d11_swap)
  endif()
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

# The machine folder is WIN10XBOX_MACHINE_DIR unless the caller sets _guest_machine; the guest's command-line
# arguments (a list, one element each) are _guest_args if the caller sets it, and _guest_debug / _guest_forbid_trace
# (a WIN10XBOX_DEBUG spec and a regex the layer's trace must not match) if it wants to assert on the trace.
function(win10xbox_guest_test name program exit_code)
  # On the builtin-only folder, also prove from the modules trace that no native system DLL was mapped.
  set(_require_builtin OFF)
  if(NOT _guest_machine)
    set(_guest_machine ${WIN10XBOX_MACHINE_DIR})
  elseif(_guest_machine STREQUAL WIN10XBOX_BUILTIN_MACHINE_DIR)
    set(_require_builtin ON)
  endif()
  # A console program (the caller sets _guest_console) never loads user32.dll: the builtin check then only
  # requires that no native system DLL was mapped.
  set(_require_user32 ${_require_builtin})
  if(_guest_console)
    set(_require_user32 OFF)
  endif()
  add_test(NAME integration.${name}
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox>
      -DMACHINE=${_guest_machine}
      -DREQUIRE_BUILTIN_DLLS=${_require_builtin}
      "-DNO_USER32=${_guest_no_user32}"
      -DREQUIRE_USER32=${_require_user32}
      -DPROGRAM=${program}
      "-DPROGRAM_ARGS=${_guest_args}"
      "-DDEBUG_SPEC=${_guest_debug}"
      "-DFORBID_TRACE_REGEX=${_guest_forbid_trace}"
      "-DEXPECT_TRACE_REGEX=${_guest_expect_trace}"
      "-DSAVE_STDOUT=${_guest_save_stdout}"
      "-DKEEP_REGISTRY_OVERLAY=${_guest_keep_overlay}"
      -DEXPECT_EXIT=${exit_code}
      "-DEXPECT_REGEX=${ARGN}"
      "-DINPUT_FILE=${_guest_input}" "-DGUEST_FILE=${_guest_file}" "-DRUN_TIMEOUT=${_guest_run_timeout}"
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

# The machine identity (D18) through every path a program can read it from (tests/guest/identity.c): CPUID leaves 0, 1,
# 0x80000001..4 and 0x40000000, RDRAND, the registry's CentralProcessor\\0, the SMBIOS table of GetSystemFirmwareTable,
# WMI Win32_Processor through COM, GetNativeSystemInfo, and the version through RtlGetVersion, GetVersionExW,
# VerifyVersionInfoW, RtlGetNtVersionNumbers, the registry and KUSER_SHARED_DATA. Exit 42 only when they all agree on
# AMD 4700S 8-Core Processor, family 23 model 136 stepping 1 and 10.0.19045.3803; every value is printed as path=value.
# On the builtin-only folder (the console's DLL set): WMI is Wine's wbemprox over our SMBIOS table, and the runner's
# native ole32/advapi32 cannot load under Wine's ntdll (see the GUI tests). WMI works under Wine, so the guest's
# WIN10XBOX_TEST_NO_WMI=1 escape hatch is not used by any registration.
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_save_stdout ${CMAKE_BINARY_DIR}/bin/guest_identity.log)   # the value table, uploaded by CI
win10xbox_guest_test(guest_identity "C:\\guest\\guest_identity.exe" 42
  "cpuid\\.brand=AMD 4700S 8-Core Processor" "registry\\.brand=AMD 4700S 8-Core Processor"
  "smbios\\.brand=AMD 4700S 8-Core Processor" "wmi\\.name=AMD 4700S 8-Core Processor"
  "version\\.KUSER_SHARED_DATA=10\\.0\\.19045" "identity=consistent")
# WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1: the runners' CPUs have RDRAND, the console's does not; the rdrand sites become ud2 so the
# emulation (the console's path) is what answers.
set_tests_properties(integration.guest_identity PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL "
  ENVIRONMENT "WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1")
unset(_guest_save_stdout)
unset(_guest_machine)

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

# Nothing of Wine is observable from the builtins (docs/slices/nowine.md, wine-pe/patches 0005-0007): tests/guest/nowine.c
# walks the modules the process has loaded and checks every export name, version resource and read-only data section,
# and that ntdll does not resolve the Wine version function. Builtin-only folder: every system DLL is one of ours.
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_console ON)
win10xbox_guest_test(guest_rdrand "C:\\guest\\guest_rdrand.exe" 42 "rdrand: ok n=16")
set_tests_properties(integration.guest_rdrand PROPERTIES FAIL_REGULAR_EXPRESSION "rdrand: FAIL"
  ENVIRONMENT "WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1")

win10xbox_guest_test(guest_nowine "C:\\guest\\guest_nowine.exe" 42
  "nowine: ntdll.dll exports=" "nowine: kernelbase.dll exports=" "nowine: ntdll.dll does not resolve" "nowine: done modules=[0-9]+ hits=0")
set_tests_properties(integration.guest_nowine PROPERTIES FAIL_REGULAR_EXPRESSION "nowine: HIT;nowine: FAIL")
unset(_guest_console)
unset(_guest_machine)

# CreateProcess (docs/DISPATCHER.md "Child processes"): NtCreateUserProcess starts a new host process running the
# host executable on the child's guest image. The parent (tests/guest/spawn.c) runs guest_hello.exe and waits for
# its exit code 42 through the host handle, then runs itself with a quoted command line, a working directory and
# an extra environment variable and checks the exit code 7; the children's stdout is the inherited pipe.
win10xbox_guest_test(guest_spawn "C:\\guest\\guest_spawn.exe" 42
  "hello from guest" "spawn: child exit 42"
  "spawn-child: argc=5" "spawn-child: argv\\[1\\]=\\[child\\]" "spawn-child: argv\\[2\\]=\\[a b\\]"
  "spawn-child: argv\\[3\\]=\\[c.d\\]" "spawn-child: argv\\[4\\]=\\[e\\]"
  "spawn-child: cwd=C:\\\\Windows" "spawn-child: mark=mark42" "spawn: child2 exit 7" "spawn: done")

# Named objects across CreateProcess (docs/DISPATCHER.md "Object manager namespace"): the parent creates a named event,
# a named section holding a string and a global event; lists both BaseNamedObjects directories; starts itself as the
# child, which opens all three by name, reads the string and sets the events. The names map onto host objects under
# the layer's prefix, so the child (another host process) finds them.
win10xbox_guest_test(guest_named_objects "C:\\guest\\guest_named_objects.exe" 42
  "named: created event, section and global event"
  "named: .*BaseNamedObjects has w10x_no_event \\[Event\\]" "named: .*BaseNamedObjects has w10x_no_section \\[Section\\]"
  "named: .*BaseNamedObjects has w10x_no_global \\[Event\\]"
  "named-child: data=\\[named-objects-section-data-42\\]" "named: event set by the child" "named: child exit 7" "named: done")

# Exception handling (tests/guest/seh.cpp): C++ throw/catch across frames and a DLL boundary, SEH __try/__except
# around an access violation / divide by zero / RaiseException / a fault in a DLL frame, vectored handlers, the
# unhandled-exception filter. Exceptions are delivered by the host kernel to the host ntdll and unwound with the
# guest's .pdata (docs/DECISIONS.md E2). A failed section prints "seh: <name> FAILED".
win10xbox_guest_test(guest_seh "C:\\guest\\guest_seh.exe" 42
  "seh: throw_catch ok" "seh: dll_boundary ok" "seh: seh ok" "seh: veh ok" "seh: uef ok" "seh: done failures=0")
set_tests_properties(integration.guest_seh PROPERTIES FAIL_REGULAR_EXPRESSION "seh: [a-z_.]+ FAILED;seh: FAIL line")
# Wine __TRY frames (kernelbase IsBadStringPtr*) caught on the main thread and on worker threads: the host's exception
# dispatcher does not walk TEB.NtTib.ExceptionList, core/dispatch/teb_frames.cpp does (tests/guest/tebframe.c).
# On the builtin-only machine folder: the shared one has the host's native kernel32, whose IsBadStringPtr is table based.
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_console ON)
set(_guest_debug "+dispatch")
set(_guest_expect_trace "TEB-frame dispatch: [0-9]+ exceptions with a live frame, [0-9]+ continued, [0-9]+ stepped aside for a table-based handler, [1-9][0-9]* unwinds to a frame")
win10xbox_guest_test(guest_tebframe "C:\\guest\\guest_tebframe.exe" 42
  "tebframe: main ok" "tebframe: workers ok" "tebframe: done failures=0")
unset(_guest_expect_trace)
unset(_guest_debug)
unset(_guest_console)
unset(_guest_machine)
set_tests_properties(integration.guest_tebframe PROPERTIES FAIL_REGULAR_EXPRESSION "tebframe: FAIL line")

# Wine's cmd.exe (programs/cmd in wine-pe/dlls.txt) is the machine's C:\Windows\System32\cmd.exe: ComSpec points at it,
# ntdll's file.c opens it, kernelbase starts it for .bat files. The VFS serves it from Windows\builtin, on the
# builtin-only folder like the GUI tests; the program path is the guest's (host-win32 must not run the host's own cmd.exe).
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_args "/c;echo cmd runs through the layer")
win10xbox_guest_test(cmd_builtin "C:\\Windows\\System32\\cmd.exe" 0 "cmd runs through the layer")
unset(_guest_args)
unset(_guest_machine)

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
# The non-client area (tests/guest/window_nc.c): frame metrics, hit testing, a caption drag (+50,+40), a bottom-right
# corner drag (+30,+20) and a click on the close button, injected by the host; the guest reads the frame's pixels back
# with GetPixel and exits 42 once the window was moved and resized and WM_CLOSE/WM_DESTROY arrived. The screenshot (the
# last frame painted: window at 150,140, 430x320) is checked too: active caption (153,180,209) at (420,152), border
# (180,180,180) at (153,300), the close button (199,80,80) at (538,150), the client (64,64,64) at (300,300).
win10xbox_guest_test(guest_window_nc "C:\\guest\\guest_window_nc.exe" 42
  "window_nc: frame ok" "window_nc: hit caption=2 close=20 max=9 min=8 client=1 top=12 topleft=13 left=10 bottomright=17"
  "window_nc: dragged to 150,140 size=400x300" "window_nc: resized to 150,140 size=430x320" "window_nc: pixels ok"
  "window_nc: WM_SYSCOMMAND SC_CLOSE" "window_nc: WM_CLOSE" "window_nc: WM_DESTROY" "window_nc: done")
set_tests_properties(integration.guest_window_nc PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window_nc.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=420,152,ff99b4d1|153,300,ffb4b4b4|538,150,ffc75050|300,300,ff404040;WIN10XBOX_TEST_INJECT_INPUT=sleep 4000\\;move 300,115\\;sleep 300\\;ldown\\;sleep 300\\;move 320,130\\;sleep 300\\;move 350,155\\;sleep 300\\;lup\\;sleep 1000\\;move 548,438\\;sleep 300\\;ldown\\;sleep 300\\;move 560,450\\;sleep 300\\;move 578,458\\;sleep 300\\;lup\\;sleep 1000\\;move 555,155\\;sleep 300\\;ldown\\;sleep 300\\;lup")
# Input semantics (tests/guest/window_input2.c): the host script covers the driver path (move, double click, right
# click, wheel, key; the guest compares every message with its hit test / WM_SETCURSOR companions), the guest the rest with
# SendInput. It prints PASS/FAIL per case and exits 42 only when every case passed.
win10xbox_guest_test(guest_window_input2 "C:\\guest\\guest_window_input2.exe" 42
  "PASS host-injected move, double click" "PASS caps lock on" "PASS click on an inactive window" "PASS capture: a click outside"
  "PASS child: click" "PASS US layout: VkKeyScan" "window_input2: [0-9]+ passed, 0 failed")
set_tests_properties(integration.guest_window_input2 PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_INJECT_INPUT=sleep 3000\\;move 150,150\\;ldown\\;lup\\;ldown\\;lup\\;rdown\\;rup\\;wheel -120\\;key 0x42"
  FAIL_REGULAR_EXPRESSION "FAIL ")
# Hooks and timers (tests/guest/window_hooks.c): WH_MOUSE x2, WH_KEYBOARD and WH_GETMESSAGE hooks on the guest's own
# thread see the host-injected input before the window procedure does, a hook returning 1 eats a message, an unhooked
# hook stops seeing keys; SetTimer fires every `elapse` ms (50 ms x20 within 950..1400 ms, never closer than 40 ms),
# clamps to 10 ms, KillTimer drops a pending WM_TIMER, a TIMERPROC is called by DispatchMessage.
win10xbox_guest_test(guest_window_hooks "C:\\guest\\guest_window_hooks.exe" 42
  "hooks: PASS injected input arrived" "hooks: PASS WH_MOUSE sees LBUTTONDOWN before the window procedure"
  "hooks: PASS WH_MOUSE chain runs newest first and CallNextHookEx reaches the older hook"
  "hooks: PASS WH_KEYBOARD sees vk 41 down and up" "hooks: PASS hook returning 1 eats WM_RBUTTONDOWN"
  "hooks: PASS UnhookWindowsHookEx stops WH_KEYBOARD" "hooks: PASS WH_GETMESSAGE sees every message with PM_REMOVE"
  "hooks: PASS SetTimer 50 ms cadence" "hooks: PASS SetTimer minimum is 10 ms"
  "hooks: PASS KillTimer removes the pending WM_TIMER" "hooks: PASS TIMERPROC is called through DispatchMessage"
  "hooks: PASS message time is the tick count" "hooks: [0-9]+ passed, 0 failed" "hooks: done")
set_tests_properties(integration.guest_window_hooks PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_INJECT_INPUT=sleep 2000\\;move 150,150\\;ldown\\;lup\\;key 0x41\\;rdown\\;rup\\;key 0x42"
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
# Menus (tests/guest/window_menu.c, core/user/win_menu.cpp): CreateMenu / CreatePopupMenu / SetMenu, the menu bar the
# window reserves under its caption (client height shrinks by SM_CYMENU = 20), the bar pixels, and the host clicking
# "File" then "Exit" (WM_COMMAND id=102) and then an item of TrackPopupMenu(TPM_RETURNCMD). The window is at (100,100)
# 400x300 with the Windows 10 frame (8 px) and caption (23 px): the bar is (108,131)-(492,151), the item "File"
# (108,131)-(156,150), the popup below it at (108,151): Open (109,154)-(219,176), Exit (109,185)-(219,207). The
# TrackPopupMenu popup opens at (300,250): its first item is at y 253..275. The screenshot shows the bar background
# (COLOR_MENU f0f0f0) and the top pixel row of the "F" glyph (COLOR_MENUTEXT) at (116,134).
win10xbox_guest_test(guest_window_menu "C:\\guest\\guest_window_menu.exe" 42
  "menu: api items=4 id0=101 checked=1 grayed=1"
  "menu: client height before=[0-9]+ after=[0-9]+ delta=20 SM_CYMENU=20"
  "menu: bar rect=108,131,492,151" "menu: item File rect=108,131,156,150"
  "menu: pixels bg=00f0f0f0 glyph=00000000"
  "menu: WM_INITMENUPOPUP popup=1"
  "menu: popup items open=109,154,219,176 exit=109,185,219,207"
  "menu: WM_COMMAND id=102" "menu: TrackPopupMenu returned 101"
  "menu: loops enter=2 exit=2 init=2" "menu: done")
set_tests_properties(integration.guest_window_menu PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window_menu.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=300,140,fff0f0f0|116,134,ff000000;WIN10XBOX_TEST_INJECT_INPUT=waitwin 60000\\;sleep 2000\\;move 132,140\\;ldown\\;lup\\;sleep 1500\\;move 128,196\\;ldown\\;lup\\;sleep 2500\\;move 320,264\\;ldown\\;lup"
  FAIL_REGULAR_EXPRESSION "FAIL ")
# Cursors and icons (tests/guest/cursor.c, core/user/win_cursor.cpp). The guest loads Wine's IDC_ARROW / IDC_IBEAM (user32
# reads the 32 bpp images from its resources and hands the bitmaps to NtUserSetCursorIconData), builds a monochrome
# AND/XOR cursor, checks GetIconInfo / GetCursorInfo / SetCursor / ShowCursor and draws the icons with DrawIconEx
# (pixels read back with GetPixel). The composed cursor is not painted by the guest, so the host script moves the
# pointer to four places (the guest changes the cursor on each move) and writes a screenshot after the first three with
# `shot FILE` (host-win32/main.cpp); run_guest.cmake checks pixels of each file (WIN10XBOX_SCREENSHOT_2.._4):
#  _2 pointer (250,250), I-beam, hotspot (3,9): image (1,1) and (3,1) opaque white at (248,242) and (250,242), image
#     (3,9) fd fd fd at (250,250), image (10,9) transparent at (257,250) = the window colour ff4080c0;
#  _3 pointer (320,250), arrow, hotspot (0,0): image (1,1) opaque black at (321,251), (2,4) and (3,5) opaque white at
#     (322,254) and (323,255), (20,5) transparent at (340,255); the old I-beam spot (250,250) is the window colour again;
#  _4 pointer (390,250), the monochrome cursor, hotspot (4,4), columns of 8: black at (388,252), white at (396,252),
#     inverted window colour (ffbf7f3f) at (404,252), unchanged at (412,252);
#  the final file (WIN10XBOX_SCREENSHOT, written when the guest has exited): the pointer at (392,252) after
#  ShowCursor(FALSE), so none of those pixels of the monochrome cursor is drawn (390,254), (398,254), (406,254), plus
#  the DrawIconEx results in the window: arrow (111,111) black and (112,114) white, stretched arrow (162,112) black,
#  monochrome (222,116) black, (230,116) white, (238,116) ffbf7f3f, (246,116) unchanged.
win10xbox_guest_test(guest_cursor "C:\\guest\\guest_cursor.exe" 42
  "cursor: loaded arrow=[0-9a-fA-Fx]+ ibeam=[0-9a-fA-Fx]+ shared=1"
  "cursor: arrow icon=0 hot=0,0 mask=32x32x1 color=32x32x32" "cursor: ibeam icon=0 hot=3,9 mask=32x32x1 color=32x32x32"
  "cursor: mono icon=0 hot=4,4 mask=32x64x1 color=none"
  "cursor: DrawIconEx arrow=ok stretched=ok mono=ok" "cursor: P0 ibeam showing pos=250,250"
  "cursor: P1 SetCursor\\(arrow\\) returned ibeam" "cursor: P2 SetCursor\\(mono\\) returned arrow"
  "cursor: P3 ShowCursor\\(FALSE\\)=-1 flags=0" "cursor: done")
set_tests_properties(integration.guest_cursor PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_cursor.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=390,254,ff4080c0|398,254,ff4080c0|406,254,ff4080c0|111,111,ff000000|112,114,ffffffff|162,112,ff000000|222,116,ff000000|230,116,ffffffff|238,116,ffbf7f3f|246,116,ff4080c0;WIN10XBOX_SCREENSHOT_2=${CMAKE_BINARY_DIR}/bin/guest_cursor_ibeam.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS_2=248,242,ffffffff|250,242,ffffffff|250,250,fffdfdfd|257,250,ff4080c0|270,250,ff4080c0;WIN10XBOX_SCREENSHOT_3=${CMAKE_BINARY_DIR}/bin/guest_cursor_arrow.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS_3=321,251,ff000000|322,254,ffffffff|323,255,ffffffff|340,255,ff4080c0|250,250,ff4080c0;WIN10XBOX_SCREENSHOT_4=${CMAKE_BINARY_DIR}/bin/guest_cursor_mono.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS_4=388,252,ff000000|396,252,ffffffff|404,252,ffbf7f3f|412,252,ff4080c0|321,251,ff4080c0;WIN10XBOX_TEST_INJECT_INPUT=waitwin 60000\\;sleep 1500\\;move 250,250\\;sleep 1500\\;shot ${CMAKE_BINARY_DIR}/bin/guest_cursor_ibeam.bmp\\;move 320,250\\;sleep 1500\\;shot ${CMAKE_BINARY_DIR}/bin/guest_cursor_arrow.bmp\\;move 390,250\\;sleep 1500\\;shot ${CMAKE_BINARY_DIR}/bin/guest_cursor_mono.bmp\\;move 392,252\\;sleep 1500"
  FAIL_REGULAR_EXPRESSION "cursor: FAIL")
# XInput (tests/guest/xinput.c): the guest imports the layer's own xinput1_4.dll (guest-dlls/xinput, a builtin next to
# Wine's) and reads the gamepad the HOST holds, through the "xinput1_4" unix library. The host script connects a neutral
# pad, waits (padwait) until the guest has read it, then presses A with stick and trigger values: the guest sees its packet
# number move and the injected values, users 1..3 stay ERROR_DEVICE_NOT_CONNECTED (1167).
win10xbox_guest_test(guest_xinput "C:\\guest\\guest_xinput.exe" 42
  "xinput: screen=[0-9]+x[0-9]+" "xinput: caps ok type=1 subtype=1" "xinput: idle packet=[0-9]+ buttons=0x0000"
  "xinput: packet changed [0-9]+ -> [0-9]+" "xinput: buttons=0x1000 lx=1000 ly=-2000 rx=3000 ry=-4000 lt=128 rt=255"
  "xinput: users 1\\.\\.3 not connected \\(1167 1167 1167\\)" "xinput: set state ok, battery type=1 level=3, bad index=160"
  "xinput: done")
set_tests_properties(integration.guest_xinput PROPERTIES
  ENVIRONMENT "WIN10XBOX_TEST_INJECT_INPUT=pad 0 0 0 0 0 0 0\\;padwait 30000\\;sleep 300\\;pad 1000 1000 -2000 3000 -4000 128 255"
  FAIL_REGULAR_EXPRESSION "xinput: FAIL")
# Scroll bars (tests/guest/window_scroll.c, core/user/win_scroll.cpp): range/position round trip, HTVSCROLL hit test,
# ScrollWindowEx / ScrollDC pixels, and a host-injected click on the vertical bar's down arrow that must arrive as
# WM_VSCROLL SB_LINEDOWN + SB_ENDSCROLL. The screenshot shows the bar's track colour (COLOR_SCROLLBAR, ffc8c8c8) at
# screen (391,250), between the thumb and the down arrow.
win10xbox_guest_test(guest_window_scroll "C:\\guest\\guest_window_scroll.exe" 42
  "scroll: client=283x183" "scroll: v min=0 max=100 page=10 pos=20" "scroll: h min=0 max=50 pos=5"
  "scroll: vbar rect=383,100,400,283" "scroll: hittest down-arrow=7 hbar=6 client=1"
  "scroll: ScrollWindowEx before=404040 after=0000ff upd=0,0,283,30" "scroll: ScrollDC pixel=0000ff upd=0,0,283,10"
  "scroll: WM_VSCROLL code=1" "scroll: after click linedown=1 endscroll=1 pos=21" "scroll: done")
set_tests_properties(integration.guest_window_scroll PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window_scroll.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=391,250,ffc8c8c8;WIN10XBOX_TEST_INJECT_INPUT=sleep 2000\\;move 391,275\\;ldown\\;sleep 100\\;lup")
# The clipboard (tests/guest/clipboard.c, core/user/win_clipboard.cpp): CF_UNICODETEXT set/read, CF_TEXT synthesis,
# a registered format, delayed rendering, the sequence number. It creates a (hidden) window, so builtin-only folder.
win10xbox_guest_test(guest_clipboard "C:\\guest\\guest_clipboard.exe" 42
  "clipboard: window ok" "clipboard: set-unicode ok" "clipboard: sequence-number ok" "clipboard: get-unicode ok"
  "clipboard: synthesized-text ok CF_TEXT=Hello Xbox clipboard" "clipboard: enum-formats ok count=4"
  "clipboard: registered-format ok" "clipboard: delayed-render ok calls=1" "clipboard: listener-update ok count=1"
  "clipboard: listener-removed ok" "clipboard: done")
set_tests_properties(integration.guest_clipboard PROPERTIES FAIL_REGULAR_EXPRESSION "clipboard: FAIL")
# The dialog manager (tests/guest/dialog.c): DialogBoxParamW with a DIALOG resource, WM_INITDIALOG, GetDlgItem,
# Set/GetDlgItemText, tab order, WM_CTLCOLORSTATIC, a click on the OK button injected by the host (screen (160,163) =
# the button's centre once the program placed the dialog at (100,100) and the button at client (10,50) 100x26),
# EndDialog(7). The screenshot is the last painted frame: the button's frame (110,150) and face (118,158), the static's
# light yellow (160,114) and the edit's white (240,136), all in screen coordinates.
win10xbox_guest_test(guest_dialog "C:\\guest\\guest_dialog.exe" 42
  "dialog: init edit=100 button=1" "dialog: text=hello dialog" "dialog: class=#32770 edit=Edit button=Button static=Static"
  "dialog: ok clicked" "dialog: result=7" "dialog: done")
set_tests_properties(integration.guest_dialog PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_dialog.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=110,150,ff646464|118,158,fff0f0f0|160,114,ffffffc0|240,136,ffffffff;WIN10XBOX_TEST_INJECT_INPUT=sleep 2500\\;move 160,163\\;ldown\\;lup")
# comctl32 (Wine's builtin, v5 classes: no manifest) through the layer: a report-view ListView (2 columns, 3 items) with its
# header, a TreeView and a smooth progress bar (bar colour RGB(0,160,0), background RGB(230,230,230), position 50 of
# 100) as children of a 400x300 popup at (100,100). The guest reads the pixels back with GetPixel (tests/guest/comctl.c);
# the screenshot checks the same ones at screen coordinates: ListView background (ListView at 5,5 -> client (150,100) =
# screen (255,205)) white, the header's button face right of its two columns (client (180,9) = (285,114)) f0f0f0, the
# progress bar (at 5,135) inside the fill (client (20,10) = (125,245)) 00a000 and outside it (client (170,10) =
# (275,245)) e6e6e6.
win10xbox_guest_test(guest_comctl "C:\\guest\\guest_comctl.exe" 42
  "comctl: init ok" "comctl: created lv=1 tv=1 pb=1 hdr=1"
  "comctl: lv count=3 cols=2 tv count=2 pb pos=50"
  "comctl: pixels lv=00ffffff hdr=00f0f0f0 pb_in=0000a000 pb_out=00e6e6e6"
  "comctl: label pixels drawn=[1-9][0-9]*" "comctl: done")
set_tests_properties(integration.guest_comctl PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_comctl.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=255,205,ffffffff|285,114,fff0f0f0|125,245,ff00a000|275,245,ffe6e6e6")
# GDI bitmaps (tests/guest/gdi_bitmap.c, docs/slices/gdibmp.md): the guest checks every pixel itself and exits 42
# only if all checks passed; the screenshot is the window part (400x300 at screen (100,100), client origin = screen
# origin + (100,100)): a 32 bpp DIB section blitted at (10,10) (pixel (3,2) = RGB(96,64,128)), a 24 bpp one at
# (30,10) (pixel (4,2) = RGB(120,130,162), top-left (0,0) = RGB(200,30,0)), an 8 bpp palettized one at (50,10),
# NOTSRCCOPY at (70,10), SRCINVERT at (90,10) and (110,10), a 2x StretchBlt at (10,40), AlphaBlend (130,10), TransparentBlt
# (150,10), LoadImage of a 24 bpp resource at (10,70) and an 8 bpp one at (40,70).
win10xbox_guest_test(guest_gdi_bitmap "C:\\guest\\guest_gdi_bitmap.exe" 42
  "bmp: setup ok" "bmp: srccopy ok" "bmp: alpha-128 ok" "bmp: transparent-px ok" "bmp: getdibits-topleft ok"
  "bmp: stretchdibits-2 ok" "bmp: dib1-top0 ok" "bmp: dib24-gdi-bits-seen ok" "bmp: dib8-setcolortable ok"
  "bmp: window created" "bmp: win-dib32-srccopy ok" "bmp: win-srcinvert ok" "bmp: win-stretch2x-all ok"
  "bmp: win-readback-bits ok" "bmp: win-alphablend-128 ok" "bmp: win-transparent-px ok"
  "bmp: loadimage24-getobject ok" "bmp: loadimage24-pixels ok" "bmp: loadimage8-pixels ok" "bmp: window drawn"
  "bmp: failures=0" "bmp: done")
set_tests_properties(integration.guest_gdi_bitmap PROPERTIES
  ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_gdi_bitmap.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=113,112,ff604080|134,112,ff7882a2|130,110,ffc81e00|151,110,ff00ff00|153,111,ffff0000|173,112,ff9fbf7f|193,112,ff9f4f80|213,112,ffff0f00|117,145,ff604080|118,144,ff804080|234,112,ff96644b|250,110,ff070809|252,112,ff010203|110,170,ff0a14fa|115,173,ffd2c861|141,170,ff00ff00|143,173,ff0000ff"
  FAIL_REGULAR_EXPRESSION "FAIL ")
unset(_guest_machine)

# ---- Networking: Wine's ws2_32 over core/afd (host Winsock) -----------------------------------------------------
# tests/guest/net.c runs an echo server thread and a client over TCP loopback. Builtin-only machine folder: a native
# ws2_32 would talk to the real AFD and bypass the code under test.
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_no_user32 ON)
win10xbox_guest_test(guest_net "C:\\guest\\guest_net.exe" 42
  "net: WSAStartup ok" "net: gethostname ok" "net: getaddrinfo localhost -> 127\\.0\\.0\\.1" "net: listening"
  "net: connected" "net: FIONBIO ok" "net: sent 35 bytes" "net: received 35 bytes: hello from the guest over loopback"
  "net: done")
set_tests_properties(integration.guest_net PROPERTIES FAIL_REGULAR_EXPRESSION "net: FAILED")
unset(_guest_no_user32)
unset(_guest_machine)

# ---- Registry persistence (docs/DISPATCHER.md "Registry persistence") -------------------------------------------------
# tests/guest/regpersist.c twice, each a new host process on the same machine folder: the first run writes a value of each
# common type under HKCU\Software\win10xbox\persist, RegFlushKey, writes one more, and starts itself as a child that
# reads the flushed and the unflushed value; the second run (argument "read", after the first exited and so after the
# exit-time save) reads every value back. run_guest.cmake deletes the overlay file before every other guest run, and
# keeps it for this one. Console DLL set.
set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
set(_guest_no_user32 ON)
win10xbox_guest_test(guest_regpersist_write "C:\\guest\\guest_regpersist.exe" 42
  "regpersist: write ok" "regpersist: child ok" "regpersist: done")
set(_guest_args "read")
set(_guest_keep_overlay ON)   # the registry the write run left
win10xbox_guest_test(guest_regpersist_read "C:\\guest\\guest_regpersist.exe" 42
  "regpersist: sz ok" "regpersist: expand ok" "regpersist: multisz ok" "regpersist: dword ok" "regpersist: qword ok"
  "regpersist: binary ok" "regpersist: default ok" "regpersist: prespawn ok" "regpersist: subkey ok" "regpersist: done")
unset(_guest_args)
unset(_guest_keep_overlay)
set_tests_properties(integration.guest_regpersist_write integration.guest_regpersist_read PROPERTIES
  FAIL_REGULAR_EXPRESSION "regpersist: [a-z ]+ FAILED")
set_tests_properties(integration.guest_regpersist_read PROPERTIES DEPENDS integration.guest_regpersist_write)
unset(_guest_no_user32)
unset(_guest_machine)
# ---- Audio: Wine's mmdevapi over core/dispatch/unix_mmdevapi.cpp (the host's WASAPI) -----------------------------------
# tests/guest/wasapi.c and xaudio2.c exit 42 when a device played the sound and 43 when the host has no audio endpoint.
# audio_test.cmake runs each one directly on the host (what the host really answers) and through the layer on the
# builtin-only folder (Wine's mmdevapi, xaudio2_9 and the registry's MMDeviceEnumerator registration) and requires the
# same verdict, in the no-endpoint case with the same HRESULT; nothing is faked either way (docs/slices/audio.md).
foreach(_a wasapi xaudio2)
  add_test(NAME integration.guest_${_a}
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox>
      -DMACHINE=${WIN10XBOX_BUILTIN_MACHINE_DIR}
      -DGUEST_EXE=$<TARGET_FILE:guest_${_a}>
      -DPROBE_EXE=$<TARGET_FILE:guest_wasapi>
      -DPROGRAM=C:\\guest\\guest_${_a}.exe
      -DTAG=${_a}
      -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/audio_test.cmake)
  set_tests_properties(integration.guest_${_a} PROPERTIES
    FIXTURES_REQUIRED w10x_machine
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "SKIP:"
    LABELS "guest_e2e"
    TIMEOUT 240)
endforeach()

# ---- Real programs: the first guests that are not ours ----------------------------------------------------------
# Lua 5.4.7, built from the upstream tarball with MSVC by tools/ci/build_lua.ps1 (a named step of ci.yml's windows
# job, which leaves it at <machine>/guest/lua.exe before ctest runs). Registered only where that step exists: MSVC
# builds, or any build configured with -DWIN10XBOX_REAL_PROGRAMS=ON after putting a lua.exe into the machine folder
# (docs/TESTING.md, "Real programs"). Not registered on linux-cross, so its skip floor never counts them.
option(WIN10XBOX_REAL_PROGRAMS "Register the real-program guest tests (lua.exe must be in <machine>/guest)" OFF)
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  configure_file(${CMAKE_SOURCE_DIR}/tests/guest/lua/smoke.lua ${CMAKE_BINARY_DIR}/bin/guest/lua_smoke.lua COPYONLY)
  # (a) the command line: -e with a quoted argument, and the exit code from os.exit.
  set(_guest_args "-e;print('hello from lua', 6*7) os.exit(42)")
  win10xbox_guest_test(lua_hello "C:\\guest\\lua.exe" 42 "hello from lua\t42")
  # (b) the script (tests/guest/lua/smoke.lua): every section prints "smoke: <name> ok".
  set(_guest_args "C:\\guest\\lua_smoke.lua;alpha;beta gamma")
  set(_lua_sections argv format format.g sort sort.words io.write os.time os.date os.date.t os.clock getenv getenv.none
    io.open.write file.close io.open.read file.line1 file.line2 file.rest file.size os.rename os.remove file.gone
    io.open.missing os.tmpname tmpname.open io.lines dofile require pcall pcall.table pcall.runtime xpcall string.rep string.rep.sub string.rep.sep table.concat gc)
  set(_lua_expect "")
  foreach(_s ${_lua_sections})
    string(REPLACE "." "\\." _re "${_s}")
    list(APPEND _lua_expect "smoke: ${_re} ok")
  endforeach()
  win10xbox_guest_test(lua_smoke "C:\\guest\\lua.exe" 42 ${_lua_expect} "smoke: done fails=0")
  set_tests_properties(integration.lua_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "smoke: [a-z.]+ FAILED")
  # (c) the host-side unwind tables: every image the guest maps must be registered with the host's
  # RtlAddFunctionTable (the host walks guest frames for pcall/longjmp and for SEH). Before the fix the lookup of
  # that export failed for every image mapped after the PEB take-over (docs/STATUS.md, "Real programs").
  set(_guest_args "-e;print(pcall(error, 'x')) os.exit(42)")
  set(_guest_debug "-all,+loader")
  set(_guest_forbid_trace "RtlAddFunctionTable failed")
  win10xbox_guest_test(lua_unwind_tables "C:\\guest\\lua.exe" 42 "false\tx")
  unset(_guest_args)
  unset(_guest_debug)
  unset(_guest_forbid_trace)

  # Sysinternals Coreinfo64.exe (tools/ci/fetch_sysinternals.ps1, pinned by SHA-256), a system-information program:
  # it reads the CPU topology through NtQuerySystemInformation. PsInfo64 (also pinned) reports the system's name, uptime
  # and kernel version; it needs Wine's ws2_32 (core/afd, docs/slices/net.md).
  # The anchors are lines every runner prints whatever its CPU vendor is (docs/slices/sysinfo.md).
  # It imports comdlg32/shell32, whose native copies cannot load on the shared
  # machine folder (docs/slices/sysinfo.md), so they run on the builtin-only one, like the GUI guests below.
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_args "-accepteula")
  win10xbox_guest_test(coreinfo "C:\\guest\\Coreinfo64.exe" 0
    "Logical to Physical Processor Map:"
    "Logical Processor to Socket Map:")
  win10xbox_guest_test(psinfo "C:\\guest\\PsInfo64.exe" 0
    "System information for "
    "Uptime:"
    "Kernel version:")
  unset(_guest_args)
  unset(_guest_machine)
endif()

# ---- fastfetch: the first prebuilt real program ----------------------------------------------------------------------
# fastfetch 2.70.0, the project's official Windows x64 release (tools/ci/fetch_fastfetch.ps1: pinned SHA-256, verified on
# every run), left at <machine>/guest/fastfetch.exe by a named step of ci.yml's windows job before ctest runs. It is a
# system-information tool: it asks the layer for the OS version, kernel, CPU, memory, locale and uptime. Gate: exit 0
# and the four headline modules on stdout; the other modules (Disk, GPU, Display, ...) are run one by one, for information,
# by a step of ci.yml (docs/slices/fastfetch.md).
# It runs on the builtin-only folder (the console's DLL set): the shared folder's native DLLs (the runner's build 26100)
# do not work under Wine's ntdll (see the GUI tests above), and fastfetch imports shell32, ole32 and advapi32.
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_args "--pipe;--logo;none;-s;OS:Kernel:CPU:Memory:Locale:Uptime")
  win10xbox_guest_test(fastfetch "C:\\guest\\fastfetch.exe" 0 "OS: " "Kernel: " "CPU: " "Memory: ")
  unset(_guest_args)
  unset(_guest_machine)
endif()

# ---- SDL2: a real third-party DLL and a GUI program on top of it ------------------------------------------------------
# sdl_rects.exe (tests/guest/sdl2/sdl_rects.c) is linked against the official SDL2 2.30.9 x64 release; SDL2.dll sits next to
# it in <machine>/guest (tools/ci/build_sdl_guest.ps1, a named ci.yml step of the windows job). A borderless 320x240 window
# at (100,100), software surface: blue clear + red rect (20,20,60,60) + green rect (120,40,50,50), pixels read back through
# GetPixel, exit 42. The screenshot is checked at screen = client + (100,100). Builtin-only folder, like the GUI tests above.
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  win10xbox_guest_test(real_sdl2 "C:\\guest\\sdl_rects.exe" 42
    "sdl: init video=windows" "sdl: surface 320x240" "sdl: pixels 00dc5000 000000ff 0000ff00" "sdl: done")
  set_tests_properties(integration.real_sdl2 PROPERTIES
    ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/real_sdl2.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=105,105,ff0050dc|130,130,ffff0000|230,150,ff00ff00")
  unset(_guest_machine)
endif()

# ---- Tiny C Compiler 0.9.27: a compiler that runs through the layer, and the program it writes ---------------------
# tools/ci/fetch_tcc.ps1 (the official prebuilt win64 zip, pinned SHA-256) leaves tcc.exe, libtcc.dll, include/ and lib/
# in <machine>/guest/tcc before ctest runs (docs/slices/realtcc.md). tcc_compile writes C:\Users\User\tcc_hello.exe
# (GUEST_FILE: deleted before, required after) and tcc_hello_runs runs that exe, so it DEPENDS on tcc_compile;
# tcc_run is tcc's in-memory mode (-run: the program is relocated into VirtualAlloc'd memory and called).
# It runs on the builtin-only folder like fastfetch: tcc.exe loads libtcc.dll, user32 and advapi32, and on the
# shared folder those pull the runner's native sechost/ucrtbase under Wine's user32 and the first call dies on
# EXCEPTION_WINE_STUB (0x80000100); measured in the first run of this slice.
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  configure_file(${CMAKE_SOURCE_DIR}/tests/guest/tcc/hello.c ${CMAKE_BINARY_DIR}/bin/guest/tcc_hello.c COPYONLY)
  set(_guest_args "-v")
  win10xbox_guest_test(tcc_version "C:\\guest\\tcc\\tcc.exe" 0 "tcc version 0\\.9\\.27")
  set(_guest_args "-o;C:\\Users\\User\\tcc_hello.exe;C:\\guest\\tcc_hello.c")
  set(_guest_file "Users/User/tcc_hello.exe")
  win10xbox_guest_test(tcc_compile "C:\\guest\\tcc\\tcc.exe" 0)
  unset(_guest_file)
  unset(_guest_args)
  set(_guest_no_user32 ON)   # tcc's exe imports only kernel32 and msvcrt
  win10xbox_guest_test(tcc_hello_runs "C:\\Users\\User\\tcc_hello.exe" 42
    "hello from tcc 42" "argc=1 sizeof\\(void\\*\\)=8")
  unset(_guest_no_user32)
  set_tests_properties(integration.tcc_hello_runs PROPERTIES DEPENDS integration.tcc_compile)
  set(_guest_args "-run;C:\\guest\\tcc_hello.c")
  win10xbox_guest_test(tcc_run "C:\\guest\\tcc\\tcc.exe" 42
    "hello from tcc 42" "argc=1 sizeof\\(void\\*\\)=8")
  unset(_guest_args)
  unset(_guest_machine)
endif()

# ---- ninja 1.12.1 (official ninja-win.zip, tools/ci/fetch_ninja.ps1; docs/slices/realninja.md) -----------------------
# ninja.exe runs on the shared machine folder next to lua.exe: ninja on Windows starts a rule's command with
# CreateProcess and no cmd.exe, so the two rules of tests/guest/ninja/ninja_build.ninja run C:\guest\lua.exe with
# ninja_tool.lua. Outputs go to C:\guest\ninja_out (builddir: .ninja_log too), the inputs are files already in C:\guest.
# The tests run in order (a fresh machine folder: a rerun on the same folder finds ninja_out and builds less).
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  configure_file(${CMAKE_SOURCE_DIR}/tests/guest/ninja/ninja_tool.lua ${CMAKE_BINARY_DIR}/bin/guest/ninja_tool.lua COPYONLY)
  configure_file(${CMAKE_SOURCE_DIR}/tests/guest/ninja/ninja_build.ninja ${CMAKE_BINARY_DIR}/bin/guest/ninja_build.ninja COPYONLY)
  set(_guest_args "--version")
  win10xbox_guest_test(ninja_version "C:\\guest\\ninja.exe" 0 "^1\\.12\\.1\n")
  set(_guest_args "-C;C:\\guest;-f;ninja_build.ninja;-t;targets;all")
  win10xbox_guest_test(ninja_targets "C:\\guest\\ninja.exe" 0
    "ninja_out[/\\]a\\.txt: copy" "ninja_out[/\\]b\\.txt: copy" "ninja_out[/\\]ab\\.txt: concat")
  set(_guest_args "-C;C:\\guest;-f;ninja_build.ninja")
  set(_guest_file "guest/ninja_out/ab.txt")
  win10xbox_guest_test(ninja_build "C:\\guest\\ninja.exe" 0 "\\[1/3\\]" "\\[2/3\\]" "\\[3/3\\] CONCAT ninja_out[/\\]ab\\.txt")
  unset(_guest_file)
  # Second run on the outputs the first one left: .ninja_log says everything is up to date.
  set(_guest_args "-C;C:\\guest;-f;ninja_build.ninja")
  win10xbox_guest_test(ninja_noop "C:\\guest\\ninja.exe" 0 "ninja: no work to do\\.")
  set_tests_properties(integration.ninja_noop PROPERTIES DEPENDS integration.ninja_build)
  unset(_guest_args)
endif()

# ---- The python.org embeddable CPython (docs/slices/realpython.md) ----------------------------------------------------
# The official CPython 3.12 embeddable package (python.org zip, nothing compiled), unpacked by
# tools/ci/fetch_python.ps1 (a named step of ci.yml's windows job) into <machine>/guest/python before ctest runs.
# Registered where that step exists: MSVC builds, or -DWIN10XBOX_REAL_PYTHON=ON after unpacking the zip there.
# Both tests run on the builtin-only machine folder (the console's DLL set, "*": "builtin,native"): python312.dll
# imports ws2_32, bcrypt, advapi32 and version, and the Windows CI runner's own build-26100 copies of those are
# written against an ntdll and an API-set schema Wine does not have (the failure that put the first run of these
# tests red: api-ms-win-core-crt-l2-1-0 -> _initterm_e resolved to nothing, Wine's 0x80000100 import stub).
option(WIN10XBOX_REAL_PYTHON "Register the real-Python guest tests (the embeddable zip in <machine>/guest/python)" OFF)
if(MSVC OR WIN10XBOX_REAL_PYTHON)
  configure_file(${CMAKE_SOURCE_DIR}/tests/guest/python/sample.json ${CMAKE_BINARY_DIR}/bin/guest/python_sample.json COPYONLY)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_console ON)
  # (a) the interpreter and the platform module: a version line, platform.uname() (os name, release and version from
  # the layer's Windows 10 identity) and os.cpu_count() as the last line. The ';' inside the program text is written
  # @SEMI@ (run_guest.cmake turns it back), so CMake keeps it in the one argument. The last regex wants the number
  # alone on the final line.
  set(_guest_args "-c;import sys, platform, os@SEMI@ print(sys.version)@SEMI@ print(platform.uname())@SEMI@ print(os.cpu_count())")
  win10xbox_guest_test(real_python_platform "C:\\guest\\python\\python.exe" 0
    "3\\.12\\.10 \\(tags/v3\\.12\\.10"
    "uname_result\\(system='Windows'"
    "\\)..?[0-9]+.?.?$")
  # (b) python -m json.tool on a small file: the module is found in python312.zip, the file is read through the
  # layer's VFS, and the pretty-printed document comes back on stdout, re-indented (the input is one line).
  set(_guest_args "-m;json.tool;C:\\guest\\python_sample.json")
  win10xbox_guest_test(real_python_json "C:\\guest\\python\\python.exe" 0
    "\"name\": \"win10xbox\""
    ".    \"list\": ."
    "\"ok\": true"
    "\"pi\": 3\\.25")
  unset(_guest_args)
  unset(_guest_console)
  unset(_guest_machine)
endif()

# ---- Real GUI programs (not ours), built from source on the Windows runner ------------------------------------------
# tools/ci/build_realgui.ps1 (a named ci.yml step of the windows job) leaves realgui_hellowin.exe (Microsoft's LearnWin32
# Hello World) and realgui_putty.exe (PuTTY 0.85, cmake + MSVC) in <machine>/guest before ctest runs. Registered only
# where that step exists: MSVC builds, or -DWIN10XBOX_REAL_GUI_PROGRAMS=ON after putting the exes there. These programs
# never exit by themselves, so the host's inject script (host-win32/main.cpp) drives the run: `waitwin` until the first
# window is painted, `shot` (writes WIN10XBOX_SCREENSHOT and logs the distinct colour count of the frame), `quit 0`.
# The gate: the screenshot was written and holds at least 3 distinct colours (desktop + frame + client; a blank
# screen has 1), and the wait did not time out.
option(WIN10XBOX_REAL_GUI_PROGRAMS "Register the real GUI program tests (realgui_*.exe must be in <machine>/guest)" OFF)
if(MSVC OR WIN10XBOX_REAL_GUI_PROGRAMS)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_expect_trace "shot: distinct=([3-9]|[1-9][0-9]+) written=1")
  set(_guest_forbid_trace "TIMEOUT: nothing painted")
  set(_realgui_script "waitwin 60000\\;sleep 2000\\;shot\\;quit 0")
  win10xbox_guest_test(realgui_hellowin "C:\\guest\\realgui_hellowin.exe" 0)
  set_tests_properties(integration.realgui_hellowin PROPERTIES
    ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/realgui_hellowin.bmp;WIN10XBOX_TEST_INJECT_INPUT=${_realgui_script}")
  # PuTTY: not registered by default. It builds and starts, but the loader stops at its imports (ole32.dll and
  # SHELL32.dll are not in the Wine builtin set the machine folder gets: STATUS_DLL_NOT_FOUND c0000135); register it
  # with -DWIN10XBOX_REAL_GUI_PUTTY=ON once they are. With a host and a protocol on the command line it skips the configuration dialog and creates the terminal.
  option(WIN10XBOX_REAL_GUI_PUTTY "Register integration.realgui_putty" OFF)
  if(WIN10XBOX_REAL_GUI_PUTTY)
    set(_guest_args "-telnet;127.0.0.1;-P;9")
    win10xbox_guest_test(realgui_putty "C:\\guest\\realgui_putty.exe" 0)
    set_tests_properties(integration.realgui_putty PROPERTIES
      ENVIRONMENT "WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/realgui_putty.bmp;WIN10XBOX_TEST_INJECT_INPUT=${_realgui_script}")
  endif()
  unset(_guest_args)
  unset(_guest_expect_trace)
  unset(_guest_forbid_trace)
  unset(_realgui_script)
  unset(_guest_machine)
endif()

# ---- Chocolate Doom 3.1.1 (official win64 zip) with the shareware doom1.wad: the first game --------------------------
# tools/ci/fetch_doom.ps1 (a named ci.yml step of the windows job) leaves chocolate-doom.exe, its SDL2 DLLs and doom1.wad
# in <machine>/guest before ctest runs (docs/slices/doom.md). Registered where the Lua tests are: MSVC builds, or
# -DWIN10XBOX_REAL_PROGRAMS=ON with those files staged; a missing binary FAILS (the host cannot start it), it never skips.
# The game runs demo1 as fast as it can (-timedemo) in a 640x400 window on SDL2's software renderer, whose blits go
# through GDI onto the window (core/user). Chocolate Doom ends a timedemo by reporting it through I_Error: the stats line
# "timed N gametics in M realtics (F fps)" goes to STDERR (the trace checks below read the guest's stderr too) and the
# process exits with -1. -nogui keeps I_Error from opening a message box that nobody would close.
# Gates: exit -1 after the whole demo, the stats line, the host's `shot` seen at least 3 distinct colours in the frame
# 300 frames into the demo (waitflush), and the frame at exit (WIN10XBOX_SCREENSHOT is rewritten when the process ends,
# so the BMP is the last frame of the demo, deterministic) holds the status bar: pixels of its grey panel and of its red digits.
# The window is centred on the 1920x1080 virtual screen at (685,309), client origin (693,340).
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_args "-iwad;C:\\guest\\doom1.wad;-nosound;-nomusic;-nomouse;-window;-geometry;640x400;-nogui;-timedemo;demo1")
  set(_guest_debug "-all,err+all,fixme+all")
  set(_guest_run_timeout 280)
  set(_guest_expect_trace "timed [0-9]+ gametics in [0-9]+ realtics \\([0-9.]+ fps\\);shot: distinct=([3-9]|[1-9][0-9]+) written=1")
  set(_guest_forbid_trace "TIMEOUT")
  win10xbox_guest_test(doom_timedemo "C:\\guest\\chocolate-doom.exe" -1 "Chocolate Doom 3\\.1\\.1" "adding C:.guest.doom1\\.wad" "Playing demo demo1")
  set_tests_properties(integration.doom_timedemo PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "SDL_RENDER_DRIVER=software;WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/doom_timedemo.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=715,694,ff303030|785,694,ff585858|707,689,ffc80000|735,704,ffbc0000;WIN10XBOX_TEST_INJECT_INPUT=waitflush 300 250000\\;shot")
  unset(_guest_args)
  unset(_guest_debug)
  unset(_guest_run_timeout)
  unset(_guest_expect_trace)
  unset(_guest_forbid_trace)
  unset(_guest_machine)
endif()

# ---- Real program: Git for Windows (MinGit 2.47.1 x64) ------------------------------------------------------------
# tools/ci/fetch_mingit.ps1 (a named ci.yml step of the windows job) unpacks MinGit into <machine>/guest/mingit before
# ctest; prepare_machine.cmake mirrors it into the builtin-only folder these tests run on (git.exe imports msvcrt,
# advapi32, user32 and ws2_32: on the shared folder the runner's native advapi32/sechost are mapped next to Wine's
# msvcrt, the 0x80000100 import-stub failure of docs/TESTING.md). mingw64/bin/git.exe is run directly (cmd/git.exe
# only wraps it). Registered where the Lua tests are: MSVC builds, or -DWIN10XBOX_REAL_PROGRAMS=ON with MinGit staged.
# The tests share one repository, C:\guest\gitrepo, and are ordered by fixtures: prepare -> init -> add -> commit.
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  set(_git "C:\\guest\\mingit\\mingw64\\bin\\git.exe")
  set(_repo "C:\\guest\\gitrepo")
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  add_test(NAME integration.real_git_prepare
    COMMAND ${CMAKE_COMMAND} -DREPO=${WIN10XBOX_BUILTIN_MACHINE_DIR}/guest/gitrepo
      -P ${CMAKE_CURRENT_SOURCE_DIR}/real_git_prepare.cmake)
  set_tests_properties(integration.real_git_prepare PROPERTIES
    FIXTURES_REQUIRED w10x_machine FIXTURES_SETUP w10x_git_prepared)
  # git's own configuration for the run: no pager, no CRLF conversion, no signing, a fixed identity, and the repository
  # directory trusted (the ownership check is git's, not the layer's).
  set(_gitcfg "--no-pager;-c;core.autocrlf=false;-c;commit.gpgsign=false;-c;user.name=w10x;-c;user.email=w10x@example.com;-c;safe.directory=*")
  set(_guest_args "--version")
  win10xbox_guest_test(real_git "${_git}" 0 "git version 2\\.47\\.1\\.windows\\.1")
  set(_guest_args "${_gitcfg};init;${_repo}")
  win10xbox_guest_test(real_git_init "${_git}" 0 "Initialized empty Git repository in .*gitrepo/\\.git/")
  set_tests_properties(integration.real_git_init PROPERTIES
    FIXTURES_REQUIRED "w10x_machine;w10x_git_prepared" FIXTURES_SETUP w10x_git_init)
  set(_guest_args "${_gitcfg};-C;${_repo};add;hello.txt")
  win10xbox_guest_test(real_git_add "${_git}" 0)
  set_tests_properties(integration.real_git_add PROPERTIES
    FIXTURES_REQUIRED "w10x_machine;w10x_git_init" FIXTURES_SETUP w10x_git_add)
  set(_guest_args "${_gitcfg};-C;${_repo};status;--short")
  win10xbox_guest_test(real_git_status "${_git}" 0 "A  hello\\.txt")
  set_tests_properties(integration.real_git_status PROPERTIES
    FIXTURES_REQUIRED "w10x_machine;w10x_git_add" FIXTURES_SETUP w10x_git_status)
  set(_guest_args "${_gitcfg};-C;${_repo};commit;-m;layer commit")
  win10xbox_guest_test(real_git_commit "${_git}" 0 "\\[[a-z]+ \\(root-commit\\) [0-9a-f]+\\] layer commit")
  set_tests_properties(integration.real_git_commit PROPERTIES
    FIXTURES_REQUIRED "w10x_machine;w10x_git_status" FIXTURES_SETUP w10x_git_commit)
  set(_guest_args "${_gitcfg};-C;${_repo};log;--oneline")
  win10xbox_guest_test(real_git_log "${_git}" 0 "[0-9a-f]+ layer commit")
  set_tests_properties(integration.real_git_log PROPERTIES FIXTURES_REQUIRED "w10x_machine;w10x_git_commit")
  unset(_guest_args)
  unset(_guest_machine)
  unset(_gitcfg)
  unset(_git)
  unset(_repo)
endif()

# Route B (docs/GUI.md "Route B: native win32k"): the same window.c on the machine folder machine-native-gui
# (machine.json "gui": "native"; Wine builtins + the six native GUI DLLs, see prepare_machine.cmake
# NATIVE_GUI_MACHINE): the guest loads the NATIVE user32/gdi32/gdi32full/win32u from the machine folder, every
# win32u syscall is passed through by name to the host's own win32u.dll, win32k's user-mode callbacks reach the
# guest's user32 through the trampolines, and the screenshot is composed from the real window. Needs the native
# DLLs, so it is registered only for a native Windows build (the Windows CI job stages them from the runner's
# System32); a cross build run under Wine has no Windows DLLs and does not register it at all (the CI skip gate
# fails on skipped tests). Without the DLLs in the machine folder it still SKIPs rather than failing obscurely.
if(NOT CMAKE_CROSSCOMPILING)
add_test(NAME integration.guest_window_native
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:win10xbox>
    -DMACHINE=${WIN10XBOX_NATIVE_GUI_MACHINE_DIR}
    -DPROGRAM=C:\\guest\\guest_window.exe
    -DEXPECT_EXIT=42
    "-DEXPECT_REGEX=window: class registered;window: created hwnd=;window: painted;window: pixel\\(10,10\\)=00dc5000 pixel\\(30,30\\)=000000ff;window: done"
    "-DEXPECT_TRACE_REGEX=gui native: [0-9]+ win32u syscalls pass through;gui native: the guest's win32u syscalls go to the host's win32k;host path [^)]*[Ss]ystem32.[Uu][Ss][Ee][Rr]32.dll;host path [^)]*[Ss]ystem32.[Ww][Ii][Nn]32[Uu].dll;win32u.dll: [0-9]+ of [0-9]+ syscall thunks renumbered;PEB.KernelCallbackTable: the guest's user32 installed;user-mode callback #[0-9]+;native compose: [1-9][0-9]* guest window"
    "-DFORBID_TRACE_REGEX=host state reached under the guest TLS view"
    -DDEBUG_SPEC=+dispatch,+modules,+process,+user
    -DREQUIRE=Windows/System32/user32.dll
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
set_tests_properties(integration.guest_window_native PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "guest_e2e"
  TIMEOUT 180
  # The screen pixels as 0xAARRGGBB from the BMP's B,G,R,A bytes: RGB(0,80,220) is ff0050dc, RGB(255,0,0) ffff0000.
  # WIN10XBOX_WINEDEBUG: Wine's own loader warnings ("No implementation for ntdll.dll.X", failed DllMains).
  ENVIRONMENT "WIN10XBOX_WINEDEBUG=+loaddll,warn+module;WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_window_native.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=110,110,ff0050dc|130,130,ffff0000")
endif()

# Direct3D 11 off-screen, route B (docs/slices/d3d.md): tests/guest/d3d11_tri.c on machine-native-d3d. The guest loads the
# NATIVE d3d11/dxgi/d3dcompiler_47 and the user-mode driver (hardware, else WARP), creates a device, renders one triangle
# into a render-target texture and reads the centre and a corner pixel back through a staging texture; the D3DKMT calls
# of those DLLs (NtGdiDdDDI* in the native win32u) are forwarded by name to the host's win32u. Exit 42 = centre is the
# triangle colour and the corner the clear colour. Native Windows builds only (no Windows DLLs under Wine on linux-cross,
# where the test is not registered at all so the skip gate never counts it).
if(NOT CMAKE_CROSSCOMPILING AND TARGET guest_d3d11_tri)
add_test(NAME integration.guest_d3d11
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:win10xbox>
    -DMACHINE=${WIN10XBOX_D3D_MACHINE_DIR}
    -DPROGRAM=C:\\guest\\guest_d3d11_tri.exe
    -DEXPECT_EXIT=42
    -DSAVE_STDOUT=${CMAKE_BINARY_DIR}/bin/guest_d3d11.log   # the "d3d11: adapter=" line, uploaded by CI
    "-DEXPECT_REGEX=d3d11: adapter=.* feature_level=0x[0-9a-f][0-9a-f][0-9a-f][0-9a-f] driver=(hardware|warp);d3d11: centre=ff0000ff corner=0000ffff;d3d11: done"
    "-DEXPECT_TRACE_REGEX=gui native: [0-9]+ win32u syscalls pass through;NtGdiDdDDI\\* \\(D3DKMT\\) thunks forwarded by name;native d3d: first D3DKMT call NtGdiDdDDI;host path [^)]*[Ss]ystem32.[Dd]3[Dd]11.dll;host path [^)]*[Ss]ystem32.[Dd][Xx][Gg][Ii].dll"
    "-DFORBID_TRACE_REGEX=host state reached under the guest TLS view"
    -DDEBUG_SPEC=+dispatch,+modules,+process,+user
    -DREQUIRE=Windows/System32/d3d11.dll
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
set_tests_properties(integration.guest_d3d11 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "guest_e2e"
  TIMEOUT 240
  ENVIRONMENT "WIN10XBOX_WINEDEBUG=+loaddll,warn+module,+seh,+rpc,+service")
endif()

# A swap chain on a window, route B (docs/slices/swapchain2.md): tests/guest/d3d11_swap.c on the same machine. The layer
# interposes the guest's dxgi (CreateDXGIFactory*) and d3d11 (D3D11CreateDeviceAndSwapChain), so the swap chain is
# a wrapper around the real one; after each Present the real back buffer is copied to a staging texture and handed
# to core/user, which shows it in the window's client rectangle. Exit 42 (readback centre red, corner blue) and the
# composed screenshot at the window (100,100, 256x256): centre (228,228) red, (101,101) blue. Same condition as
# integration.guest_d3d11.
if(NOT CMAKE_CROSSCOMPILING AND TARGET guest_d3d11_swap)
add_test(NAME integration.guest_d3d11_swap
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:win10xbox>
    -DMACHINE=${WIN10XBOX_D3D_MACHINE_DIR}
    -DPROGRAM=C:\\guest\\guest_d3d11_swap.exe
    -DEXPECT_EXIT=42
    -DSAVE_STDOUT=${CMAKE_BINARY_DIR}/bin/guest_d3d11_swap.log
    "-DEXPECT_REGEX=swap: window hwnd=;swap: CreateDXGIFactory1 -> 00000000;swap: CreateSwapChain -> 00000000;swap: readback centre=ffff0000 corner=ff0000ff;swap: presented 60 frames;swap: done"
    "-DEXPECT_TRACE_REGEX=dxgi: dxgi.dll: 3 export\\(s\\) interposed;dxgi: d3d11.dll: 1 export\\(s\\) interposed;dxgi: CreateDXGIFactory1 -> 00000000;dxgi: IDXGIFactory::CreateSwapChain\\(window [0-9a-fA-Fx]+, 256x256[^)]*\\) -> 00000000;dxgi: IDXGISwapChain::Present\\(1, 0x0\\) -> 0000000[0-9] \\(back buffer copied\\)"
    "-DFORBID_TRACE_REGEX=host state reached under the guest TLS view;not handed to the compositor"
    -DDEBUG_SPEC=+dispatch,+modules,+process,+user
    -DREQUIRE=Windows/System32/d3d11.dll
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
set_tests_properties(integration.guest_d3d11_swap PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "guest_e2e"
  TIMEOUT 240
  ENVIRONMENT "WIN10XBOX_WINEDEBUG=+loaddll,warn+module;WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_d3d11_swap.bmp;WIN10XBOX_EXPECT_SCREENSHOT_PIXELS=228,228,ffff0000|101,101,ff0000ff")
endif()

# The same program under the generic present-limit harness (WIN10XBOX_TEST_PRESENT_LIMIT, docs/TESTING.md): the layer
# ends the guest with exit 42 after its 10th Present and prints the "present-limit:" line; the last composed frame
# (window at 100,100, 256x256) must still be on the screenshot, non-uniform (blue clear + triangle).
if(NOT CMAKE_CROSSCOMPILING AND TARGET guest_d3d11_swap)
add_test(NAME integration.guest_d3d11_swap_limit
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:win10xbox>
    -DMACHINE=${WIN10XBOX_D3D_MACHINE_DIR}
    -DPROGRAM=C:\\guest\\guest_d3d11_swap.exe
    -DEXPECT_EXIT=42
    -DSAVE_STDOUT=${CMAKE_BINARY_DIR}/bin/guest_d3d11_swap_limit.log
    "-DEXPECT_REGEX=swap: CreateSwapChain -> 00000000;present-limit: 10 frames presented, 256x256, "
    "-DEXPECT_TRACE_REGEX=dxgi: IDXGISwapChain::Present\\(1, 0x0\\) -> 0000000[0-9] \\(back buffer copied\\);present-limit: 10 frames presented"
    "-DFORBID_TRACE_REGEX=host state reached under the guest TLS view;not handed to the compositor"
    -DDEBUG_SPEC=+dispatch,+modules,+process,+user
    -DREQUIRE=Windows/System32/d3d11.dll
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
set_tests_properties(integration.guest_d3d11_swap_limit PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "guest_e2e"
  TIMEOUT 240
  ENVIRONMENT "WIN10XBOX_TEST_PRESENT_LIMIT=10;WIN10XBOX_WINEDEBUG=+loaddll,warn+module;WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_d3d11_swap_limit.bmp;WIN10XBOX_EXPECT_SCREENSHOT_VARIED=100,100,356,356,8,2")
endif()

# Route B + Direct3D 11 (docs/DECISIONS.md G1): Dear ImGui's example_win32_directx11 (tests/guest/imgui_d3d11.cpp,
# built when CI provides the Dear ImGui source) on machine-native-d3d: native user32/gdi32/win32u and the native
# d3d11/dxgi/d3dcompiler_47/d3d10warp as guest code, every win32u syscall -- the D3DKMT ones (NtGdiDdDDI*) included --
# passed through by name to the host's win32u. It runs WIN10XBOX_IMGUI_FRAMES frames, exits 0, and the screenshot
# (composed from the real window) must show the ImGui window region non-uniform: at least 3 colours on a 40-pixel
# grid, the example's clear colour (0.45, 0.55, 0.60) = ff738c99 among them.
if(NOT CMAKE_CROSSCOMPILING AND WIN10XBOX_HAVE_IMGUI_GUEST)
add_dependencies(prepare_machine guest_imgui_d3d11)
add_test(NAME integration.guest_imgui_d3d11
  COMMAND ${CMAKE_COMMAND}
    -DHOST=$<TARGET_FILE:win10xbox>
    -DMACHINE=${WIN10XBOX_D3D_MACHINE_DIR}
    -DPROGRAM=C:\\guest\\guest_imgui_d3d11.exe
    -DEXPECT_EXIT=0
    "-DEXPECT_REGEX=imgui: D3D11CreateDeviceAndSwapChain\\(driver type [0-9]\\) -> 00000000;imgui: adapter ;imgui: frame 1 presented;imgui: done frames=60"
    "-DEXPECT_TRACE_REGEX=gui native: [0-9]+ win32u syscalls pass through;host path [^)]*[Ss]ystem32.[Dd]3[Dd]11.dll;host path [^)]*[Ss]ystem32.[Dd][Xx][Gg][Ii].dll"
    "-DFORBID_TRACE_REGEX=host state reached under the guest TLS view"
    -DDEBUG_SPEC=+dispatch,+modules,+process,+user
    -DREQUIRE=Windows/System32/d3d11.dll
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_guest.cmake)
# The on-screen check of the presented frames (WIN10XBOX_EXPECT_SCREENSHOT_VARIED over the client area): the layer's
# D3D11CreateDeviceAndSwapChain wrapper copies each presented back buffer into the composed frame
# (docs/slices/swapchain2.md).
set_tests_properties(integration.guest_imgui_d3d11 PROPERTIES
  FIXTURES_REQUIRED w10x_machine
  SKIP_RETURN_CODE 77
  SKIP_REGULAR_EXPRESSION "SKIP:"
  LABELS "guest_e2e"
  TIMEOUT 240
  ENVIRONMENT "WIN10XBOX_IMGUI_FRAMES=60;WIN10XBOX_WINEDEBUG=+loaddll,warn+module;WIN10XBOX_SCREENSHOT=${CMAKE_BINARY_DIR}/bin/guest_imgui_d3d11.bmp;WIN10XBOX_EXPECT_SCREENSHOT_VARIED=110,140,1010,760,40,3,ff738c99")
endif()

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

# ---- Real program: 7-Zip's 7zr.exe (docs/TESTING.md, "Real programs") --------------------------------------------
# Built from the official source by tools/ci/build_7zip.ps1 (a named step of ci.yml's windows job, which leaves it at
# <machine>/guest/7zr.exe before ctest runs). Registered only where that step exists (MSVC) or with
# -DWIN10XBOX_REAL_PROGRAMS=ON after putting a 7zr.exe into the machine folder; never on linux-cross's skip floor.
# real7z: 7zr a / t / l / x round trip (SHA-256 of every extracted file); real7z_bench: 7zr b -mmt1.
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  foreach(_mode roundtrip bench)
    if(_mode STREQUAL "roundtrip")
      set(_t integration.real7z)
    else()
      set(_t integration.real7z_bench)
    endif()
    add_test(NAME ${_t}
      COMMAND ${CMAKE_COMMAND} -DHOST=$<TARGET_FILE:win10xbox> -DMACHINE=${WIN10XBOX_BUILTIN_MACHINE_DIR}
        -DMODE=${_mode} -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR} -P ${CMAKE_CURRENT_SOURCE_DIR}/real7z.cmake)
    set_tests_properties(${_t} PROPERTIES
      FIXTURES_REQUIRED w10x_machine
      SKIP_RETURN_CODE 77
      SKIP_REGULAR_EXPRESSION "SKIP:"
      LABELS "guest_e2e;real7z"
      TIMEOUT 900)
  endforeach()
endif()

# The SQLite shell and the regression guests its bring-up needed (tests/guest/sqlite).
include(${CMAKE_CURRENT_LIST_DIR}/sqlite_tests.cmake)

# ---- Real program: curl's curl.exe (docs/TESTING.md, "Real programs") ---------------------------------------------
# Built from the release tarball by tools/ci/build_curl.ps1 (CMake + MSVC, no TLS; a named step of ci.yml's windows
# job, which leaves it at <machine>/guest/curl.exe before ctest runs). Registered like the Lua tests: MSVC builds, or
# -DWIN10XBOX_REAL_PROGRAMS=ON after putting a curl.exe into the machine folder. No network: --version, and the
# file:// handler reading and copying files under the machine folder's guest home (C:\Users\User).
if(MSVC OR WIN10XBOX_REAL_PROGRAMS)
  # curl links ws2_32, which on the shared folder is the runner's native one (its advapi32/sechost/rpcrt4 chain dies
  # on Wine's 0x80000100 import stub, docs/TESTING.md "GUI guests"): run on the builtin-only folder, where it is Wine's.
  # curl never loads user32, which that folder's trace check otherwise demands.
  set(_guest_machine ${WIN10XBOX_BUILTIN_MACHINE_DIR})
  set(_guest_no_user32 ON)
  set(_curl_home ${WIN10XBOX_BUILTIN_MACHINE_DIR}/Users/User)
  file(MAKE_DIRECTORY ${_curl_home})
  file(WRITE ${_curl_home}/curl_in.txt "hello from a guest file, read by curl\nsecond line\n")
  string(REPEAT "0123456789abcdef" 20000 _curl_big)   # 320000 bytes
  file(WRITE ${_curl_home}/curl_big.bin "${_curl_big}")
  unset(_curl_big)
  # (a) the banner and the protocol list (no TLS backend: https must not be listed).
  set(_guest_args "--version")
  win10xbox_guest_test(real_curl_version "C:\\guest\\curl.exe" 0
    "curl 8\\.[0-9]+\\.[0-9]+ " "libcurl/8\\." "Protocols: [a-z0-9 ]*file" "Protocols: [a-z0-9 ]*http" "Features: ")
  set_tests_properties(integration.real_curl_version PROPERTIES FAIL_REGULAR_EXPRESSION "Protocols: [a-z0-9 ]*https")
  # (b) file:// of a guest file to stdout.
  set(_guest_args "-sS;file:///C:/Users/User/curl_in.txt")
  win10xbox_guest_test(real_curl_file "C:\\guest\\curl.exe" 0
    "hello from a guest file, read by curl.second line")
  # (c) file:// copied with -o (a new file under the guest home, which must exist afterwards); curl reports the size.
  set(_guest_args "-sS;-o;C:\\Users\\User\\curl_out.bin;-w;size=%{size_download};file:///C:/Users/User/curl_big.bin")
  set(_guest_file Users/User/curl_out.bin)
  win10xbox_guest_test(real_curl_file_copy "C:\\guest\\curl.exe" 0 "size=320000")
  unset(_guest_file)
  # (d) a missing file: CURLE_FILE_COULDNT_READ_FILE, exit code 37.
  set(_guest_args "-sS;file:///C:/Users/User/curl_missing.txt")
  win10xbox_guest_test(real_curl_file_missing "C:\\guest\\curl.exe" 37 "")
  unset(_guest_args)
  unset(_guest_machine)
  unset(_guest_no_user32)
endif()
