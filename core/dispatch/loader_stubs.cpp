// Stand-ins for core/loader, compiled only while that module is absent
// (WIN10XBOX_HAVE_LOADER=OFF): the dispatcher links and its unit tests run;
// process::Launch fails with STATUS_NOT_IMPLEMENTED at the first mapping.
#include "win10xbox/loader.h"
#include "win10xbox/trace.h"
#include <cstring>

namespace win10xbox::loader {

static constexpr NTSTATUS kNotImplemented = static_cast<NTSTATUS>(0xC0000002);

NTSTATUS MapImageFromMemory(const void*, std::size_t, MapFlags, void*, MappedImage*) {
    ERR(loader, "core/loader is not built (WIN10XBOX_HAVE_LOADER=OFF)");
    return kNotImplemented;
}
NTSTATUS MapImageFromFile(const wchar_t*, MapFlags, void*, MappedImage*) { return kNotImplemented; }
NTSTATUS FinalizeProtections(const MappedImage&) { return kNotImplemented; }
void UnmapImage(MappedImage&) {}

void* FindExport(const void* image_base, const char* name, std::string* forwarder) {
    auto* base = static_cast<const std::uint8_t*>(image_base);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return nullptr;
    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    auto* ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name)) continue;
        DWORD rva = funcs[ords[i]];
        if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) {
            if (forwarder) *forwarder = reinterpret_cast<const char*>(base + rva);
            return nullptr;
        }
        return const_cast<std::uint8_t*>(base + rva);
    }
    return nullptr;
}
void* FindExportByOrdinal(const void*, std::uint16_t, std::string*) { return nullptr; }

std::size_t RewriteSyscallStubs(const MappedImage&, void*, std::vector<StubPatch>*, std::vector<std::string>*) { return 0; }
NTSTATUS ScanAndPatch(const MappedImage&, const ScanOptions&, ScanResult*) { return kNotImplemented; }
NTSTATUS ScanAndPatchRange(void*, std::size_t, const ScanOptions&, ScanResult*) { return kNotImplemented; }
void EnsureTrapHandlerInstalled() {}

}  // namespace win10xbox::loader
