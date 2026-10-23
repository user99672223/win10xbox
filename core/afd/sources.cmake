# core/afd: Wine's \Device\Afd ioctls translated onto host Winsock sockets (docs/DECISIONS.md, N1).
target_sources(win10xbox_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/afd.cpp)
target_link_libraries(win10xbox_core PUBLIC ws2_32)
target_compile_definitions(win10xbox_core PRIVATE WIN10XBOX_HAVE_AFD=1)
