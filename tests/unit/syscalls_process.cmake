# test_syscalls_process.cpp starts bin/guest/guest_hello.exe as the "host" of a spawned child: build it first.
if(TARGET guest_hello)
  add_dependencies(test_core guest_hello)
endif()
