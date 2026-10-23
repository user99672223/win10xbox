# integration.guest_wasapi / integration.guest_xaudio2 (cmake -P): the layer must agree with the host.
#   -DHOST=<win10xbox.exe> -DMACHINE=<builtin-only machine dir> -DGUEST_EXE=<built guest .exe>
#   -DPROBE_EXE=<built guest_wasapi.exe> -DPROGRAM=<C:\guest\guest_x.exe> -DTAG=<wasapi|xaudio2> [-DEMULATOR=<wine>]
#
# The guests exit 42 when a device played the sound, 43 when the host has no audio endpoint (they print
# "<tag>: RESULT no-endpoint hr=0x..."), 1 on any other failure. A runner may or may not have an endpoint, so the gate
# does not assume either:
#   1. run the guest directly on the host: its verdict (42 or 43) is what the host's audio stack really answers;
#   2. run it through the layer on the builtin-only machine folder (Wine's mmdevapi, xaudio2_9, the registry's
#      MMDeviceEnumerator class, the host's WASAPI behind core/dispatch/unix_mmdevapi.cpp);
#   3. the layer must give the same exit code; with an endpoint the whole sequence (the guest prints one line per
#      step and "RESULT played") must have run, without one the layer must answer with the same HRESULT as the host.
# A layer that registered a made-up device, or that fails to reach the host's, differs from the host's answer here.
# Prints "SKIP: ..." (exit 77) under the same conditions as run_guest.cmake.
if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll")
  message("SKIP: ${MACHINE}/Windows/builtin/ntdll.dll missing (Wine PE builtins)")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
if(NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  message("SKIP: ${MACHINE}/Windows/System32/c_1252.nls missing (Windows NLS data)")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()
if(NOT EXISTS "${MACHINE}/Windows/builtin/mmdevapi.dll" OR NOT EXISTS "${MACHINE}/Windows/builtin/xaudio2_9.dll")
  message(FATAL_ERROR "the machine folder has no builtin mmdevapi.dll / xaudio2_9.dll (wine-pe/dlls.txt builds them)")
endif()

# 1. directly on the host. The verdict is the one of guest_wasapi (PROBE_EXE, WASAPI's own answer): both guests ask the
#    same host stack, and a host Wine without a device can crash inside its own xaudio2 (FAudio dereferences a missing
#    device), which says nothing about the layer. The guest under test is also run directly, for the log; it decides
#    the verdict itself only when it is the probe.
execute_process(COMMAND ${EMULATOR} "${PROBE_EXE}" RESULT_VARIABLE rc_host OUTPUT_VARIABLE out_host
  ERROR_VARIABLE err_host TIMEOUT 120)
string(REPLACE "\r\n" "\n" out_host "${out_host}")
message("=== host verdict, guest_wasapi run directly on the host (exit ${rc_host}) ===\n${out_host}")
if(NOT rc_host STREQUAL "42" AND NOT rc_host STREQUAL "43")
  message("--- host stderr ---\n${err_host}")
  message(FATAL_ERROR "guest_wasapi run directly on the host exited ${rc_host}: neither 42 (played) nor 43 (no endpoint)")
endif()
if(NOT "${GUEST_EXE}" STREQUAL "${PROBE_EXE}")
  execute_process(COMMAND ${EMULATOR} "${GUEST_EXE}" RESULT_VARIABLE rc_own OUTPUT_VARIABLE out_own
    ERROR_VARIABLE err_own TIMEOUT 120)
  string(REPLACE "\r\n" "\n" out_own "${out_own}")
  string(SUBSTRING "${err_own}" 0 1500 err_own)
  message("=== ${TAG} run directly on the host, for the log only (exit ${rc_own}) ===\n${out_own}${err_own}")
endif()

# 2. through the layer
set(_machine "${MACHINE}")
if(EMULATOR AND _machine MATCHES "^/")
  set(_machine "Z:${_machine}")
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
set(ENV{WIN10XBOX_DEBUG} "+audio")
execute_process(COMMAND ${EMULATOR} "${HOST}" --machine "${_machine}" "${PROGRAM}"
  RESULT_VARIABLE rc_layer OUTPUT_VARIABLE out_layer ERROR_VARIABLE err_layer TIMEOUT 120)
string(REPLACE "\r\n" "\n" out_layer "${out_layer}")
message("=== ${TAG}: through the layer (exit ${rc_layer}) ===\n${out_layer}")
if(err_layer)
  string(REGEX MATCHALL "[^\n]*(audio|mmdevapi|wasapi|xaudio|FIXME|fixme|err:)[^\n]*" _lines "${err_layer}")
  list(LENGTH _lines _n)
  if(_n GREATER 60)
    list(SUBLIST _lines 0 60 _lines)
  endif()
  string(REPLACE ";" "\n" _lines "${_lines}")
  message("--- layer stderr (audio-related lines) ---\n${_lines}")
endif()

set(_fail "")
if(NOT rc_layer STREQUAL rc_host)
  string(APPEND _fail "the layer exited ${rc_layer}, the host exited ${rc_host}\n")
  get_filename_component(_host_dir "${HOST}" DIRECTORY)
  if(EXISTS "${_host_dir}/crash.txt")
    file(READ "${_host_dir}/crash.txt" _crash)
    message("--- crash.txt ---\n${_crash}")
  endif()
endif()
if(rc_host STREQUAL "43")
  # The host has no endpoint: the layer must report exactly the host's HRESULT, not another error and not a device.
  if(NOT out_layer MATCHES "${TAG}: RESULT no-endpoint hr=0x[0-9a-f]+")
    string(APPEND _fail "the layer printed no '${TAG}: RESULT no-endpoint' line\n")
  elseif("${TAG}" STREQUAL "wasapi")
    # WASAPI's own answer is exact: the same HRESULT as the host's (E_NOTFOUND, 0x80070490, on a machine without one).
    if(out_host MATCHES "wasapi: RESULT no-endpoint hr=(0x[0-9a-f]+)")
      set(_hr_host "${CMAKE_MATCH_1}")
      if(NOT out_layer MATCHES "wasapi: RESULT no-endpoint hr=${_hr_host}")
        string(APPEND _fail "the host answers hr=${_hr_host}, the layer printed a different no-endpoint HRESULT\n")
      endif()
    endif()
  endif()
else()
  foreach(_re "${TAG}: RESULT played" "${TAG}: done")
    if(NOT out_layer MATCHES "${_re}")
      string(APPEND _fail "the layer never printed '${_re}'\n")
    endif()
  endforeach()
  if(out_layer MATCHES "${TAG}: FAILED[^\n]*")
    string(APPEND _fail "${CMAKE_MATCH_0}\n")
  endif()
endif()
if(_fail)
  message(FATAL_ERROR "${TAG}:\n${_fail}")
endif()
if(rc_host STREQUAL "43")
  message("PASS: the host has no audio endpoint and the layer answers exactly as the host does")
else()
  message("PASS: the host plays the sound and so does the layer")
endif()
