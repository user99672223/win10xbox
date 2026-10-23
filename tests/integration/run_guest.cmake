# Runs one guest program through host-win32 and checks its output and exit
# code (cmake -P; ctest's PASS_REGULAR_EXPRESSION alone ignores exit codes).
#   -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> -DPROGRAM=<C:\guest\x.exe>
#   -DEXPECT_EXIT=<n> -DEXPECT_REGEX=<regex;regex...> [-DEMULATOR=<wine>]
#   [-DPROGRAM_ARGS=<arg;arg...>]   the guest's own command-line arguments, one list element each ("\;" inside one)
#   [-DFORBID_REGEX=<regex>] [-DDEBUG_SPEC=<WIN10XBOX_DEBUG spec>]
# EXPECT_REGEX/FORBID_REGEX match the guest's stdout. The layer's own trace
# output goes to stderr, so assertions about it use the TRACE variants:
#   [-DEXPECT_TRACE_REGEX=<regex;regex...>] [-DFORBID_TRACE_REGEX=<regex>]
#   [-DREQUIRE=<path relative to the machine folder>]: SKIP (77) when that file is absent, e.g. the native
#   Windows DLLs a test needs (machine/import_windows_dlls.ps1 stages them on a Windows host only).
# ('@SEMI@' in PROGRAM_ARGS stands for a ';' inside one argument.)
# -DINPUT_FILE=<host file> is its stdin;
# -DGUEST_FILE=<path under the machine folder, '/' separated> is deleted before the run and must exist after it.
# -DREQUIRE_BUILTIN_DLLS=ON adds +modules to WIN10XBOX_DEBUG and fails unless every system DLL the guest
# mapped (C:\Windows\System32\<x>.dll in the modules trace) came from <machine>\Windows\builtin, with
# user32.dll among them (unless -DNO_USER32=ON: a console program that never loads it); the list is printed either way.
# user32.dll among them (unless -DREQUIRE_USER32=OFF: a console program); the list is printed either way.
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
string(REPLACE "@SEMI@" "\\;" _args "${PROGRAM_ARGS}")   # '@SEMI@' stands for a ';' inside one argument
if(GUEST_FILE)
  file(REMOVE "${MACHINE}/${GUEST_FILE}")
endif()
set(_stdin "")
if(INPUT_FILE)
  set(_stdin INPUT_FILE "${INPUT_FILE}")
endif()
if(DEBUG_SPEC)
  set(ENV{WIN10XBOX_DEBUG} "${DEBUG_SPEC}")
endif()
if(REQUIRE_BUILTIN_DLLS)
  if("$ENV{WIN10XBOX_DEBUG}" STREQUAL "")
    set(ENV{WIN10XBOX_DEBUG} "+modules")
  else()
    set(ENV{WIN10XBOX_DEBUG} "$ENV{WIN10XBOX_DEBUG},+modules")
  endif()
endif()
# ${_args} is expanded right here, not through an intermediate list variable: set() joins the expanded elements
# without their "\;" escape, so an argument that holds a ';' would be split in two.
execute_process(COMMAND ${EMULATOR} "${HOST}" --machine "${_machine}" "${_program}" ${_args} ${_stdin}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
# Before the guest's output: ctest keeps only the first 1024 bytes of a passing test's output.
set(_builtin_fail "")
if(REQUIRE_BUILTIN_DLLS)
  # trace:modules:Xl_NtCreateSection section <h>: C:\Windows\System32\x.dll (<n> bytes, host path <path>)
  string(REGEX MATCHALL "C:\\\\Windows\\\\System32\\\\[^ \\\\]+ \\([0-9]+ bytes, host path [^)\n]+\\)" _maps "${err}")
  set(_builtin "")
  set(_native "")
  foreach(m ${_maps})
    string(REGEX REPLACE "^C:\\\\Windows\\\\System32\\\\([^ ]+) .*" "\\1" _dll "${m}")
    if(m MATCHES "host path [^)]*\\\\Windows\\\\builtin\\\\[^\\\\)]+\\)$")
      list(APPEND _builtin "${_dll}")
    else()
      list(APPEND _native "${_dll}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _builtin)
  string(REPLACE ";" " " _b "${_builtin}")
  message("guest system DLLs from Windows\\builtin: ${_b}")
  if(_native)
    list(REMOVE_DUPLICATES _native)
    string(REPLACE ";" " " _n "${_native}")
    string(APPEND _builtin_fail "guest mapped system DLLs from outside Windows\\builtin: ${_n}\n")
  endif()
  # A console guest never loads user32: skipped for -DNO_USER32=ON (the caller's _guest_no_user32) and for
  # -DREQUIRE_USER32=OFF (the caller's _guest_console); both spellings are in use.
  if(NOT NO_USER32 AND (NOT DEFINED REQUIRE_USER32 OR REQUIRE_USER32))
    string(TOLOWER "${_builtin}" _builtin_lc)
    list(FIND _builtin_lc "user32.dll" _u32)
    if(_u32 LESS 0)
      string(APPEND _builtin_fail "no builtin user32.dll in the modules trace (is +modules on?)\n")
    endif()
  endif()
endif()
string(REPLACE "\r\n" "\n" out "${out}")   # text-mode stdout of a Windows console program
message("${out}")
if(err)
  message("--- stderr ---\n${err}")
endif()
set(_fail "${_builtin_fail}")
if(NOT "${rc}" STREQUAL "${EXPECT_EXIT}")
  string(APPEND _fail "exit code ${rc}, expected ${EXPECT_EXIT}\n")
  # host-win32 (MSVC) writes crash.txt next to win10xbox.exe on a fatal exception: show it, a later test
  # run would overwrite it before the CI artifact upload.
  get_filename_component(_host_dir "${HOST}" DIRECTORY)
  if(EXISTS "${_host_dir}/crash.txt")
    file(TIMESTAMP "${_host_dir}/crash.txt" _crash_time "%s")
    string(TIMESTAMP _now "%s")
    math(EXPR _age "${_now} - ${_crash_time}")
    if(_age LESS 120)
      file(READ "${_host_dir}/crash.txt" _crash)
      message("--- crash.txt ---\n${_crash}")
    endif()
  endif()
endif()
foreach(re ${EXPECT_REGEX})
  if(NOT out MATCHES "${re}")
    string(APPEND _fail "stdout does not match '${re}'\n")
  endif()
endforeach()
if(GUEST_FILE AND NOT EXISTS "${MACHINE}/${GUEST_FILE}")
  string(APPEND _fail "${MACHINE}/${GUEST_FILE} was not created\n")
endif()
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
