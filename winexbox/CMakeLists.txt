# winexbox: display/input backend interface for Wine's win32u. M3.
#
# Interface (include/winexbox/driver.h) + headless backend. Independent of UWP
# and of Wine headers; depends on win10xbox_core only for the trace channels
# and config::DisplayBackend. The real D3D12/CoreWindow backend and the
# win32u binding (winexbox.drv) are deferred (docs/DECISIONS.md D8, README.md).
add_library(winexbox STATIC
  src/headless.cpp
  src/registry_stub.cpp
)
target_include_directories(winexbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(winexbox PUBLIC win10xbox_core)

# tests/unit globs test_*.cpp into test_core, but cannot know about this
# library, and tests/ is added after this directory. Link it once the whole
# tree is configured, and register a ctest entry that runs only our cases
# (test names start with "winexbox_"; main.cpp filters by substring).
if(WIN10XBOX_BUILD_TESTS)
  function(winexbox_attach_unit_tests)
    if(TARGET test_core)
      target_link_libraries(test_core PRIVATE winexbox)
      add_test(NAME unit.winexbox COMMAND test_core winexbox_)
    endif()
  endfunction()
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL winexbox_attach_unit_tests)
endif()
