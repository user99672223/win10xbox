# host-win32: console host that runs core/ unsandboxed on Windows. This is the
# CI test vehicle and the desktop development host.
add_executable(win10xbox main.cpp)
target_link_libraries(win10xbox PRIVATE win10xbox_core)
if(MSVC)
  # Crash capture (crash.txt / crash.dmp / entry-reached.txt, see crash.h and the comment at the top of crash.cpp) and
  # a linker map. MSVC only: crash.cpp needs __try and delayimp.lib's __HrLoadAllImportsForDll, and the llvm-mingw
  # build of this directory (ci.yml linux-cross) has to keep building without it (main.cpp compiles the capture out
  # unless WIN10XBOX_HOST_CRASH_CAPTURE is defined).
  #
  # KNOWN DEBT, on purpose: crash.cpp / crash.h are copies of host-uwp-lite/crash.{cpp,h} (host-uwp has a third
  # copy). All three were byte-identical (sha256) until this copy's crash.cpp gained host-module attribution; every
  # change is tagged "[host-win32]" and described at the top of the file, crash.h is still byte-identical. Folding
  # the copies into one shared library is a separate decision, not made here.
  target_sources(win10xbox PRIVATE crash.cpp crash.h)
  target_compile_definitions(win10xbox PRIVATE WIN10XBOX_HOST_CRASH_CAPTURE=1)
  target_link_libraries(win10xbox PRIVATE
    dbghelp           # MiniDumpWriteDump for crash.dmp (crash.cpp) ...
    delayimp)         # ... delay-loaded, see the link options below
  # /DELAYLOAD:dbghelp.dll: crash.cpp binds it at startup (__HrLoadAllImportsForDll) and treats a failure as "no
  # crash.dmp". Unlike the console's AppContainer a desktop runner has dbghelp.dll, so crash.dmp is produced there.
  #
  # /MAP: the linker map, next to the executable that host-win32-smoke.yml runs and uploads: build/bin/win10xbox.map.
  # Its Rva+Base column is relative to the "Preferred load address" in its header; a crash address minus the load base
  # that win10xbox.exe prints at startup ("host-win32: host-image base=...") is the RVA, preferred + RVA is the map
  # address. Written by the same link that writes the exe, so they always belong together (the map's "Timestamp is"
  # equals the host-image line's timestamp=).
  target_link_options(win10xbox PRIVATE
    /DELAYLOAD:dbghelp.dll
    "/MAP:$<TARGET_FILE_DIR:win10xbox>/$<TARGET_FILE_BASE_NAME:win10xbox>.map"
    /MAPINFO:EXPORTS)
else()
  target_link_options(win10xbox PRIVATE -municode)   # wmain entry under mingw
endif()
