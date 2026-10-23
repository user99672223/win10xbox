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
#   <MACHINE>\Windows\System32\*.nls            NLS data (c_*.nls, l_intl, locale, norm*): import_windows_dlls.ps1
#   <MACHINE>\Windows\Globalization\Sorting\    sortdefault.nls (same script)
#   <MACHINE>\Windows\System32\config\         registry hives (optional)
#   <MACHINE>\Windows\Temp\
#   <MACHINE>\Users\User\AppData\{Local\Temp,Roaming}\
#   <MACHINE>\ProgramData\  <MACHINE>\Program Files\  <MACHINE>\Program Files (x86)\
#   <MACHINE>\guest\*                          test programs from <bin>/guest
# Existing files are refreshed only when the source is newer.
if(NOT MACHINE)
  message(FATAL_ERROR "prepare_machine: MACHINE not set")
endif()

foreach(d Windows/builtin Windows/System32 Windows/System32/config Windows/Globalization/Sorting Windows/Temp
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
  file(GLOB _files "${GUEST_DIR}/*.exe" "${GUEST_DIR}/*.dll" "${GUEST_DIR}/*.lua")
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
# Wine's ntdll locale init needs the Windows NLS data files (emulated
# NtGetNlsSectionPtr reads them from the machine folder). They are Microsoft
# data, never committed: on Windows CI machine/import_windows_dlls.ps1 stages
# them; elsewhere run_guest.cmake SKIPs the guest tests (same check).
if(NOT EXISTS "${MACHINE}/Windows/System32/c_1252.nls")
  message(STATUS "prepare_machine: no NLS data (${MACHINE}/Windows/System32/c_1252.nls; "
                 "run machine/import_windows_dlls.ps1 on Windows); guest tests will be skipped")
endif()

# -DBUILTIN_ONLY_MACHINE=<dir>: a second machine folder with the console's DLL set, used by the GUI tests.
# It holds the same builtins, guest programs and NLS data as MACHINE but NO native DLLs, and its machine.json
# says "*": "builtin,native" -- what host-uwp stages on the console (Wine builtins + NLS only). The shared
# folder cannot serve the GUI guests on Windows CI: import_windows_dlls.ps1 fills its System32 with the runner's
# own DLLs (build 26100) and machine.default.json prefers native, so Wine's builtin user32/gdi32 pulled in
# native advapi32/sechost/rpcrt4, which import ntdll exports Wine 11.9 lacks and resolve the CRT apiset to
# kernelbase (always Wine's): the guest died on Wine's 0x80000100 import stub. Run at test time (the
# integration.prepare_machine fixture), after CI has staged the NLS data into MACHINE.
if(BUILTIN_ONLY_MACHINE)
  foreach(d Windows/builtin Windows/System32 Windows/System32/config Windows/Globalization/Sorting Windows/Temp
            Users/User/AppData/Local/Temp Users/User/AppData/Roaming Users/Public
            ProgramData "Program Files" "Program Files (x86)" guest)
    file(MAKE_DIRECTORY "${BUILTIN_ONLY_MACHINE}/${d}")
  endforeach()
  file(GLOB _files "${MACHINE}/Windows/builtin/*" "${MACHINE}/guest/*" "${MACHINE}/Windows/System32/*.nls"
                   "${MACHINE}/Windows/Globalization/Sorting/*.nls")
  set(_copied 0)
  foreach(f ${_files})
    file(RELATIVE_PATH _rel "${MACHINE}" "${f}")
    if(NOT IS_DIRECTORY "${f}")
      file(COPY_FILE "${f}" "${BUILTIN_ONLY_MACHINE}/${_rel}" ONLY_IF_DIFFERENT)
      math(EXPR _copied "${_copied} + 1")
    endif()
  endforeach()
  # Programs with a directory of their own under guest/ (MinGit: guest/mingit, a git.exe with its DLLs beside it).
  file(GLOB_RECURSE _files "${MACHINE}/guest/*/*")
  foreach(f ${_files})
    file(RELATIVE_PATH _rel "${MACHINE}" "${f}")
    get_filename_component(_dir "${_rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${BUILTIN_ONLY_MACHINE}/${_dir}")
    file(COPY_FILE "${f}" "${BUILTIN_ONLY_MACHINE}/${_rel}" ONLY_IF_DIFFERENT)
    math(EXPR _copied "${_copied} + 1")
  endforeach()
  if(DEFAULT_JSON AND EXISTS "${DEFAULT_JSON}")
    file(READ "${DEFAULT_JSON}" _json)
    string(REGEX REPLACE "\"\\*\"[ \t]*:[ \t]*\"[^\"]*\"" "\"*\": \"builtin,native\"" _json "${_json}")
    if(NOT _json MATCHES "\"\\*\": \"builtin,native\"")
      message(FATAL_ERROR "prepare_machine: no \"*\" entry in ${DEFAULT_JSON}'s dll_overrides to rewrite")
    endif()
    file(CONFIGURE OUTPUT "${BUILTIN_ONLY_MACHINE}/machine.json" CONTENT "${_json}" @ONLY)
  endif()
  message(STATUS "prepare_machine: ${BUILTIN_ONLY_MACHINE}: builtin-only machine, ${_copied} files")
endif()
