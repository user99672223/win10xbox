# integration.guest_tcp_echo: runs tests/integration/tcp_echo_runner (a host-side echo server) which in turn runs
#   win10xbox.exe --machine <builtin machine> C:\guest\guest_tcp_echo.exe {PORT}
# with {PORT} replaced by the echo server's port. The guest talks to the server through Wine's ws2_32, \Device\Afd
# (core/afd) and host Winsock.
#   -DRUNNER=<tcp_echo_runner.exe> -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> -DEXPECT_EXIT=<n> [-DEMULATOR=<wine>]
# Prints "SKIP: ..." and exits 77 when the machine folder has no Wine builtins / NLS data (as run_guest.cmake does).
if(NOT EXISTS "${MACHINE}/Windows/builtin/ws2_32.dll")
  message("SKIP: ${MACHINE}/Windows/builtin/ws2_32.dll missing: build Wine's PE DLLs (wine-pe/build.sh) or download "
          "the wine-pe artifact into the machine folder")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
if(NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  message("SKIP: ${MACHINE}/Windows/System32/c_1252.nls missing: the guest's ntdll needs the Windows NLS data")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
set(_machine "${MACHINE}")
set(_host "${HOST}")
set(_runner "${RUNNER}")
if(EMULATOR AND _machine MATCHES "^/")
  set(_machine "Z:${_machine}")
  set(_host "Z:${_host}")
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
string(REPLACE "/" "\\" _host "${_host}")
set(ENV{WIN10XBOX_DEBUG} "+modules")
execute_process(
  COMMAND ${EMULATOR} "${_runner}" ${EXPECT_EXIT} -- "${_host}" --machine "${_machine}" "C:\\guest\\guest_tcp_echo.exe" "{PORT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 150)
message("${out}")
if(err)
  message("--- stderr ---\n${err}")
endif()
set(_fail "")
if(NOT "${rc}" STREQUAL "0")
  string(APPEND _fail "tcp_echo_runner exit code ${rc}\n")
endif()
foreach(re "tcp_echo: localhost -> 127\\.0\\.0\\.1" "tcp_echo: socket created" "tcp_echo: connected"
           "tcp_echo: sent 30 bytes" "tcp_echo: received 30 bytes: hello from the guest over AFD" "tcp_echo: done"
           "tcp_echo_runner: PASS")
  if(NOT out MATCHES "${re}")
    string(APPEND _fail "stdout does not match '${re}'\n")
  endif()
endforeach()
# The guest's ws2_32 must be Wine's builtin (so the AFD ioctls, not a native ws2_32, carried the traffic).
if(NOT err MATCHES "C:\\\\Windows\\\\System32\\\\ws2_32\\.dll \\([0-9]+ bytes, host path [^)]*\\\\Windows\\\\builtin\\\\ws2_32\\.dll\\)")
  string(APPEND _fail "ws2_32.dll was not mapped from Windows\\builtin (modules trace)\n")
endif()
if(_fail)
  message(FATAL_ERROR "guest_tcp_echo:\n${_fail}")
endif()
message("PASS: guest_tcp_echo")
