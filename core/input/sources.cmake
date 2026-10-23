# core/input: the host's gamepad state (win10xbox/gamepad.h), served to the guest's XInput.
# Included from core/CMakeLists.txt (target win10xbox_core).
target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/gamepad.cpp)
