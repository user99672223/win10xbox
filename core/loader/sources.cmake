# Loader module: PE mapper, syscall-stub rewriter and code scanner. Pure C++20
# (thunk machine code is emitted at runtime, so no MASM/GAS split is needed).
target_sources(win10xbox_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/internal.cpp
  ${CMAKE_CURRENT_LIST_DIR}/mapper.cpp
  ${CMAKE_CURRENT_LIST_DIR}/stubs.cpp
  ${CMAKE_CURRENT_LIST_DIR}/scanner.cpp
)
target_include_directories(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR})
