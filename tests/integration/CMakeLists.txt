# Integration tests: real EXEs run through host-win32. M1 adds guest programs.
add_test(NAME integration.host_hello COMMAND win10xbox)
set_tests_properties(integration.host_hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from host-win32")
add_test(NAME integration.host_version COMMAND win10xbox --version)
set_tests_properties(integration.host_version PROPERTIES PASS_REGULAR_EXPRESSION "^win10xbox [0-9]")
