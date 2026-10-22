# core/dispatch: syscall dispatcher, translators, process bootstrap.
# Included from core/CMakeLists.txt (target win10xbox_core).
#
# Subsystems are wired by name (their Nt-signature functions). Each one is
# optional so the dispatcher builds while the modules are written in
# parallel; the default follows the presence of core/<module>/sources.cmake
# and can be forced with -DWIN10XBOX_HAVE_<SUB>=OFF (VFS, REGISTRY, CLOCK, TOKEN).
# The loader is not optional once it exists (its sources are always compiled
# into win10xbox_core); loader_stubs.cpp stands in only while it is absent.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../loader/sources.cmake)
  target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_LOADER=1)
else()
  target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_LOADER=0)
  target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/loader_stubs.cpp)
  message(STATUS "core/dispatch: core/loader absent, using loader_stubs.cpp (process::Launch will fail)")
endif()

# A subsystem switched OFF keeps its own sources in the library but is not
# wired: its syscalls fall back to pass-through (see subsystems.cpp).
foreach(_sub VFS REGISTRY CLOCK TOKEN)
  string(TOLOWER ${_sub} _dir)
  if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../${_dir}/sources.cmake)
    set(_default ON)
  else()
    set(_default OFF)
  endif()
  option(WIN10XBOX_HAVE_${_sub} "core/dispatch: link against core/${_dir}" ${_default})
  if(WIN10XBOX_HAVE_${_sub})
    target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_${_sub}=1)
  else()
    target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_${_sub}=0)
    message(STATUS "core/dispatch: ${_dir} module not built (WIN10XBOX_HAVE_${_sub}=OFF)")
  endif()
endforeach()

target_sources(win10xbox_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/dispatcher.cpp
  ${CMAKE_CURRENT_LIST_DIR}/nt_file.cpp
  ${CMAKE_CURRENT_LIST_DIR}/nt_section.cpp
  ${CMAKE_CURRENT_LIST_DIR}/nt_process.cpp
  ${CMAKE_CURRENT_LIST_DIR}/nt_nls.cpp
  ${CMAKE_CURRENT_LIST_DIR}/subsystems.cpp
  ${CMAKE_CURRENT_LIST_DIR}/unix_calls.cpp
  ${CMAKE_CURRENT_LIST_DIR}/rtl_forward.cpp
  ${CMAKE_CURRENT_LIST_DIR}/cpu_sites.cpp
  ${CMAKE_CURRENT_LIST_DIR}/process.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win32k_native.cpp
)

if(MSVC)
  target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../asm/syscall_entry.asm)
else()
  target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../asm/syscall_entry.S)
endif()
