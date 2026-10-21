# Third-party dependencies, built from the pinned submodules. Each keeps its
# own license file in its directory. Wine and ReactOS are not submodules
# (size); see third_party/PINS.md for the pinned tags fetched by scripts.

# --- Zydis (MIT): x86 decoder used by the code scanner and the VEH emulator.
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYAN_FORCE_ASSERTS OFF CACHE BOOL "" FORCE)
add_subdirectory(zydis EXCLUDE_FROM_ALL)

# --- MinHook (BSD-2): inline hooking / trampolines (alternative to Detours).
add_library(minhook STATIC
  minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c
  minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC minhook/include)
if(NOT MSVC)
  target_compile_options(minhook PRIVATE -w)
endif()

# --- Microsoft Detours (MIT): built from source, x64 only.
add_library(detours STATIC
  Detours/src/detours.cpp Detours/src/modules.cpp Detours/src/disasm.cpp
  Detours/src/image.cpp Detours/src/creatwth.cpp)
target_include_directories(detours PUBLIC Detours/src)
target_compile_definitions(detours PRIVATE DETOURS_X64 DETOURS_64BIT _AMD64_)
if(MSVC)
  target_compile_options(detours PRIVATE /W0)
else()
  target_compile_options(detours PRIVATE -w)
endif()

# --- MemoryModule (MPL-2.0): starting point for the manual PE mapper. The
# loader in core/ is derived from it; the original is kept buildable as a
# reference library for differential tests.
add_library(memorymodule STATIC MemoryModule/MemoryModule.c)
target_include_directories(memorymodule PUBLIC MemoryModule)
if(NOT MSVC)
  target_compile_options(memorymodule PRIVATE -w)
endif()

# --- phnt (MIT): NT headers. Header-only.
add_library(phnt INTERFACE)
target_include_directories(phnt INTERFACE phnt)

# --- hivex (LGPL-2.1): REGF hive reader/writer. Built by machine/ (needs a
# small shim layer for gnulib pieces); see machine/hivex/CMakeLists.txt.
