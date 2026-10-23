# stage.cmake - assemble the AppX layout that package.cmd packs (cmake -P).
# Run by the stage_appx target of host-uwp/CMakeLists.txt:
#
#   cmake -DSTAGE=<dir> -DEXE=<win10xbox-uwp.exe> -DGUESTS=<guest1.exe,guest2.exe,...>
#         -DWINMD=<Win10XboxUwp.winmd or empty>
#         -DMANIFEST=<AppxManifest.xml> -DASSETS_DIR=<dir with *.png>
#         -DPAYLOAD_DIR=<host-uwp/payload> -DBUILTIN_DIR=<wine-pe/out/builtin>
#         -DMACHINE_JSON=<machine.default.json> [-DGUEST_DLL_DIR=<bin/guest-dlls>: our own builtin DLLs]
#         -P stage.cmake
#
# Layout (see the header of host-uwp/CMakeLists.txt):
#   AppxManifest.xml  win10xbox-uwp.exe  Win10XboxUwp.winmd  Assets\*.png
#   payload\machine.json  payload\launch.json  payload\launch.example.json
#   payload\guest\* (every file of GUESTS: window.exe, hello.exe, ...)
#   payload\nls\*.nls (Wine NLS data, -DNLS_DIR=<wine-pe/out/nls>)
#   payload\builtin\*.dll (+ LICENSE.wine, MANIFEST.txt)
# At run time host_stage.cpp (StageMachineFiles) copies payload\builtin and payload\guest into
# LocalState\machine, StageMachineJson copies payload\machine.json (never over an existing one),
# host.cpp reads payload\launch.json as the packaged default launch configuration and
# host_shell.cpp (PublishExampleConfig) copies payload\launch.example.json to LocalState.
foreach(_v STAGE EXE GUESTS MANIFEST ASSETS_DIR)
  if("${${_v}}" STREQUAL "")
    message(FATAL_ERROR "stage.cmake: -D${_v}= is required")
  endif()
endforeach()

function(stage_copy src dst)
  if(NOT EXISTS "${src}")
    message(FATAL_ERROR "stage.cmake: missing input file ${src}")
  endif()
  file(COPY_FILE "${src}" "${dst}")
endfunction()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}/Assets" "${STAGE}/payload/guest" "${STAGE}/payload/builtin")

stage_copy("${EXE}" "${STAGE}/win10xbox-uwp.exe")
stage_copy("${MANIFEST}" "${STAGE}/AppxManifest.xml")

if(NOT "${WINMD}" STREQUAL "")
  stage_copy("${WINMD}" "${STAGE}/Win10XboxUwp.winmd")
else()
  message(WARNING "stage.cmake: no WinRT metadata (HOST_UWP_SKIP_WINMD): the Xbox launcher is expected to refuse this package")
endif()

file(GLOB _pngs "${ASSETS_DIR}/*.png")
if(NOT _pngs)
  message(FATAL_ERROR "stage.cmake: no *.png in ${ASSETS_DIR}")
endif()
foreach(_f IN LISTS _pngs)
  get_filename_component(_n "${_f}" NAME)
  file(COPY_FILE "${_f}" "${STAGE}/Assets/${_n}")
endforeach()

# GUESTS is comma-separated (CMakeLists.txt: a ";" list does not survive the command line).
string(REPLACE "," ";" _guests "${GUESTS}")
set(_ng 0)
foreach(_g IN LISTS _guests)
  get_filename_component(_n "${_g}" NAME)
  if(EXISTS "${STAGE}/payload/guest/${_n}")
    message(FATAL_ERROR "stage.cmake: two guests are both named ${_n}")
  endif()
  stage_copy("${_g}" "${STAGE}/payload/guest/${_n}")
  math(EXPR _ng "${_ng} + 1")
endforeach()
message(STATUS "stage.cmake: bundled ${_ng} guest program(s)")
# Manifest listing of the bundled guests (one file name per line), checked by host-uwp.yml.
set(_list "")
foreach(_g IN LISTS _guests)
  get_filename_component(_n "${_g}" NAME)
  string(APPEND _list "${_n}\n")
endforeach()
file(WRITE "${STAGE}/payload/guests.txt" "${_list}")

# Files that are not built here: the sidecars (.args/.env/.title/.launch, host-uwp/payload/guest/) and, when
# EXTRA_GUEST_DIR is given, everything in it (Chocolate Doom, its DLLs and the WAD). Not listed in guests.txt.
set(_extra "")
if(EXISTS "${PAYLOAD_DIR}/guest")
  file(GLOB _side "${PAYLOAD_DIR}/guest/*")
  list(APPEND _extra ${_side})
endif()
if(NOT "${EXTRA_GUEST_DIR}" STREQUAL "")
  if(NOT IS_DIRECTORY "${EXTRA_GUEST_DIR}")
    message(FATAL_ERROR "stage.cmake: EXTRA_GUEST_DIR ${EXTRA_GUEST_DIR} is not a directory")
  endif()
  file(GLOB _more "${EXTRA_GUEST_DIR}/*")
  list(APPEND _extra ${_more})
endif()
foreach(_f IN LISTS _extra)
  if(NOT IS_DIRECTORY "${_f}")
    get_filename_component(_n "${_f}" NAME)
    if(EXISTS "${STAGE}/payload/guest/${_n}")
      message(FATAL_ERROR "stage.cmake: extra guest file ${_n} collides with a bundled guest")
    endif()
    file(COPY_FILE "${_f}" "${STAGE}/payload/guest/${_n}")
  endif()
endforeach()
list(LENGTH _extra _nx)
message(STATUS "stage.cmake: ${_nx} extra guest file(s) (sidecars, fetched programs)")

if(NOT "${MACHINE_JSON}" STREQUAL "" AND EXISTS "${MACHINE_JSON}")
  file(COPY_FILE "${MACHINE_JSON}" "${STAGE}/payload/machine.json")
endif()

# Static payload files from the source tree: launch.json (the packaged default launch
# configuration) and launch.example.json (every key documented, published to LocalState by
# the shell). Optional: without launch.json the host falls back to its built-in defaults.
if(NOT "${PAYLOAD_DIR}" STREQUAL "" AND IS_DIRECTORY "${PAYLOAD_DIR}")
  file(GLOB _payload_json "${PAYLOAD_DIR}/*.json")
  foreach(_f IN LISTS _payload_json)
    get_filename_component(_n "${_f}" NAME)
    file(COPY_FILE "${_f}" "${STAGE}/payload/${_n}")
  endforeach()
endif()

# Wine PE builtins (LGPL-2.1+; the wine-pe-builtin CI artifact). Optional.
set(_nb 0)
if(NOT "${BUILTIN_DIR}" STREQUAL "" AND IS_DIRECTORY "${BUILTIN_DIR}")
  file(GLOB _dlls "${BUILTIN_DIR}/*.dll" "${BUILTIN_DIR}/*.drv")
  foreach(_f IN LISTS _dlls)
    get_filename_component(_n "${_f}" NAME)
    file(COPY_FILE "${_f}" "${STAGE}/payload/builtin/${_n}")
    math(EXPR _nb "${_nb} + 1")
  endforeach()
  foreach(_extra LICENSE.wine MANIFEST.txt)
    if(EXISTS "${BUILTIN_DIR}/${_extra}")
      file(COPY_FILE "${BUILTIN_DIR}/${_extra}" "${STAGE}/payload/builtin/${_extra}")
    endif()
  endforeach()
endif()

# Our own builtin DLLs (guest-dlls/: xinput1_4.dll and its copies), next to Wine's. Counted apart: a package with only
# these still cannot run a guest (the warning below is about Wine's).
set(_nw ${_nb})
if(NOT "${GUEST_DLL_DIR}" STREQUAL "" AND IS_DIRECTORY "${GUEST_DLL_DIR}")
  file(GLOB _dlls "${GUEST_DLL_DIR}/*.dll")
  foreach(_f IN LISTS _dlls)
    get_filename_component(_n "${_f}" NAME)
    file(COPY_FILE "${_f}" "${STAGE}/payload/builtin/${_n}")
    math(EXPR _nb "${_nb} + 1")
  endforeach()
  message(STATUS "stage.cmake: bundled ${_nb} builtin DLLs in all, ${_nw} from Wine")
endif()

if(_nw EQUAL 0)
  message(WARNING "stage.cmake: no Wine PE builtin DLLs in '${BUILTIN_DIR}'. The package is still produced; on the "
                  "console, copy ntdll.dll, kernelbase.dll, kernel32.dll (and friends) into "
                  "LocalState\\machine\\Windows\\builtin, or rebuild with the wine-pe-builtin artifact.")
else()
  message(STATUS "stage.cmake: bundled ${_nw} Wine PE builtin DLLs from ${BUILTIN_DIR}")
endif()
# Wine NLS data (LGPL-2.1+; the wine-pe-nls CI artifact): payload\nls\*.nls. The host copies it into
# LocalState\machine\Windows\System32 (sortdefault.nls into ...\Globalization\Sorting) at first run, where the
# emulated NtGetNlsSectionPtr reads it. Without it the package depends on the console's own System32 (it has none).
set(_nn 0)
if(NOT "${NLS_DIR}" STREQUAL "" AND IS_DIRECTORY "${NLS_DIR}")
  file(MAKE_DIRECTORY "${STAGE}/payload/nls")
  file(GLOB _nls "${NLS_DIR}/*.nls")
  foreach(_f IN LISTS _nls)
    get_filename_component(_n "${_f}" NAME)
    file(COPY_FILE "${_f}" "${STAGE}/payload/nls/${_n}")
    math(EXPR _nn "${_nn} + 1")
  endforeach()
endif()
if(_nn EQUAL 0)
  message(WARNING "stage.cmake: no NLS files in '${NLS_DIR}'. The package is produced without payload/nls and every "
                  "guest crashes in RtlInitNlsTables on a console whose System32 lacks l_intl.nls/c_10000.nls.")
else()
  message(STATUS "stage.cmake: bundled ${_nn} NLS files from ${NLS_DIR}")
endif()

message(STATUS "stage.cmake: AppX layout ready in ${STAGE}")
