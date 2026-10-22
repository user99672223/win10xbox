# Real program: curl's curl.exe (built by tools/ci/build_curl.ps1 into <machine>/guest) through win10xbox.
#   cmake -DHOST=<win10xbox.exe> -DMACHINE=<machine dir> [-DEMULATOR=<wine>] -P realcurl.cmake
# Through the layer:
#   curl --version                            the banner, the protocol and feature lists (no TLS: http, file, ...)
#   curl -sS file:///C:/workcurl/in/small.txt the file:// handler: open/read of a guest path, written to stdout
#   curl -sS -o C:\workcurl\out\big.bin file:///C:/workcurl/in/big.bin   a 512 KiB copy, compared by SHA-256
#   curl -sS -w ... file:///.../missing.txt   a missing file must fail with curl's exit 37 (FILE_COULDNT_READ_FILE)
# HTTP against a host-side server is tests/integration/realcurl_http.py (integration.real_curl_http).
# WIN10XBOX_DEBUG is "-all" unless REALCURL_DEBUG is set.
if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll" OR NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  message(FATAL_ERROR "realcurl: machine folder ${MACHINE} has no Wine builtins / NLS data")
endif()
if(NOT EXISTS "${MACHINE}/guest/curl.exe")
  message(FATAL_ERROR "realcurl: ${MACHINE}/guest/curl.exe missing (tools/ci/build_curl.ps1)")
endif()

set(_machine "${MACHINE}")
if(EMULATOR AND _machine MATCHES "^/")
  set(_machine "Z:${_machine}")
endif()
string(REPLACE "/" "\\" _machine "${_machine}")
if(DEFINED ENV{REALCURL_DEBUG})
  set(ENV{WIN10XBOX_DEBUG} "$ENV{REALCURL_DEBUG}")
else()
  set(ENV{WIN10XBOX_DEBUG} "-all")
endif()

# ---- the file set --------------------------------------------------------------------------------------------
set(_work "${MACHINE}/workcurl")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}/in" "${_work}/out")
file(WRITE "${_work}/in/small.txt" "hello from a guest file, read by curl\nsecond line\n")
set(_chunks "")
foreach(i RANGE 0 31)
  string(RANDOM LENGTH 1024 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789 \n" RANDOM_SEED ${i} _c)
  set(_chunk${i} "${_c}")
endforeach()
set(_seed 4242)
file(WRITE "${_work}/in/big.bin" "")
foreach(k RANGE 1 512)
  math(EXPR _seed "(${_seed} * 1103515245 + 12345) % 2147483648")
  math(EXPR _idx "(${_seed} / 65536) % 32")
  file(APPEND "${_work}/in/big.bin" "${_chunk${_idx}}")
endforeach()
file(SIZE "${_work}/in/big.bin" _big_size)
file(SHA256 "${_work}/in/big.bin" _big_sha)
message("realcurl: big.bin is ${_big_size} bytes, sha256 ${_big_sha}")

# ---- running curl --------------------------------------------------------------------------------------------
function(runcurl label expect_rc expect_regex)
  execute_process(COMMAND ${EMULATOR} "${HOST}" --machine "${_machine}" "C:\\guest\\curl.exe" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
  message("=== curl ${ARGN} -> exit ${rc}\n${out}")
  if(err)
    string(LENGTH "${err}" _el)
    if(_el GREATER 4000)
      string(SUBSTRING "${err}" 0 4000 err)
    endif()
    message("--- stderr ---\n${err}")
  endif()
  if(NOT "${rc}" STREQUAL "${expect_rc}")
    message(SEND_ERROR "${label}: exit code ${rc}, expected ${expect_rc}")
  endif()
  foreach(re ${expect_regex})
    if(NOT out MATCHES "${re}")
      message(SEND_ERROR "${label}: stdout does not match '${re}'")
    endif()
  endforeach()
  set(_last_out "${out}" PARENT_SCOPE)
endfunction()

runcurl("version" 0 "^curl 8\\.[0-9]+\\.[0-9]+ \\(;libcurl/8\\.;Protocols:[^\n]* file[ \n];Protocols:[^\n]* http[ \n];Features:" --version)
if(_last_out MATCHES "Protocols:[^\n]* https[ \n]")
  message(SEND_ERROR "version: https listed, but the build has no TLS backend")
endif()

runcurl("file small" 0 "^hello from a guest file, read by curl\nsecond line\n$" -sS "file:///C:/workcurl/in/small.txt")
runcurl("file big -o" 0 "^size=${_big_size}$" -sS -o "C:\\workcurl\\out\\big.bin" -w "size=%{size_download}"
  "file:///C:/workcurl/in/big.bin")
if(NOT EXISTS "${_work}/out/big.bin")
  message(SEND_ERROR "file big -o: C:\\workcurl\\out\\big.bin was not written")
else()
  file(SHA256 "${_work}/out/big.bin" _got)
  if(NOT "${_got}" STREQUAL "${_big_sha}")
    file(SIZE "${_work}/out/big.bin" _got_size)
    message(SEND_ERROR "file big -o: SHA-256 ${_got} (${_got_size} bytes), expected ${_big_sha} (${_big_size} bytes)")
  else()
    message("realcurl: out/big.bin sha256 ok")
  endif()
endif()
# CURLE_FILE_COULDNT_READ_FILE.
runcurl("file missing" 37 "" -sS "file:///C:/workcurl/in/missing.txt")

message("realcurl: done")
