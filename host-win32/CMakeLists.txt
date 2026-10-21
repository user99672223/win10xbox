# host-win32: console host that runs core/ unsandboxed on Windows. This is the
# CI test vehicle and the desktop development host.
add_executable(win10xbox main.cpp)
target_link_libraries(win10xbox PRIVATE win10xbox_core)
if(NOT MSVC)
  target_link_options(win10xbox PRIVATE -municode)   # wmain entry under mingw
endif()
