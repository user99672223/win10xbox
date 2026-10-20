# probe-core: the shared static library. Built once per host (win32 / uwp) so
# that the CRT flavor, WINAPI_FAMILY and AppContainer flags match the host that
# links it. The parent project sets those; this file just lists the sources and
# the invariant compile options (notably /EHa so seh.cpp's __try/__except and C++
# try/catch coexist).
cmake_minimum_required(VERSION 3.20)

add_library(probe_core STATIC
    reporter.cpp
    seh.cpp
    env.cpp
    http.cpp
    core_run.cpp
    probe_A_dynamic_code.cpp
    probe_B_dll_inventory.cpp
    probe_B_ntdll_stubs.cpp
    probe_C_syscalls.cpp
    probe_D_veh_trap.cpp
    probe_E_cpu_identity.cpp
    probe_F_filesystem.cpp
    probe_G_registry.cpp
    probe_H_token.cpp
    probe_I_graphics.cpp
    probe_J_audio.cpp
    probe_K_network.cpp
    probe_L_processes.cpp
    probe_M_system.cpp
    probe_N_winrt.cpp
)

target_include_directories(probe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(probe_core PUBLIC cxx_std_20)

# /EHa: asynchronous (SEH-aware) exception model, required by seh.cpp.
# Keep each probe independent: no fast-math or other surprises.
target_compile_options(probe_core PRIVATE /EHa /W3 /permissive-)

# No link libraries here on purpose. The win32 and uwp hosts have incompatible
# umbrella libs (desktop import libs vs WindowsApp.lib, which cannot be mixed
# without duplicate-symbol errors), so each host supplies its own set. Modules
# that need D3D/DXGI/XAudio/Nt* resolve them at runtime via GetProcAddress/COM so
# this static lib references no symbol that only one host can satisfy.
