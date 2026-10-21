# test_loader.cpp maps bin/guest/guest_dll.dll: make sure it is built first.
if(TARGET guest_dll)
  add_dependencies(test_core guest_dll)
endif()
