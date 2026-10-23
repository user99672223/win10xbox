# Runs one Wine conformance test binary through win10xbox and records the
# result (cmake -P). Modelled on tests/integration/run_guest.cmake.
#
#   -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> -DNAME=<dll>_<test>
#   -DEXPECT=<run|crash|timeout> -DMAX_FAILURES=<n> [-DTIMEOUT=<s, default 60>]
#   [-DRESULTS_DIR=<dir>] [-DEMULATOR=<wine>] [-DDEBUG_SPEC=<WIN10XBOX_DEBUG spec>]
#
# The guest is C:\guest-wine\<NAME>.exe (staged by stage_wine_tests.cmake).
# Wine's test.h prints "<pid>:<test>:<time> N tests executed (T marked as todo,
# F as flaky, K failures), S skipped." when the test function returns. Outcome:
#   ok       summary line found
#   crashed  no summary line (unhandled exception, guest main thread ended
#            early, or the layer failed to start the program)
#   timeout  still running after TIMEOUT seconds (per-test cap, default 60 s)
# The ctest result is a baseline check, not "all green":
#   EXPECT=run     must complete with failures <= MAX_FAILURES (else FAIL)
#   EXPECT=crash|timeout   must NOT complete (crashed or timeout both count);
#                  if it completes it passes with an IMPROVED note: lower the
#                  baseline in CMakeLists.txt
# Exits 77 (SKIP) when the Wine builtins or the NLS data are missing, or the
# guest was not built (no wine-pe/wine-src, or a toolchain that cannot build it).
if(NOT TIMEOUT)
  set(TIMEOUT 60)
endif()
if(NOT EXPECT)
  set(EXPECT run)
endif()
function(w10x_skip msg)
  message("SKIP: ${msg}")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
endfunction()

if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll")
  w10x_skip("${MACHINE}/Windows/builtin/ntdll.dll missing: build Wine's PE DLLs (wine-pe/build.sh) or download the wine-pe artifact")
  return()
endif()
if(NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  w10x_skip("${MACHINE}/Windows/System32/c_1252.nls missing: the guest's ntdll needs the Windows NLS data (machine/import_windows_dlls.ps1, or the Wine prefix's system32/*.nls on Linux)")
  return()
endif()
if(NOT EXISTS "${MACHINE}/guest-wine/${NAME}.exe")
  w10x_skip("${MACHINE}/guest-wine/${NAME}.exe missing: the Wine test guests are built only with llvm-mingw and a fetched wine-pe/wine-src (wine-pe/fetch-wine.sh)")
  return()
endif()

set(_machine "${MACHINE}")
if(EMULATOR AND _machine MATCHES "^/")
  set(_machine "Z:${_machine}")   # Wine: Z: is the Unix root
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
file(REMOVE "${MACHINE}/registry-overlay.bin")   # a test starts from the registry of the hives (persistence)
set(_cmd ${EMULATOR} "${HOST}" --machine "${_machine}" "C:\\guest-wine\\${NAME}.exe")
set(_cmake_timeout ${TIMEOUT})
# Kill the whole run on timeout (cmake's own TIMEOUT only signals the launcher,
# which can leave the Wine process behind).
if(CMAKE_HOST_UNIX)
  find_program(_timeout_tool timeout)
  if(_timeout_tool)
    set(_cmd ${_timeout_tool} -k 5 ${TIMEOUT} ${_cmd})
    math(EXPR _cmake_timeout "${TIMEOUT} + 15")   # the tool fires first (exit 124)
  endif()
endif()
# Relay tracing is on by default in Debug builds and would bury the test output.
if(DEBUG_SPEC)
  set(ENV{WIN10XBOX_DEBUG} "${DEBUG_SPEC}")
else()
  set(ENV{WIN10XBOX_DEBUG} "-all")
endif()

# Output goes to a file: some tests print millions of lines before the timeout.
if(RESULTS_DIR)
  file(MAKE_DIRECTORY "${RESULTS_DIR}")
  set(_log "${RESULTS_DIR}/${NAME}.log")
else()
  set(_log "${MACHINE}/guest-wine/${NAME}.log")
endif()
string(TIMESTAMP _t0 "%s")
set(_stdin "")
if(CMAKE_HOST_UNIX)
  set(_stdin INPUT_FILE /dev/null)
endif()
execute_process(COMMAND ${_cmd} RESULT_VARIABLE rc OUTPUT_FILE "${_log}" ERROR_VARIABLE err
  TIMEOUT ${_cmake_timeout} ${_stdin})
string(TIMESTAMP _t1 "%s")
math(EXPR secs "${_t1} - ${_t0}")

# Only the head and tail of the log matter: the summary is the last line.
file(SIZE "${_log}" _size)
set(_tail_len 65536)
if(_size GREATER _tail_len)
  math(EXPR _off "${_size} - ${_tail_len}")
else()
  set(_off 0)
endif()
file(READ "${_log}" tail OFFSET ${_off} LIMIT ${_tail_len})
set(_re "([A-Za-z0-9_]+):[^ \n]* ([0-9]+) tests executed \\(([0-9]+) marked as todo, ([0-9]+) as flaky, ([0-9]+) failures?\\), ([0-9]+) skipped")
set(executed 0)
set(todo 0)
set(flaky 0)
set(failures 0)
set(skipped 0)
if(rc STREQUAL "124" OR rc STREQUAL "137" OR rc MATCHES "timeout")
  set(status timeout)
elseif(tail MATCHES "${_re}")
  set(status ok)
  set(executed ${CMAKE_MATCH_2})
  set(todo ${CMAKE_MATCH_3})
  set(flaky ${CMAKE_MATCH_4})
  set(failures ${CMAKE_MATCH_5})
  set(skipped ${CMAKE_MATCH_6})
else()
  set(status crashed)
endif()

# Last "file.c:NNN:" line the test printed: where a crash or hang was.
set(lastseen "")
string(REPLACE ";" "," _ltail "${tail}")
string(REPLACE "[" "(" _ltail "${_ltail}")
string(REPLACE "]" ")" _ltail "${_ltail}")
string(REGEX MATCHALL "[A-Za-z0-9_]+\\.c:[0-9]+:[^\n]*" _lines "${_ltail}")
if(_lines)
  list(GET _lines -1 lastseen)
  string(SUBSTRING "${lastseen}" 0 160 lastseen)
endif()
if(status STREQUAL "ok")
  set(lastseen "")
endif()
# Layer start-up failures are reported on stderr by win10xbox itself.
set(layer_err "")
if(err MATCHES "win10xbox: [^\n]*")
  set(layer_err "${CMAKE_MATCH_0}")
endif()

if(_size LESS 6000)
  file(READ "${_log}" _show)
else()
  string(LENGTH "${tail}" _tl)
  math(EXPR _from "${_tl} - 3000")
  string(SUBSTRING "${tail}" ${_from} 3000 _show)
  set(_show "[... ${_size} bytes of output, last 3000 shown]\n${_show}")
endif()
message("${_show}")
if(err)
  string(SUBSTRING "${err}" 0 2000 _err_show)
  message("--- stderr ---\n${_err_show}")
endif()

set(_line "${NAME}\t${status}\t${executed}\t${todo}\t${flaky}\t${failures}\t${skipped}\t${rc}\t${secs}\t${lastseen}")
if(RESULTS_DIR)
  file(WRITE "${RESULTS_DIR}/${NAME}.tsv" "${_line}\n")
endif()
message("WINE_RESULT ${NAME}: status=${status} executed=${executed} todo=${todo} flaky=${flaky} failures=${failures} "
        "skipped=${skipped} exit=${rc} time=${secs}s baseline=${EXPECT}/max_failures=${MAX_FAILURES}")
if(lastseen AND NOT status STREQUAL "ok")
  message("last test line before it stopped: ${lastseen}")
endif()
if(layer_err)
  message("${layer_err}")
endif()

set(_fail "")
if(EXPECT STREQUAL "run")
  if(NOT status STREQUAL "ok")
    set(_fail "REGRESSION: baseline is 'completes', now ${status}")
  elseif(executed LESS 1)
    set(_fail "REGRESSION: no assertion executed")
  elseif(failures GREATER MAX_FAILURES)
    set(_fail "REGRESSION: ${failures} failures, baseline allows ${MAX_FAILURES}")
  elseif(failures LESS MAX_FAILURES)
    message("IMPROVED: ${failures} failures < baseline ${MAX_FAILURES}; lower it in tests/wine-tests/CMakeLists.txt")
  endif()
else()
  if(status STREQUAL "ok")
    message("IMPROVED: now completes (${executed} executed, ${failures} failures) but the baseline is '${EXPECT}'; "
            "update tests/wine-tests/CMakeLists.txt")
  else()
    message("XFAIL: ${NAME} ${status} as recorded in the baseline")
  endif()
endif()
if(_fail)
  message(FATAL_ERROR "${NAME}: ${_fail}")
endif()
message("PASS (baseline holds): ${NAME}")
