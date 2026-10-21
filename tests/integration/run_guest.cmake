# Runs one guest program through host-win32 and checks its output and exit
# code (cmake -P; ctest's PASS_REGULAR_EXPRESSION alone ignores exit codes).
#   -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> -DPROGRAM=<C:\guest\x.exe>
#   -DEXPECT_EXIT=<n> -DEXPECT_REGEX=<regex;regex...> [-DEMULATOR=<wine>]
#   [-DFORBID_REGEX=<regex>] [-DDEBUG_SPEC=<WIN10XBOX_DEBUG spec>]
# Prints "SKIP: ..." and exits 77 (cmake >= 3.29) when the machine folder has
# no Wine builtins; the test sets both SKIP_RETURN_CODE 77 and
# SKIP_REGULAR_EXPRESSION "SKIP:".
if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll")
  message("SKIP: ${MACHINE}/Windows/builtin/ntdll.dll missing: build Wine's PE DLLs (wine-pe/build.sh) "
          "or download the wine-pe artifact into the machine folder")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
string(REPLACE "/" "\\" _program "${PROGRAM}")
set(_machine "${MACHINE}")
if(EMULATOR AND _machine MATCHES "^/")
  # Wine: Z: is the Unix root.
  set(_machine "Z:${_machine}")
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
set(_cmd ${EMULATOR} "${HOST}" --machine "${_machine}" "${_program}")
if(DEBUG_SPEC)
  set(ENV{WIN10XBOX_DEBUG} "${DEBUG_SPEC}")
endif()
execute_process(COMMAND ${_cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
message("${out}")
if(err)
  message("--- stderr ---\n${err}")
endif()
set(_fail "")
if(NOT "${rc}" STREQUAL "${EXPECT_EXIT}")
  string(APPEND _fail "exit code ${rc}, expected ${EXPECT_EXIT}\n")
endif()
foreach(re ${EXPECT_REGEX})
  if(NOT out MATCHES "${re}")
    string(APPEND _fail "stdout does not match '${re}'\n")
  endif()
endforeach()
if(FORBID_REGEX AND out MATCHES "${FORBID_REGEX}")
  string(APPEND _fail "stdout matches forbidden '${FORBID_REGEX}'\n")
endif()
if(_fail)
  message(FATAL_ERROR "${PROGRAM}:\n${_fail}")
endif()
message("PASS: ${PROGRAM} (exit ${rc})")
