# integration.guest_audio_probe (cmake -P): the audio probe's answers through the layer must be the HOST's.
#   -DHOST=<win10xbox.exe> -DMACHINE=<builtin-only machine dir> -DPROBE=<guest_audio_probe.exe>
#   -DSTUB_DIR=<dir with the bridge stubs mmdevapi.dll / xaudio2_9.dll> [-DEMULATOR=<wine>]
#
# 1. Run the probe directly on the host: what the host's audio stack really answers (a headless runner has no
#    device: E_NOTFOUND / AUDCLNT_E_DEVICE_INVALIDATED style codes, an honest "no device").
# 2. Stage the stubs in <machine>\Windows\builtin and run the probe through the layer (guest ntdll/combase are
#    Wine's; mmdevapi/xaudio2 reach the host through core/audio).
# 3. Every step must be present in both runs with the same verdict AND the same HRESULT. A layer-side fake
#    (class not registered, DLL missing, a made-up success) differs from the host's answer and fails here.
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

set(_steps co_init
  wasapi.enumerator wasapi.default_endpoint wasapi.activate wasapi.mix_format wasapi.initialize
  wasapi.render_client wasapi.write
  xaudio2.load xaudio2.create xaudio2.mastering xaudio2.source xaudio2.play)

foreach(dll mmdevapi.dll xaudio2_9.dll)
  if(NOT EXISTS "${STUB_DIR}/${dll}")
    message(FATAL_ERROR "bridge stub ${STUB_DIR}/${dll} was not built")
  endif()
  file(COPY_FILE "${STUB_DIR}/${dll}" "${MACHINE}/Windows/builtin/${dll}" ONLY_IF_DIFFERENT)
endforeach()

# 1. direct
execute_process(COMMAND ${EMULATOR} "${PROBE}" RESULT_VARIABLE rc_native OUTPUT_VARIABLE out_native
  ERROR_VARIABLE err_native TIMEOUT 90)
message("=== direct (host) ===\n${out_native}")
if(NOT "${rc_native}" STREQUAL "0")
  message(FATAL_ERROR "the probe run directly exited ${rc_native}\n${err_native}")
endif()

# 2. through the layer
set(_machine "${MACHINE}")
if(EMULATOR AND _machine MATCHES "^/")
  set(_machine "Z:${_machine}")
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
set(ENV{WIN10XBOX_DEBUG} "+audio")
execute_process(COMMAND ${EMULATOR} "${HOST}" --machine "${_machine}" "C:\\guest\\guest_audio_probe.exe"
  RESULT_VARIABLE rc_layer OUTPUT_VARIABLE out_layer ERROR_VARIABLE err_layer TIMEOUT 90)
message("=== through the layer ===\n${out_layer}")
if(err_layer)
  message("--- layer stderr ---\n${err_layer}")
endif()

set(_fail "")
if(NOT "${rc_layer}" STREQUAL "0")
  string(APPEND _fail "the probe through the layer exited ${rc_layer}, expected 0\n")
  get_filename_component(_host_dir "${HOST}" DIRECTORY)
  if(EXISTS "${_host_dir}/crash.txt")
    file(READ "${_host_dir}/crash.txt" _crash)
    message("--- crash.txt ---\n${_crash}")
  endif()
endif()

# 3. compare
function(w10x_verdict out text step)
  string(REPLACE "." "\\." _re "${step}")
  if(text MATCHES "audio: ${_re} (PASS|FAIL) hr=(0x[0-9a-f]+)")
    set(${out} "${CMAKE_MATCH_1} ${CMAKE_MATCH_2}" PARENT_SCOPE)
  else()
    set(${out} "<no line>" PARENT_SCOPE)
  endif()
endfunction()
set(_summary "")
foreach(s ${_steps})
  w10x_verdict(_n "${out_native}" "${s}")
  w10x_verdict(_l "${out_layer}" "${s}")
  string(APPEND _summary "  ${s}: host=${_n} layer=${_l}\n")
  if(_n STREQUAL "<no line>")
    string(APPEND _fail "step ${s}: the direct run printed no result line\n")
  elseif(NOT _n STREQUAL _l)
    string(APPEND _fail "step ${s}: layer answered '${_l}', the host answers '${_n}'\n")
  endif()
endforeach()
if(NOT out_layer MATCHES "audio: done")
  string(APPEND _fail "the layer run never reached 'audio: done'\n")
endif()
message("=== per step ===\n${_summary}")
if(_fail)
  message(FATAL_ERROR "audio probe:\n${_fail}")
endif()
message("PASS: every audio step through the layer matches the host's answer")
