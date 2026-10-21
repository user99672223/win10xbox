# win10xbox_import_lib(<out-var> <name> <def-file> <dll-name>)
# Builds an import library from a .def file: llvm-mingw dlltool or MSVC lib.exe.
function(win10xbox_import_lib out_target name def dllname)
  set(_lib ${CMAKE_BINARY_DIR}/lib/drivers/${name}.lib)
  if(MSVC)
    add_custom_command(OUTPUT ${_lib}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/lib/drivers
      COMMAND ${CMAKE_AR} /nologo /DEF:${def} /NAME:${dllname} /MACHINE:X64 /OUT:${_lib}
      DEPENDS ${def} VERBATIM)
  else()
    get_filename_component(_cdir ${CMAKE_C_COMPILER} DIRECTORY)
    find_program(WIN10XBOX_DLLTOOL NAMES x86_64-w64-mingw32-dlltool dlltool HINTS ${_cdir})
    if(NOT WIN10XBOX_DLLTOOL)
      message(FATAL_ERROR "dlltool not found next to ${CMAKE_C_COMPILER}")
    endif()
    set(_lib ${CMAKE_BINARY_DIR}/lib/drivers/lib${name}.a)
    add_custom_command(OUTPUT ${_lib}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/lib/drivers
      COMMAND ${WIN10XBOX_DLLTOOL} -d ${def} -l ${_lib} -D ${dllname}
      DEPENDS ${def} VERBATIM)
  endif()
  add_custom_target(${name}_implib DEPENDS ${_lib})
  set(${out_target} ${_lib} PARENT_SCOPE)
endfunction()
