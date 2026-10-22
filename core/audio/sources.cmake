# core/audio: bridge from the guest to the host's audio stack (WASAPI / XAudio2). Included from core/CMakeLists.txt.
# The guest-side stubs (core/audio/guest/*.c) are built as guest PEs by tests/guest/CMakeLists.txt.
target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/audio.cpp)
target_include_directories(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR})
