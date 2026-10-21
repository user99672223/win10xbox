# core: the compatibility layer as a static library. Pure Win32/NT C++20,
# ZERO UWP/WinRT dependencies. Links into host-win32 and host-uwp.
execute_process(COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE WIN10XBOX_GIT_REV
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

add_library(win10xbox_core STATIC
  src/core.cpp
  src/trace.cpp
)
target_include_directories(win10xbox_core PUBLIC include)
target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_GIT_REV="${WIN10XBOX_GIT_REV}")
if(WIN10XBOX_TRACE_DEFAULT_ON)
  target_compile_definitions(win10xbox_core PUBLIC WIN10XBOX_TRACE_DEFAULT_ON=1)
endif()
target_link_libraries(win10xbox_core PUBLIC phnt Zydis minhook detours)
# No UWP/WinRT headers may be reachable from here.
target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_CORE_BUILD=1)
