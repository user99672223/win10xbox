# core/registry: virtual registry over REGF hives + in-memory overlay.
# Own REGF reader/writer (regf.cpp); hivex is NOT used, see machine/hivex/README.md.
target_sources(win10xbox_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/regf.cpp
  ${CMAKE_CURRENT_LIST_DIR}/registry.cpp
)
# Fixture-hive generator (machine/hivex/mkfixture.cpp), built from the same regf.cpp.
if(NOT TARGET w10x_mkfixture)
  add_subdirectory(${CMAKE_SOURCE_DIR}/machine/hivex ${CMAKE_BINARY_DIR}/machine/hivex)
endif()
