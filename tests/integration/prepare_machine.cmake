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
  file(GLOB _files "${GUEST_DIR}/*.exe" "${GUEST_DIR}/*.dll" "${GUEST_DIR}/*.lua" "${GUEST_DIR}/*.ninja"
       "${GUEST_DIR}/*.py" "${GUEST_DIR}/*.json")
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
  # guest/ recursively: a guest can be a folder (guest/python is the python.org embeddable package, unpacked as shipped).
  file(GLOB_RECURSE _guest_files "${MACHINE}/guest/*")
  file(GLOB _files "${MACHINE}/Windows/builtin/*" "${MACHINE}/Windows/System32/*.nls"
                   "${MACHINE}/Windows/Globalization/Sorting/*.nls")
  list(APPEND _files ${_guest_files})
  set(_copied 0)
  foreach(f ${_files})
    file(RELATIVE_PATH _rel "${MACHINE}" "${f}")
    if(NOT IS_DIRECTORY "${f}")
      get_filename_component(_reldir "${_rel}" DIRECTORY)
      file(MAKE_DIRECTORY "${BUILTIN_ONLY_MACHINE}/${_reldir}")
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

# -DNATIVE_GUI_MACHINE=<dir>: route B's folder (docs/GUI.md "Route B: native win32k"): the builtin-only set plus
# the NATIVE GUI DLLs from MACHINE's System32 (user32, win32u, gdi32, gdi32full, msvcp_win, imm32: what the
# native user32 pulls in that must be native), machine.json "gui": "native", "*": "builtin,native" and "native"
# for those six. Everything else stays Wine's: a whole native chain (msvcrt, rpcrt4, advapi32...) imports CRT
# apisets and ntdll internals Wine's builtins do not provide. Without the native DLLs (any non-Windows host) the
# folder is still made and the test that uses it SKIPs on the missing user32.dll.
if(NATIVE_GUI_MACHINE)
  foreach(d Windows/builtin Windows/System32 Windows/System32/config Windows/Globalization/Sorting Windows/Temp
            Users/User/AppData/Local/Temp Users/User/AppData/Roaming Users/Public
            ProgramData "Program Files" "Program Files (x86)" guest)
    file(MAKE_DIRECTORY "${NATIVE_GUI_MACHINE}/${d}")
  endforeach()
  set(_gui_dlls user32.dll win32u.dll gdi32.dll gdi32full.dll msvcp_win.dll imm32.dll)
  file(GLOB _files "${MACHINE}/Windows/builtin/*" "${MACHINE}/guest/*" "${MACHINE}/Windows/System32/*.nls"
                   "${MACHINE}/Windows/Globalization/Sorting/*.nls")
  foreach(n ${_gui_dlls})
    if(EXISTS "${MACHINE}/Windows/System32/${n}")
      list(APPEND _files "${MACHINE}/Windows/System32/${n}")
    endif()
  endforeach()
  set(_copied 0)
  foreach(f ${_files})
    file(RELATIVE_PATH _rel "${MACHINE}" "${f}")
    if(NOT IS_DIRECTORY "${f}")
      file(COPY_FILE "${f}" "${NATIVE_GUI_MACHINE}/${_rel}" ONLY_IF_DIFFERENT)
      math(EXPR _copied "${_copied} + 1")
    endif()
  endforeach()
  # imm32 delay-loads msctf.dll; the stand-in (tests/guest/msctf_stub.c) goes where the loader looks for the real one.
  if(MSCTF_STUB AND EXISTS "${MSCTF_STUB}" AND EXISTS "${MACHINE}/Windows/System32/imm32.dll")
    file(COPY_FILE "${MSCTF_STUB}" "${NATIVE_GUI_MACHINE}/Windows/System32/msctf.dll" ONLY_IF_DIFFERENT)
  endif()
  if(DEFAULT_JSON AND EXISTS "${DEFAULT_JSON}")
    file(READ "${DEFAULT_JSON}" _json)
    set(_ov "\"*\": \"builtin,native\"")
    foreach(n ${_gui_dlls})
      string(REPLACE ".dll" "" _b "${n}")
      string(APPEND _ov ", \"${_b}\": \"native\"")
    endforeach()
    string(REGEX REPLACE "\"\\*\"[ \t]*:[ \t]*\"[^\"]*\"" "${_ov}" _json "${_json}")
    string(REGEX REPLACE "\"version\"[ \t]*:[ \t]*1[ \t]*," "\"version\": 1,\n  \"gui\": \"native\"," _json "${_json}")
    if(NOT _json MATCHES "\"gui\": \"native\"" OR NOT _json MATCHES "\"user32\": \"native\"")
      message(FATAL_ERROR "prepare_machine: could not derive the native-GUI machine.json from ${DEFAULT_JSON}")
    endif()
    file(CONFIGURE OUTPUT "${NATIVE_GUI_MACHINE}/machine.json" CONTENT "${_json}" @ONLY)
  endif()
  message(STATUS "prepare_machine: ${NATIVE_GUI_MACHINE}: native-GUI machine, ${_copied} files")
endif()

# -DD3D_MACHINE=<dir>: route B + Direct3D 11 (docs/DECISIONS.md G1, integration.guest_imgui_d3d11): the folder
# NATIVE_GUI_MACHINE describes (builtins, NLS, guests, the native GUI DLLs, "gui": "native") plus the NATIVE Direct3D
# runtime: d3d11/dxgi/d3dcompiler_47 run as guest code, and their D3DKMT calls (gdi32 -> the native win32u's
# NtGdiDdDDI* thunks) pass through by name to the host's win32u like every other route-B syscall, i.e. to the host's
# dxgkrnl. d3d10warp is the user-mode driver d3d11 loads for the runner's "Microsoft Basic Render Driver" (and for
# D3D_DRIVER_TYPE_WARP). Wine's d3d11/dxgi are not an option here (they are wined3d front ends over GL/Vulkan in
# Wine's unix libraries, which this project does not have).
if(D3D_MACHINE)
  foreach(d Windows/builtin Windows/System32 Windows/System32/config Windows/Globalization/Sorting Windows/Temp
            Users/User/AppData/Local/Temp Users/User/AppData/Roaming Users/Public
            ProgramData "Program Files" "Program Files (x86)" guest)
    file(MAKE_DIRECTORY "${D3D_MACHINE}/${d}")
  endforeach()
  set(_d3d_native user32.dll win32u.dll gdi32.dll gdi32full.dll msvcp_win.dll imm32.dll
                  d3d11.dll dxgi.dll d3dcompiler_47.dll d3d10warp.dll dxcore.dll dwmapi.dll
                  d3d12.dll D3D12Core.dll D3DSCache.dll
                  powrprof.dll umpdc.dll kernel.appcore.dll)   # api-ms-win-power-setting-* / appmodel-runtime-* (dxgi, d3d11)
  file(GLOB _files "${MACHINE}/Windows/builtin/*" "${MACHINE}/guest/*" "${MACHINE}/Windows/System32/*.nls"
                   "${MACHINE}/Windows/Globalization/Sorting/*.nls")
  foreach(n ${_d3d_native})
    if(EXISTS "${MACHINE}/Windows/System32/${n}")
      list(APPEND _files "${MACHINE}/Windows/System32/${n}")
    endif()
  endforeach()
  set(_copied 0)
  foreach(f ${_files})
    file(RELATIVE_PATH _rel "${MACHINE}" "${f}")
    if(NOT IS_DIRECTORY "${f}")
      file(COPY_FILE "${f}" "${D3D_MACHINE}/${_rel}" ONLY_IF_DIFFERENT)
      math(EXPR _copied "${_copied} + 1")
    endif()
  endforeach()
  if(DEFAULT_JSON AND EXISTS "${DEFAULT_JSON}")
    file(READ "${DEFAULT_JSON}" _json)
    set(_ov "\"*\": \"builtin,native\"")
    foreach(n ${_d3d_native})
      string(REPLACE ".dll" "" _b "${n}")
      string(APPEND _ov ", \"${_b}\": \"native\"")
    endforeach()
    string(REGEX REPLACE "\"\\*\"[ \t]*:[ \t]*\"[^\"]*\"" "${_ov}" _json "${_json}")
    string(REGEX REPLACE "\"version\"[ \t]*:[ \t]*1[ \t]*," "\"version\": 1,\n  \"gui\": \"native\"," _json "${_json}")
    if(NOT _json MATCHES "\"gui\": \"native\"" OR NOT _json MATCHES "\"d3d11\": \"native\"")
      message(FATAL_ERROR "prepare_machine: could not derive the D3D machine.json from ${DEFAULT_JSON}")
    endif()
    file(CONFIGURE OUTPUT "${D3D_MACHINE}/machine.json" CONTENT "${_json}" @ONLY)
  endif()
  message(STATUS "prepare_machine: ${D3D_MACHINE}: route B + Direct3D 11 machine, ${_copied} files")
endif()
