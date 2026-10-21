# Prepares the machine folder used by the integration tests (cmake -P).
#   cmake -DMACHINE=<bin>/machine -DBUILTIN_DIR=<wine-pe/out/builtin>
#         -DGUEST_DIR=<bin>/guest -DDEFAULT_JSON=<machine/machine.default.json>
#         -P prepare_machine.cmake
#
# Layout (the same one CI builds with machine/import_windows_dlls.ps1 plus the
# downloaded wine-pe artifact):
#   <MACHINE>\machine.json                     copied from DEFAULT_JSON if missing
#   <MACHINE>\Windows\builtin\*.dll|*.exe      Wine PE builtins (wine-pe/out/builtin)
#   <MACHINE>\Windows\System32\                native DLLs (CI: import_windows_dlls.ps1)
#   <MACHINE>\Windows\System32\config\         registry hives (optional)
#   <MACHINE>\Windows\Temp\
#   <MACHINE>\Users\User\AppData\{Local\Temp,Roaming}\
#   <MACHINE>\ProgramData\  <MACHINE>\Program Files\  <MACHINE>\Program Files (x86)\
#   <MACHINE>\guest\*                          test programs from <bin>/guest
# Existing files are refreshed only when the source is newer.
if(NOT MACHINE)
  message(FATAL_ERROR "prepare_machine: MACHINE not set")
endif()

foreach(d Windows/builtin Windows/System32 Windows/System32/config Windows/Temp
          Users/User/AppData/Local/Temp Users/User/AppData/Roaming Users/Public
          ProgramData "Program Files" "Program Files (x86)" guest)
  file(MAKE_DIRECTORY "${MACHINE}/${d}")
endforeach()

if(DEFAULT_JSON AND EXISTS "${DEFAULT_JSON}" AND NOT EXISTS "${MACHINE}/machine.json")
  file(COPY_FILE "${DEFAULT_JSON}" "${MACHINE}/machine.json")
endif()

set(_builtins 0)
if(BUILTIN_DIR AND IS_DIRECTORY "${BUILTIN_DIR}")
  file(GLOB _files "${BUILTIN_DIR}/*.dll" "${BUILTIN_DIR}/*.exe")
  foreach(f ${_files})
    get_filename_component(n "${f}" NAME)
    file(COPY_FILE "${f}" "${MACHINE}/Windows/builtin/${n}" ONLY_IF_DIFFERENT)
    math(EXPR _builtins "${_builtins} + 1")
  endforeach()
endif()

set(_guests 0)
if(GUEST_DIR AND IS_DIRECTORY "${GUEST_DIR}")
  file(GLOB _files "${GUEST_DIR}/*.exe" "${GUEST_DIR}/*.dll")
  foreach(f ${_files})
    get_filename_component(n "${f}" NAME)
    file(COPY_FILE "${f}" "${MACHINE}/guest/${n}" ONLY_IF_DIFFERENT)
    math(EXPR _guests "${_guests} + 1")
  endforeach()
endif()

if(NOT EXISTS "${MACHINE}/Windows/builtin/ntdll.dll")
  message(STATUS "prepare_machine: no Wine builtins in ${BUILTIN_DIR} (run wine-pe/build.sh); guest tests will be skipped")
else()
  message(STATUS "prepare_machine: ${MACHINE}: ${_builtins} builtins, ${_guests} guest files")
endif()
