# Registry hive tooling. Despite the directory name (reserved by the D13 plan
# for building hivex) hivex is not used: see README.md in this directory.
#
#   w10x_mkfixture            writes the synthetic test hives
#   regen-registry-fixtures   runs it into tests/fixtures/registry/machine
add_executable(w10x_mkfixture
  ${CMAKE_CURRENT_LIST_DIR}/mkfixture.cpp
  ${CMAKE_SOURCE_DIR}/core/registry/regf.cpp)
target_include_directories(w10x_mkfixture PRIVATE ${CMAKE_SOURCE_DIR}/core/registry)
if(NOT MSVC)
  target_link_options(w10x_mkfixture PRIVATE -municode)   # wmain under mingw
endif()
add_custom_target(regen-registry-fixtures
  COMMAND w10x_mkfixture ${CMAKE_SOURCE_DIR}/tests/fixtures/registry/machine
  DEPENDS w10x_mkfixture
  COMMENT "Regenerating tests/fixtures/registry/machine")
