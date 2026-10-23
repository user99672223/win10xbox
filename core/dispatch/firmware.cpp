// Firmware tables (D18): the console denies GetSystemFirmwareTable, so the guest's only SMBIOS is the one built here
// from config::machine_identity and the host's processor topology. NtQuerySystemInformation(SystemFirmwareTable
// Information, 'RSMB') is answered from it; every other provider goes to the host.
//
// The table is the raw SMBIOS data Windows hands out (RawSMBIOSData: calling method, SMBIOS major/minor, DMI revision,
// the length of the table, then the structures): type 0 (BIOS), type 1 (system), type 4 (processor) and type 127
// (end of table), each a formatted area followed by its strings and a double NUL. Type 4 is SMBIOS 3.3's, 0x30 bytes:
// ProcessorVersion is the brand string, ProcessorId holds CPUID.1 EAX (the family/model/stepping signature the CPUID
// patch serves) and EDX, the core and thread counts are the host's.
#include "internal.h"
#include "win10xbox/config.h"
#include <cstring>

namespace win10xbox::dispatch::detail {
namespace {

constexpr ULONG kProviderRsmb = 0x52534D42;   // 'RSMB'
constexpr ULONG kActionEnumerate = 0, kActionGet = 1;
constexpr ULONG kInfoHeader = 16;             // SYSTEM_FIRMWARE_TABLE_INFORMATION up to TableBuffer

SRWLOCK g_fw_lock = SRWLOCK_INIT;
std::vector<std::uint8_t> g_smbios;           // RawSMBIOSData; empty until BuildFirmwareTables

struct Structure {
    std::vector<std::uint8_t> bytes;          // formatted area, then strings
    std::vector<std::string> strings;
    explicit Structure(std::size_t formatted) : bytes(formatted, 0) {}
    void U8(std::size_t at, unsigned v) { bytes[at] = static_cast<std::uint8_t>(v); }
    void U16(std::size_t at, unsigned v) { U8(at, v & 0xFF); U8(at + 1, v >> 8); }
    void U64(std::size_t at, std::uint64_t v) { for (int i = 0; i < 8; ++i) U8(at + i, (v >> (8 * i)) & 0xFF); }
    // Adds a string to the string set and returns its 1-based index (0 = none).
    unsigned Str(const std::string& s) {
        if (s.empty()) return 0;
        strings.push_back(s);
        return static_cast<unsigned>(strings.size());
    }
    void Finish(std::vector<std::uint8_t>& out) const {
        out.insert(out.end(), bytes.begin(), bytes.end());
        if (strings.empty()) {
            out.push_back(0);
        } else {
            for (const auto& s : strings) {
                out.insert(out.end(), s.begin(), s.end());
                out.push_back(0);
            }
        }
        out.push_back(0);
    }
};

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

struct Topology { unsigned cores = 0, threads = 0; };

Topology HostTopology() {
    Topology t;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
        std::vector<std::uint8_t> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                             reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            for (DWORD off = 0; off < len;) {
                auto* e = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                if (!e->Size) break;
                if (e->Relationship == RelationProcessorCore) {
                    ++t.cores;
                    for (WORD g = 0; g < e->Processor.GroupCount; ++g)
                        t.threads += static_cast<unsigned>(__popcnt64(e->Processor.GroupMask[g].Mask));
                }
                off += e->Size;
            }
        }
    }
    if (!t.cores || !t.threads) {
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        t.cores = t.threads = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    }
    return t;
}

unsigned HostMhz() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &k) !=
        ERROR_SUCCESS)
        return 0;
    DWORD type = 0, v = 0, size = sizeof v;
    const LONG r = RegQueryValueExW(k, L"~MHz", nullptr, &type, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD ? v : 0;
}

std::uint32_t Signature(const config::Config::MachineIdentity& id) {
    const std::uint32_t base_family = id.cpu_family > 0xF ? 0xF : id.cpu_family;
    const std::uint32_t ext_family = id.cpu_family > 0xF ? id.cpu_family - 0xF : 0;
    return (id.cpu_stepping & 0xF) | ((id.cpu_model & 0xF) << 4) | (base_family << 8) |
           (((id.cpu_model >> 4) & 0xF) << 16) | (ext_family << 20);
}

}  // namespace

std::vector<std::uint8_t> MakeSmbios(const config::Config::MachineIdentity& id, const std::string& vendor,
                                     unsigned cores, unsigned threads, unsigned mhz, std::uint32_t id_edx) {
    std::vector<std::uint8_t> t;

    {   // Type 0: BIOS information (SMBIOS 2.4: 0x18 bytes)
        Structure s(0x18);
        s.U8(0, 0); s.U8(1, 0x18); s.U16(2, 0x0000);
        s.U8(4, s.Str("win10xbox"));
        s.U8(5, s.Str("1.0"));
        s.U16(6, 0xE000);                  // starting address segment
        s.U8(8, s.Str("01/01/2024"));
        s.U8(9, 0xFF);                     // ROM size: see the extended field
        s.U64(10, 0x08);                   // characteristics: not supported
        s.U8(18, 0x01); s.U8(19, 0x00);    // extension bytes
        s.U8(20, 1); s.U8(21, 0);          // system BIOS release
        s.U8(22, 0xFF); s.U8(23, 0xFF);    // embedded controller release: none
        s.Finish(t);
    }
    {   // Type 1: system information (SMBIOS 2.4: 0x1B bytes); the UUID is "not set" (all FF)
        Structure s(0x1B);
        s.U8(0, 1); s.U8(1, 0x1B); s.U16(2, 0x0001);
        s.U8(4, s.Str("win10xbox"));
        s.U8(5, s.Str("Windows on Xbox"));
        s.U8(6, s.Str("1.0"));
        s.U8(7, s.Str("0"));
        for (int i = 0; i < 16; ++i) s.U8(8 + i, 0xFF);
        s.U8(24, 0x06);                    // wake-up type: power switch
        s.U8(25, s.Str("win10xbox"));
        s.U8(26, s.Str("Xbox"));
        s.Finish(t);
    }
    {   // Type 4: processor information (SMBIOS 3.3: 0x30 bytes)
        Structure s(0x30);
        const unsigned family = 0x6B;      // Zen
        s.U8(0, 4); s.U8(1, 0x30); s.U16(2, 0x0004);
        s.U8(4, s.Str("Socket 0"));
        s.U8(5, 3);                        // central processor
        s.U8(6, family);
        s.U8(7, s.Str(vendor));
        s.U64(8, static_cast<std::uint64_t>(Signature(id)) | (static_cast<std::uint64_t>(id_edx) << 32));
        s.U8(16, s.Str(Narrow(id.cpu_brand)));
        s.U8(17, 0x8B);                    // voltage: 1.1 V
        s.U16(18, 100);                    // external clock
        s.U16(20, mhz); s.U16(22, mhz);    // max and current speed
        s.U8(24, 0x41);                    // populated, enabled
        s.U8(25, 0x01);                    // upgrade: other
        s.U16(26, 0xFFFF); s.U16(28, 0xFFFF); s.U16(30, 0xFFFF);   // cache handles: none
        s.U8(32, 0); s.U8(33, 0); s.U8(34, 0);
        s.U8(35, cores > 0xFF ? 0xFF : cores);
        s.U8(36, cores > 0xFF ? 0xFF : cores);
        s.U8(37, threads > 0xFF ? 0xFF : threads);
        s.U16(38, 0x00FC);                 // 64-bit, multi-core, hardware thread, execute protection, virtualization, power control
        s.U16(40, family);
        s.U16(42, cores); s.U16(44, cores); s.U16(46, threads);
        s.Finish(t);
    }
    {   // Type 127: end of table
        Structure s(4);
        s.U8(0, 127); s.U8(1, 4); s.U16(2, 0x007F);
        s.Finish(t);
    }

    std::vector<std::uint8_t> raw;
    const std::uint32_t len = static_cast<std::uint32_t>(t.size());
    raw.push_back(0);                      // calling method
    raw.push_back(3);                      // SMBIOS major
    raw.push_back(3);                      // SMBIOS minor
    raw.push_back(0);                      // DMI revision
    for (int i = 0; i < 4; ++i) raw.push_back(static_cast<std::uint8_t>(len >> (8 * i)));
    raw.insert(raw.end(), t.begin(), t.end());
    return raw;
}

void BuildFirmwareTables(const std::string& vendor) {
    int r[4] = {};
    __cpuid(r, 1);
    const Topology topo = HostTopology();
    std::vector<std::uint8_t> raw = MakeSmbios(config::Current().machine_identity, vendor, topo.cores, topo.threads,
                                               HostMhz(), static_cast<std::uint32_t>(r[3]));
    AcquireSRWLockExclusive(&g_fw_lock);
    g_smbios = std::move(raw);
    ReleaseSRWLockExclusive(&g_fw_lock);
    TRACE(dispatch, "SMBIOS table: %zu bytes, %u cores / %u threads", g_smbios.size(), topo.cores, topo.threads);
}

// SYSTEM_FIRMWARE_TABLE_INFORMATION { ULONG ProviderSignature, Action, TableID, TableBufferLength; UCHAR TableBuffer[] }.
// Returns false when the request is not ours (another provider, or no table was built): the caller asks the host.
bool QuerySystemFirmwareTable(PVOID buffer, ULONG length, PULONG ret_len, NTSTATUS* status) {
    if (!buffer || length < kInfoHeader) return false;
    auto* info = static_cast<std::uint32_t*>(buffer);
    if (info[0] != kProviderRsmb) return false;
    AcquireSRWLockShared(&g_fw_lock);
    const std::size_t have = g_smbios.size();
    ULONG need = 0;
    const std::uint8_t* src = nullptr;
    std::uint32_t zero_id = 0;
    if (have && info[1] == kActionEnumerate) {
        need = sizeof(std::uint32_t);          // the table ids of RSMB: one, 0
        src = reinterpret_cast<const std::uint8_t*>(&zero_id);
    } else if (have && info[1] == kActionGet) {
        need = static_cast<ULONG>(have);
        src = g_smbios.data();
    }
    if (!src) {
        ReleaseSRWLockShared(&g_fw_lock);
        if (have) *status = st::NotImplemented;   // an action the provider does not know
        return have != 0;
    }
    info[3] = need;
    if (ret_len) *ret_len = kInfoHeader + need;
    if (length < kInfoHeader + need) {
        *status = st::BufferTooSmall;
    } else {
        std::memcpy(info + 4, src, need);
        *status = st::Success;
    }
    ReleaseSRWLockShared(&g_fw_lock);
    return true;
}

}  // namespace win10xbox::dispatch::detail
