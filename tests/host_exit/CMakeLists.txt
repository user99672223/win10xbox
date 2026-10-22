# How win10xbox.exe ENDS. host-win32-smoke on 532a3bf was the first real-Windows run in which a guest ran to completion,
# and the host then died in ExitProcess's DLL detach (an unhandled 0xC000000D from combase.dll, which only dbghelp.dll
# pulls in): exit code 127 instead of the guest's 42, after a 3 minute stall. The earlier tests could not see it:
# integration.host_hello / host_version are PASS_REGULAR_EXPRESSION tests, which ignore the exit code, and the guest
# tests all died before reaching the end. See the header comment of host-win32/main.cpp.
#
# The fault itself needs a Microsoft ntdll and combase, so no Wine job can reproduce it. What CAN be gated everywhere is
# the property that makes it harmless: nothing a DLL does at process exit may change the exit code. exit_probe.dll stands
# in for the misbehaving DLL (its detach ends the process with 99); the host must be gone before that code runs.
#
# Needs the machine folder of tests/integration (prepare_machine, fixture w10x_machine) for the guest runs, which SKIP
# without the Wine builtins or the NLS data like every guest test; label guest_e2e, so on the MSVC job they run
# non-blocking with the rest of the real-Windows bring-up. The no-guest test needs neither and gates everywhere.
add_library(host_exit_probe SHARED exit_probe.c)
set_target_properties(host_exit_probe PROPERTIES
  PREFIX ""
  OUTPUT_NAME exit_probe
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/host_exit
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/host_exit
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/host_exit)

# win10xbox_host_exit_test(<name> <expected exit code>
#     [GUEST <guest program>] [PROBE] [NORMAL_EXIT] [ACTCTX] [LEAK_ACTCTX]
#     [OUT <stdout regex>...] [ERR <stderr regex>...] [FORBID <stderr regex>])
function(win10xbox_host_exit_test name expect_exit)
  cmake_parse_arguments(A "PROBE;NORMAL_EXIT;ACTCTX;LEAK_ACTCTX" "GUEST;FORBID" "OUT;ERR" ${ARGN})
  set(args
    -DHOST=$<TARGET_FILE:win10xbox>
    -DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}
    -DEXPECT_EXIT=${expect_exit})
  if(A_GUEST)
    list(APPEND args -DMACHINE=${WIN10XBOX_MACHINE_DIR} "-DPROGRAM=${A_GUEST}")
  endif()
  if(A_PROBE)
    list(APPEND args -DEXIT_PROBE_DLL=$<TARGET_FILE:host_exit_probe>)
  endif()
  if(A_NORMAL_EXIT)
    list(APPEND args -DNORMAL_EXIT=1)
  endif()
  if(A_ACTCTX)
    list(APPEND args -DHOST_ACTCTX=1)
  endif()
  if(A_LEAK_ACTCTX)
    list(APPEND args -DLEAK_ACTCTX=1)
  endif()
  if(A_OUT)
    list(APPEND args "-DEXPECT_OUT_REGEX=${A_OUT}")
  endif()
  if(A_ERR)
    list(APPEND args "-DEXPECT_ERR_REGEX=${A_ERR}")
  endif()
  if(A_FORBID)
    list(APPEND args "-DFORBID_ERR_REGEX=${A_FORBID}")
  endif()
  add_test(NAME integration.${name}
    COMMAND ${CMAKE_COMMAND} ${args} -P ${CMAKE_CURRENT_SOURCE_DIR}/run_host.cmake)
  set_tests_properties(integration.${name} PROPERTIES TIMEOUT 180)
  if(A_GUEST)
    set_tests_properties(integration.${name} PROPERTIES
      FIXTURES_REQUIRED w10x_machine
      SKIP_RETURN_CODE 77
      SKIP_REGULAR_EXPRESSION "SKIP:"
      LABELS "guest_e2e")
  endif()
endfunction()

# 1. No guest: the hello path leaves with exit code 0, and its output survives the exit (stdio is flushed by hand now).
win10xbox_host_exit_test(host_exit_code_hello 0 OUT "hello from host-win32")

# 2. The property: with a DLL whose detach would end the process with 99, the exit code is still the guest's 42.
win10xbox_host_exit_test(host_exit_is_guests 42 GUEST "C:\\guest\\guest_hello.exe" PROBE
  OUT "hello from guest"
  ERR "exit probe .* loaded" "guest exit code 42 \\(0x0000002a\\); ending the process with TerminateProcess")

# 3. Not vacuous: through the normal teardown (WIN10XBOX_NORMAL_EXIT=1, the diagnostic switch) the same run DOES hit the
# probe, so test 2 passes because the host skips DLL detach and not because the probe never loaded or never fires.
win10xbox_host_exit_test(host_exit_probe_fires_on_normal_exit 99 GUEST "C:\\guest\\guest_hello.exe" PROBE NORMAL_EXIT
  OUT "hello from guest"
  ERR "exit probe .* loaded")

# 4. The take-over gives the host back its process-default activation context. Wine's loader_init overwrites
# PEB.ActivationContextData in the guest (actctx.c actctx_init); the host plants a marker there first and checks it
# afterwards. Wine hosts only; on a Microsoft ntdll the host prints "check skipped" and does nothing.
win10xbox_host_exit_test(host_actctx_restored 42 GUEST "C:\\guest\\guest_hello.exe" ACTCTX
  OUT "hello from guest"
  ERR "ActivationContextData (intact after the guest|check skipped)"
  FORBID "CLOBBERED")

# 5. Not vacuous either: with the restore switched off (WIN10XBOX_TEST_LEAK_GUEST_ACTCTX=1, the behaviour from before it
# existed) the same check reports the guest's value. Wine hosts only, like test 4.
win10xbox_host_exit_test(host_actctx_leak_detected 42 GUEST "C:\\guest\\guest_hello.exe" ACTCTX LEAK_ACTCTX
  OUT "hello from guest"
  ERR "ActivationContextData (CLOBBERED by the guest|check skipped)")
