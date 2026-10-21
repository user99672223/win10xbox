# drivers: the in-process driver host (D16). A user-mode ntoskrnl/hal export
# surface, the DRIVER_OBJECT/DEVICE_OBJECT/IRP engine and object manager, and a
# minimal KMDF shim. Test drivers and tests live in tests/drivers.
#
#   win10xbox_drivers   static library; links win10xbox_core (loader, dispatcher, trace, clock)
#   include/ntk/ntk.h   kernel-mode types for drivers built against the host
#   include/win10xbox/drivers.h   public API (see drivers/README.md)
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
add_library(win10xbox_drivers STATIC ${WIN10XBOX_DRIVERS_SOURCES})
target_include_directories(win10xbox_drivers
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ntoskrnl)
target_link_libraries(win10xbox_drivers PUBLIC win10xbox_core)

# Import library a driver links against: `import ntoskrnl.exe!Name`.
include(${CMAKE_CURRENT_LIST_DIR}/import_lib.cmake)

if(WIN10XBOX_BUILD_TESTS)
  add_subdirectory(${CMAKE_SOURCE_DIR}/tests/drivers ${CMAKE_BINARY_DIR}/tests/drivers)
endif()
