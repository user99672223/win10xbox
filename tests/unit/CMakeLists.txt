# Unit tests for core/. Minimal self-contained harness (tests/unit/check.h);
# no external test framework so the same sources build with MSVC and llvm-mingw.
add_executable(test_core
  main.cpp
  test_trace.cpp
)
target_link_libraries(test_core PRIVATE win10xbox_core)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit.core COMMAND test_core)
