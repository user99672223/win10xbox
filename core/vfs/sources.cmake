# VFS module: machine-folder path translation (see core/include/win10xbox/vfs.h).
target_sources(win10xbox_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/vfs.cpp
  ${CMAKE_CURRENT_LIST_DIR}/path.cpp
  ${CMAKE_CURRENT_LIST_DIR}/json.cpp
)
