# xinput1_4.dll, also staged under the names xinput1_3.dll and xinput9_1_0.dll (the same code: every game imports
# one of the three). Output: ${WIN10XBOX_GUEST_DLL_DIR}/xinput1_4.dll plus the two copies made after the link.
add_library(xinput1_4 SHARED xinput.c xinput.def)
target_include_directories(xinput1_4 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../core/include)
set_target_properties(xinput1_4 PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY ${WIN10XBOX_GUEST_DLL_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${WIN10XBOX_GUEST_DLL_DIR}
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/guest-dlls)
add_custom_command(TARGET xinput1_4 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xinput1_4> ${WIN10XBOX_GUEST_DLL_DIR}/xinput1_3.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:xinput1_4> ${WIN10XBOX_GUEST_DLL_DIR}/xinput9_1_0.dll
  VERBATIM)
if(NOT MSVC)
  # llvm-mingw: no C runtime start-up in the DLL (it needs none, and the guest loader should run as little as
  # possible of it); DllMain is the entry point itself. MSVC's /MT start-up is the same as tests/guest/dll.c's.
  target_link_options(xinput1_4 PRIVATE -nostartfiles -Wl,-e,DllMain)
endif()
