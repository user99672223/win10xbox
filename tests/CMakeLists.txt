# tests: unit (core), guest test programs (real PEs run through the layer),
# integration (real EXEs through host-win32 on CI).
add_subdirectory(guest)
add_subdirectory(unit)
add_subdirectory(integration)
