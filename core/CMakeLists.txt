# core: the compatibility layer as a static library. Pure Win32/NT C++20,
# ZERO UWP/WinRT dependencies. Links into host-win32 and host-uwp.
#
# Each module lives in core/<module>/ and registers its sources through a
# core/<module>/sources.cmake file (target_sources(win10xbox_core ...)), so
# modules can be developed independently without touching this file.
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
target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_CORE_BUILD=1)

file(GLOB WIN10XBOX_CORE_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/sources.cmake)
foreach(_m ${WIN10XBOX_CORE_MODULES})
  include(${_m})
endforeach()
