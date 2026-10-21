# Unit tests for core/. Minimal self-contained harness (tests/unit/check.h);
# no external test framework so the same sources build with MSVC and llvm-mingw.
# Every tests/unit/test_*.cpp is compiled in automatically.
file(GLOB WIN10XBOX_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(test_core main.cpp ${WIN10XBOX_UNIT_SOURCES})
target_link_libraries(test_core PRIVATE win10xbox_core)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Tests find fixtures and guest binaries relative to the build's bin dir.
target_compile_definitions(test_core PRIVATE
  WIN10XBOX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  WIN10XBOX_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME unit.core COMMAND test_core)
# Optional extra link libraries/sources registered by modules' test fragments.
file(GLOB WIN10XBOX_UNIT_FRAGMENTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cmake)
foreach(_f ${WIN10XBOX_UNIT_FRAGMENTS})
  include(${_f})
endforeach()
