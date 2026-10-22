# core/user: the win32k surface (NtUser* / NtGdi*) behind Wine's PE user32 and
# gdi32, emulated in-process (docs/GUI.md). Included from core/CMakeLists.txt.
#
# Window surfaces and input come from the winexbox display driver; only its
# INTERFACE header is used here (winexbox/include/winexbox/driver.h has no
# <windows.h> and no link dependency), the host hands the driver object in
# through user::SetDisplayDriver. The winexbox library itself links against
# win10xbox_core, so this module must not link back to it.
target_sources(win10xbox_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/user.cpp
  ${CMAKE_CURRENT_LIST_DIR}/gdi_objects.cpp
  ${CMAKE_CURRENT_LIST_DIR}/gdi_dc.cpp
  ${CMAKE_CURRENT_LIST_DIR}/gdi_draw.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_class.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_window.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_defwnd.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_message.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_paint.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_sysparams.cpp
  ${CMAKE_CURRENT_LIST_DIR}/win_clipboard.cpp
)
target_include_directories(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(win10xbox_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../winexbox/include)
target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_USER=1)
