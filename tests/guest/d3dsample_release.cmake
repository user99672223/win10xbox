# Included by the d3dsample subdirectories (tests/guest/d3dsample, host-uwp/d3dsample): the sample is a fixture, like the other
# fetched programs, so it is built in Release whatever the layer's configuration is (games ship release-compiled shaders; the
# Debug build of Tutorial07 compiles its shader with D3DCOMPILE_DEBUG, a different code path: docs/slices/crypto-rsaaes.md).
# Directory-scoped on purpose: only the targets of the including directory see these.
foreach(_c DEBUG RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_C_FLAGS_${_c} "${CMAKE_C_FLAGS_RELEASE}")
  set(CMAKE_CXX_FLAGS_${_c} "${CMAKE_CXX_FLAGS_RELEASE}")
  set(CMAKE_EXE_LINKER_FLAGS_${_c} "${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
  set(CMAKE_RC_FLAGS_${_c} "${CMAKE_RC_FLAGS_RELEASE}")
endforeach()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
