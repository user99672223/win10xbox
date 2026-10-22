# Runs one guest program through host-win32 and checks its output and exit
# code (cmake -P; ctest's PASS_REGULAR_EXPRESSION alone ignores exit codes).
#   -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> -DPROGRAM=<C:\guest\x.exe>
#   -DEXPECT_EXIT=<n> -DEXPECT_REGEX=<regex;regex...> [-DEMULATOR=<wine>]
#   [-DFORBID_REGEX=<regex>] [-DDEBUG_SPEC=<WIN10XBOX_DEBUG spec>]
# EXPECT_REGEX/FORBID_REGEX match the guest's stdout. The layer's own trace
# output goes to stderr, so assertions about it use the TRACE variants:
#   [-DEXPECT_TRACE_REGEX=<regex;regex...>] [-DFORBID_TRACE_REGEX=<regex>]
#   [-DREQUIRE=<path relative to the machine folder>]: SKIP (77) when that file is absent, e.g. the native
#   Windows DLLs a test needs (machine/import_windows_dlls.ps1 stages them on a Windows host only).
# Prints "SKIP: ..." and exits 77 (cmake >= 3.29) when the machine folder has
# no Wine builtins or no Windows NLS data (c_1252.nls, staged by
# machine/import_windows_dlls.ps1); the test sets both SKIP_RETURN_CODE 77 and
# SKIP_REGULAR_EXPRESSION "SKIP:".
if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll")
  message("SKIP: ${MACHINE}/Windows/builtin/ntdll.dll missing: build Wine's PE DLLs (wine-pe/build.sh) "
          "or download the wine-pe artifact into the machine folder")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
if(NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  message("SKIP: ${MACHINE}/Windows/System32/c_1252.nls missing: the guest's ntdll needs the Windows NLS data "
          "(machine/import_windows_dlls.ps1 stages it from a Windows install; not available on Linux)")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
if(REQUIRE AND NOT EXISTS "${MACHINE}/${REQUIRE}")
  message("SKIP: ${MACHINE}/${REQUIRE} missing: this test needs the native Windows DLLs in the machine folder "
          "(machine/import_windows_dlls.ps1 stages them from a Windows install; not available on Linux)")
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
foreach(re ${EXPECT_TRACE_REGEX})
  if(NOT err MATCHES "${re}")
    string(APPEND _fail "trace output does not match '${re}'\n")
  endif()
endforeach()
if(FORBID_TRACE_REGEX AND err MATCHES "${FORBID_TRACE_REGEX}")
  string(APPEND _fail "trace output matches forbidden '${FORBID_TRACE_REGEX}'\n")
endif()
# WIN10XBOX_EXPECT_SCREENSHOT_PIXELS="x,y,aarrggbb|x,y,aarrggbb": the host wrote WIN10XBOX_SCREENSHOT (a 32-bit
# top-down BMP of the virtual screen, see core/user); check pixels of it.
if(DEFINED ENV{WIN10XBOX_SCREENSHOT} AND DEFINED ENV{WIN10XBOX_EXPECT_SCREENSHOT_PIXELS})
  set(_bmp "$ENV{WIN10XBOX_SCREENSHOT}")
  if(NOT EXISTS "${_bmp}")
    string(APPEND _fail "screenshot ${_bmp} was not written\n")
  else()
    file(READ "${_bmp}" _hex HEX)
    # BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40); width at byte 18, height at 22 (negative = top-down).
    string(SUBSTRING "${_hex}" 36 8 _w_le)
    string(REGEX REPLACE "(..)(..)(..)(..)" "\\4\\3\\2\\1" _w_be "${_w_le}")
    math(EXPR _w "0x${_w_be}")
    string(REPLACE "|" ";" _pixels "$ENV{WIN10XBOX_EXPECT_SCREENSHOT_PIXELS}")
  foreach(_px ${_pixels})
      string(REPLACE "," ";" _p "${_px}")
      list(GET _p 0 _x)
      list(GET _p 1 _y)
      list(GET _p 2 _want)
      math(EXPR _off "(54 + (${_y} * ${_w} + ${_x}) * 4) * 2")
      string(SUBSTRING "${_hex}" ${_off} 8 _le)
      string(REGEX REPLACE "(..)(..)(..)(..)" "\\4\\3\\2\\1" _got "${_le}")
      if(NOT "${_got}" STREQUAL "${_want}")
        string(APPEND _fail "screenshot pixel (${_x},${_y}) is ${_got}, expected ${_want} (${_bmp})\n")
      endif()
    endforeach()
  endif()
endif()
if(_fail)
  message(FATAL_ERROR "${PROGRAM}:\n${_fail}")
endif()
message("PASS: ${PROGRAM} (exit ${rc})")
