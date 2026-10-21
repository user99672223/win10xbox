// win10xbox core configuration: every design fork listed in docs/DECISIONS.md
// is a field here with its current default. Flags are plain data so hosts and
// tests can override them before Core::Initialize().
#pragma once
#include <cstdint>
#include <string>

namespace win10xbox::config {

enum class ProcessModel   { InProcess, MultiInstance };      // D1
enum class ImageMapping   { Manual, SecImage };               // D2
enum class StubStrategy   { Rewrite, ExportHook };            // D4
enum class Wow64          { Off, HeavensGate };               // D6
enum class DisplayBackend { Headless, D3D12 };                // D8
enum class ClockSource    { HostQpc, Tsc };                   // D9
enum class JitPolicy      { WriteThenProtect, Rwx };          // D11
enum class TrapEngine     { Veh, StaticPatch };               // D12
enum class RegistryBackend{ Hivex, HostReadThrough };         // D13
enum class DriverHost     { InProcess, Separate };            // D16

struct Config {
    ProcessModel    process_model    = ProcessModel::InProcess;
    ImageMapping    image_mapping    = ImageMapping::Manual;
    StubStrategy    stub_strategy    = StubStrategy::Rewrite;
    std::uint64_t   memory_budget_bytes = 2ull << 30;        // D5
    bool            use_guest_arena  = false;                 // D5
    Wow64           wow64            = Wow64::Off;
    bool            inbound_sockets  = false;                 // D7
    DisplayBackend  display_backend  = DisplayBackend::Headless;
    ClockSource     clock_source     = ClockSource::HostQpc;
    bool            patch_cpuid      = true;                  // D10
    JitPolicy       jit_policy       = JitPolicy::WriteThenProtect;
    TrapEngine      trap_engine      = TrapEngine::Veh;
    RegistryBackend registry_backend = RegistryBackend::Hivex;
    DriverHost      driver_host      = DriverHost::InProcess;

    // Machine folder root (the virtual C:\). Hosts set this; D15.
    std::wstring    machine_root;

    // Version the guest sees (D14: fidelity values live in machine.json and
    // override these).
    std::uint32_t   os_major = 10, os_minor = 0, os_build = 19045;   // 22H2
    std::wstring    os_csd;  // "" for 22H2
    std::uint32_t   os_ubr = 3803;                                   // 19045.3803

    // Tracing: on in Debug builds, off in Release unless WIN10XBOX_TRACE is set.
#if defined(_DEBUG) || !defined(NDEBUG) || defined(WIN10XBOX_TRACE_DEFAULT_ON)
    bool trace_default_on = true;
#else
    bool trace_default_on = false;
#endif
};

Config& Current();   // process-wide, mutable until Core::Initialize()

}  // namespace win10xbox::config
