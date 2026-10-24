# guest_d3dsample, always Release (../d3dsample_release.cmake; docs/slices/d3dsample.md). _ds comes from the parent.
include(${CMAKE_CURRENT_LIST_DIR}/../d3dsample_release.cmake)
add_executable(guest_d3dsample WIN32 ${_ds}/Tutorial07.cpp ${_ds}/DDSTextureLoader.cpp ${_ds}/Tutorial07.rc)
target_include_directories(guest_d3dsample PRIVATE ${_ds})
target_compile_definitions(guest_d3dsample PRIVATE UNICODE _UNICODE)
target_compile_options(guest_d3dsample PRIVATE /W3)   # third-party code: not held to the project's /W4
target_link_libraries(guest_d3dsample PRIVATE d3d11 d3dcompiler dxguid comctl32 user32 gdi32)
win10xbox_add_guest(guest_d3dsample)
add_custom_command(TARGET guest_d3dsample POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ds}/Tutorial07.fxh ${_ds}/seafloor.dds ${WIN10XBOX_GUEST_DIR}
  COMMENT "Staging guest_d3dsample's shader and texture" VERBATIM)
