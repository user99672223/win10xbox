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
set(_default_json ${CMAKE_SOURCE_DIR}/machine/machine.default.json)
set(_prepare_args
  -DMACHINE=${WIN10XBOX_MACHINE_DIR}
  -DBUILTIN_DIR=${WIN10XBOX_BUILTIN_DIR}
  -DGUEST_DIR=${CMAKE_BINARY_DIR}/bin/guest
  -DDEFAULT_JSON=${_default_json}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/prepare_machine.cmake)

add_custom_target(prepare_machine COMMAND ${CMAKE_COMMAND} ${_prepare_args}
  COMMENT "Preparing integration machine folder ${WIN10XBOX_MACHINE_DIR}" VERBATIM)
if(TARGET guest_hello)
  add_dependencies(prepare_machine guest_hello guest_cpuid)
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

function(win10xbox_guest_test name program exit_code)
  add_test(NAME integration.${name}
    COMMAND ${CMAKE_COMMAND}
      -DHOST=$<TARGET_FILE:win10xbox>
      -DMACHINE=${WIN10XBOX_MACHINE_DIR}
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
