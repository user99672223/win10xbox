# stage.cmake - assemble the AppX layout that package.cmd packs (cmake -P).
# Run by the stage_appx target of host-uwp-lite/CMakeLists.txt:
#
#   cmake -DSTAGE=<dir> -DEXE=<win10xbox-uwp-lite.exe> -DGUEST_GUI=<smoke.exe>
#         -DGUEST_CUI=<smoke_cui.exe> -DWINMD=<Win10XboxLite.winmd or empty>
#         -DMANIFEST=<AppxManifest.xml> -DASSETS_DIR=<dir with *.png>
#         -DBUILTIN_DIR=<wine-pe/out/builtin> -DMACHINE_JSON=<machine.default.json>
#         -P stage.cmake
#
# Layout (see the header of host-uwp-lite/CMakeLists.txt):
#   AppxManifest.xml  win10xbox-uwp-lite.exe  Win10XboxLite.winmd  Assets\*.png
#   payload\machine.json  payload\guest\*.exe  payload\builtin\*.dll (+ LICENSE.wine, MANIFEST.txt)
# The app copies payload\ into LocalState\machine at first run (smoke.cpp).
foreach(_v STAGE EXE GUEST_GUI GUEST_CUI MANIFEST ASSETS_DIR)
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

stage_copy("${EXE}" "${STAGE}/win10xbox-uwp-lite.exe")
stage_copy("${MANIFEST}" "${STAGE}/AppxManifest.xml")

if(NOT "${WINMD}" STREQUAL "")
  stage_copy("${WINMD}" "${STAGE}/Win10XboxLite.winmd")
else()
  message(WARNING "stage.cmake: no WinRT metadata (UWP_LITE_SKIP_WINMD): the Xbox launcher is expected to refuse this package")
endif()

file(GLOB _pngs "${ASSETS_DIR}/*.png")
if(NOT _pngs)
  message(FATAL_ERROR "stage.cmake: no *.png in ${ASSETS_DIR}")
endif()
foreach(_f IN LISTS _pngs)
  get_filename_component(_n "${_f}" NAME)
  file(COPY_FILE "${_f}" "${STAGE}/Assets/${_n}")
endforeach()

foreach(_g "${GUEST_GUI}" "${GUEST_CUI}")
  get_filename_component(_n "${_g}" NAME)
  stage_copy("${_g}" "${STAGE}/payload/guest/${_n}")
endforeach()

if(NOT "${MACHINE_JSON}" STREQUAL "" AND EXISTS "${MACHINE_JSON}")
  file(COPY_FILE "${MACHINE_JSON}" "${STAGE}/payload/machine.json")
endif()

# Wine PE builtins (LGPL-2.1+; the wine-pe-builtin CI artifact). Optional.
set(_nb 0)
if(NOT "${BUILTIN_DIR}" STREQUAL "" AND IS_DIRECTORY "${BUILTIN_DIR}")
  file(GLOB _dlls "${BUILTIN_DIR}/*.dll")
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

if(_nb EQUAL 0)
  message(WARNING "stage.cmake: no Wine PE builtin DLLs in '${BUILTIN_DIR}'. The package is still produced; on the "
                  "console, copy ntdll.dll, kernelbase.dll, kernel32.dll (and friends) into "
                  "LocalState\\machine\\Windows\\builtin, or rebuild with the wine-pe-builtin artifact.")
else()
  message(STATUS "stage.cmake: bundled ${_nb} Wine PE builtin DLLs from ${BUILTIN_DIR}")
endif()
message(STATUS "stage.cmake: AppX layout ready in ${STAGE}")
