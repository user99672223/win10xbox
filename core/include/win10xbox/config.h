// win10xbox core configuration: every design fork listed in docs/DECISIONS.md
// is a field here with its current default. Flags are plain data so hosts and
// tests can override them before Core::Initialize().
#pragma once
#include <cstdint>
#include <string>

namespace win10xbox::config {

enum class ProcessModel   { InProcess, RealChild };           // D1 (RealChild = real suspended children, confirmed on console)
enum class ImageMapping   { Manual, SecImage };               // D2
enum class StubStrategy   { Rewrite, ExportHook };            // D4
enum class Wow64          { Off, HeavensGate };               // D6
enum class DisplayBackend { Headless, D3D12 };                // D8
enum class ClockSource    { HostQpc, Tsc };                   // D9
enum class JitPolicy      { WriteThenProtect, Rwx };          // D11
enum class TrapEngine     { Veh, StaticPatch };               // D12
enum class RegistryBackend{ Regf, HostReadThrough };          // D13 (own REGF reader, not hivex)
enum class DriverHost     { InProcess, Separate };            // D16

struct Config {
    ProcessModel    process_model    = ProcessModel::RealChild;   // D1: console creates real children
    ImageMapping    image_mapping    = ImageMapping::SecImage;     // D2: SEC_IMAGE confirmed on console
    StubStrategy    stub_strategy    = StubStrategy::Rewrite;
    std::uint64_t   memory_budget_bytes = 7ull << 29;        // D5: 3.5 GiB (console shows ~5 GiB, per-build)
    bool            use_guest_arena  = false;                 // D5
    Wow64           wow64            = Wow64::Off;
    bool            inbound_sockets  = true;                  // D7: console allows inbound/loopback/raw
    DisplayBackend  display_backend  = DisplayBackend::Headless;  // D8: D3D12 backend is M6
    ClockSource     clock_source     = ClockSource::HostQpc;
    bool            patch_cpuid      = true;                  // D10
    JitPolicy       jit_policy       = JitPolicy::WriteThenProtect;
    TrapEngine      trap_engine      = TrapEngine::Veh;
    RegistryBackend registry_backend = RegistryBackend::Regf;
    DriverHost      driver_host      = DriverHost::InProcess;

    // D1: may the guest start child processes? NtCreateUserProcess spawns a new HOST process running the host
    // executable itself (GetModuleFileNameW(NULL)) on the child's image, so only a host that is a plain Win32
    // program can turn it on (host-win32 does). The UWP host cannot create processes: it leaves this false and the
    // syscall answers STATUS_NOT_SUPPORTED with a warning. See docs/DISPATCHER.md "Child processes".
    bool            child_processes  = false;

    // Machine folder root (the virtual C:\). Hosts set this; D15.
    std::wstring    machine_root;

    // D18: single CPU-identity story served by the CPUID patch, the registry
    // overlay (ProcessorNameString), the synthetic SMBIOS type-4 table and
    // wbemprox Win32_Processor. The console reports "Microsoft Xbox Series S
    // CPU"; the guest is told it is the desktop-kit silicon instead. Firmware
    // tables are denied on the console, so GetSystemFirmwareTable is emulated.
    struct MachineIdentity {
        std::wstring cpu_brand = L"AMD 4700S 8-Core Processor";
        std::uint32_t cpu_family = 23, cpu_model = 136, cpu_stepping = 1;  // Zen 2, matches console
        bool advertise_rdrand = true;     // absent on console; emulated via #UD trap -> BCryptGenRandom
        bool hide_hypervisor_leaf = true; // leaf 0x40000000 -> all zeros (console returns "Microsoft Xb")
    } machine_identity;

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
