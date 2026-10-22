# tests: unit (core), guest test programs (real PEs run through the layer),
# integration (real EXEs through host-win32 on CI).
add_subdirectory(guest)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(host_exit)    # how win10xbox.exe ends: the exit code survives anything the DLL detach does
add_subdirectory(wine-tests)   # Wine conformance subset (label wine_conformance)
