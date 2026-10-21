// Driver loading: map the .sys with core/loader, bind its imports to the
// user-mode ntoskrnl, build the DRIVER_OBJECT and call DriverEntry.
#include "../ntoskrnl/internal.h"
#include <algorithm>
#include <memory>

namespace ntk {
VOID NTAPI IoDeleteDevice(PDEVICE_OBJECT);
}

namespace win10xbox::drivers::detail {

LIST_ENTRY g_ps_loaded_module_list;
void CloseHandlesOfDriver(DRIVER_OBJECT* drv);   // irp_top.cpp

struct DriverRec {
    std::uint32_t id = 0;
    std::wstring name;           // \Driver\X
    std::wstring registry_path;
    std::wstring file_name;      // X.sys
    DRIVER_OBJECT* drv = nullptr;
    loader::MappedImage img;
    bool has_image = false;
    StubArena arena;
    std::size_t stubs = 0;
    KLDR_DATA_TABLE_ENTRY_NTK* ldr = nullptr;
    bool unloaded = false;
};

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::unique_ptr<DriverRec>>& Drivers() { static std::vector<std::unique_ptr<DriverRec>> v; return v; }
std::uint32_t g_next_id = 1;
win10xbox::drivers::ImageLoadHook g_image_hook = nullptr;
void* g_image_hook_ctx = nullptr;

struct DriverBlob {
    DRIVER_OBJECT drv;
    DRIVER_EXTENSION ext;
    UNICODE_STRING hwdb;      // DRIVER_OBJECT::HardwareDatabase
    UNICODE_STRING regpath;   // RegistryPath argument of DriverEntry (service key)
};

std::wstring Stem(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring f = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = f.find_last_of(L'.');
    return dot == std::wstring::npos ? f : f.substr(0, dot);
}
std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

PWSTR CopyWide(const std::wstring& s, USHORT* len_bytes, USHORT* max_bytes) {
    size_t bytes = s.size() * sizeof(wchar_t);
    auto* p = static_cast<wchar_t*>(PoolAlloc(bytes + sizeof(wchar_t), kTagStr));
    if (p) std::memcpy(p, s.c_str(), bytes + sizeof(wchar_t));
    *len_bytes = static_cast<USHORT>(bytes);
    *max_bytes = static_cast<USHORT>(bytes + sizeof(wchar_t));
    return p;
}

bool RvaOk(const loader::MappedImage& img, std::uint64_t rva, std::uint64_t len) { return rva + len <= img.size && rva + len >= rva; }

// Writes the import address table of a mapped (still writable) image.
NTSTATUS BindImports(const loader::MappedImage& img, StubArena* arena, std::size_t* stubs, const std::wstring& label) {
    auto* base = static_cast<std::uint8_t*>(img.base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
        WARN(driver, "%ls: delay-load imports are not bound", label.c_str());
    if (!dir.VirtualAddress) return STATUS_SUCCESS;
    if (!RvaOk(img, dir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) return STATUS_INVALID_IMAGE_FORMAT;
    std::vector<std::string> stubbed;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (!RvaOk(img, d->Name, 2) || !RvaOk(img, d->FirstThunk, 8)) return STATUS_INVALID_IMAGE_FORMAT;
        const char* module = reinterpret_cast<const char*>(base + d->Name);
        const std::uint32_t ref_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        auto* ref = reinterpret_cast<std::uint64_t*>(base + ref_rva);
        auto* iat = reinterpret_cast<std::uint64_t*>(base + d->FirstThunk);
        for (std::size_t i = 0;; ++i) {
            if (!RvaOk(img, ref_rva + i * 8, 8) || !RvaOk(img, d->FirstThunk + i * 8, 8)) return STATUS_INVALID_IMAGE_FORMAT;
            const std::uint64_t r = ref[i];
            if (!r) break;
            void* target;
            bool is_stub = false;
            std::string label_name;
            if (r & IMAGE_ORDINAL_FLAG64) {
                label_name = "ordinal#" + std::to_string(r & 0xFFFF);
                target = arena->Add((std::string(module) + "!" + label_name).c_str());
                is_stub = true;
            } else {
                if (!RvaOk(img, r, 4)) return STATUS_INVALID_IMAGE_FORMAT;
                const char* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + r)->Name;
                label_name = name;
                target = BindImport(module, name, arena, &is_stub);
            }
            if (!target) return STATUS_INSUFFICIENT_RESOURCES;
            iat[i] = reinterpret_cast<std::uint64_t>(target);
            if (is_stub) {
                ++*stubs;
                stubbed.push_back(std::string(module) + "!" + label_name);
            }
        }
    }
    for (const std::string& s : stubbed)
        WARN(driver, "%ls: import %s is not implemented (bound to a loud STATUS_NOT_IMPLEMENTED stub)", label.c_str(), s.c_str());
    return STATUS_SUCCESS;
}

void DriverObjectDestroy(void* p) {
    auto* blob = static_cast<DriverBlob*>(p);
    PoolFree(blob->drv.DriverName.Buffer, 0, false);
    PoolFree(blob->hwdb.Buffer, 0, false);
    PoolFree(blob->regpath.Buffer, 0, false);
    PoolFree(blob->ext.ServiceKeyName.Buffer, 0, false);
    PoolFree(blob, 0, false);
}

}  // namespace

DriverRec* DriverFromObject(DRIVER_OBJECT* d) {
    DriverRec* r = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (auto& up : Drivers())
        if (up->drv == d) { r = up.get(); break; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

std::wstring DriverObjectName(DRIVER_OBJECT* d) { return d ? ToWide(d->DriverName) : std::wstring(); }

DRIVER_OBJECT* DriverByName(std::wstring_view name) {
    std::wstring key = NsCanonical(name);
    DRIVER_OBJECT* r = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (auto& up : Drivers())
        if (NsCanonical(up->name) == key) { r = up->drv; break; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

NTSTATUS CreateDriverObject(const std::wstring& name, const std::wstring& registry_path, DRIVER_OBJECT** out) {
    auto* blob = static_cast<DriverBlob*>(PoolAlloc(sizeof(DriverBlob), kTagDrv));
    if (!blob) return STATUS_INSUFFICIENT_RESOURCES;
    DRIVER_OBJECT* d = &blob->drv;
    d->Type = DRIVER_OBJECT_TYPE_ID;
    d->Size = sizeof(DRIVER_OBJECT);
    d->Flags = DRVO_LEGACY_DRIVER;
    d->DriverName.Buffer = CopyWide(name, &d->DriverName.Length, &d->DriverName.MaximumLength);
    blob->hwdb.Buffer = CopyWide(L"\\Registry\\Machine\\Hardware\\Description\\System", &blob->hwdb.Length, &blob->hwdb.MaximumLength);
    blob->regpath.Buffer = CopyWide(registry_path, &blob->regpath.Length, &blob->regpath.MaximumLength);
    d->HardwareDatabase = &blob->hwdb;
    d->DriverExtension = &blob->ext;
    blob->ext.DriverObject = d;
    // DriverExtension->ServiceKeyName = the registry path (the key name is its last component).
    std::wstring svc = registry_path;
    size_t slash = svc.find_last_of(L'\\');
    if (slash != std::wstring::npos) svc = svc.substr(slash + 1);
    blob->ext.ServiceKeyName.Buffer = CopyWide(svc, &blob->ext.ServiceKeyName.Length, &blob->ext.ServiceKeyName.MaximumLength);
    for (auto& f : d->MajorFunction) f = &InvalidDeviceRequest;
    ObjRegister(d, ObjType::Driver, [](void* p) { DriverObjectDestroy(p); }, 1);
    *out = d;
    return STATUS_SUCCESS;
}

void DestroyDriverObject(DRIVER_OBJECT* d) {
    // The blob starts with the DRIVER_OBJECT: DriverObjectDestroy is registered, drop the creation reference.
    ObjDeref(d);
}

namespace {

void ClearInitializingFlags(DRIVER_OBJECT* drv) {
    for (PDEVICE_OBJECT d = drv->DeviceObject; d; d = d->NextDevice) d->Flags &= ~static_cast<ULONG>(DO_DEVICE_INITIALIZING);
}

void DeleteLeftoverDevices(DRIVER_OBJECT* drv) {
    while (drv->DeviceObject) {
        PDEVICE_OBJECT d = drv->DeviceObject;
        std::wstring n = NsDeviceName(d);
        if (!n.empty()) {
            WARN(driver, "%ls left device %ls behind, deleting it", DriverObjectName(drv).c_str(), n.c_str());
            NsRemoveLinksTargeting(n);
        }
        ntk::IoDeleteDevice(d);
    }
}

void InsertLoadedModule(DriverRec* r) {
    if (!r->has_image) return;
    const std::wstring full = L"\\SystemRoot\\System32\\drivers\\" + r->file_name;
    SIZE_T bytes = sizeof(KLDR_DATA_TABLE_ENTRY_NTK);
    auto* e = static_cast<KLDR_DATA_TABLE_ENTRY_NTK*>(PoolAlloc(bytes, kTagMisc));
    if (!e) return;
    e->DllBase = r->img.base;
    e->EntryPoint = r->img.entry;
    e->SizeOfImage = static_cast<ULONG>(r->img.size);
    e->FullDllName.Buffer = CopyWide(full, &e->FullDllName.Length, &e->FullDllName.MaximumLength);
    e->BaseDllName.Buffer = CopyWide(r->file_name, &e->BaseDllName.Length, &e->BaseDllName.MaximumLength);
    e->LoadCount = 1;
    e->Flags = 0x00004000;   // LDRP_ENTRY_PROCESSED
    InsertTailList(&g_ps_loaded_module_list, &e->InLoadOrderLinks);
    r->ldr = e;
    r->drv->DriverSection = e;
}

void RemoveLoadedModule(DriverRec* r) {
    if (!r->ldr) return;
    RemoveEntryList(&r->ldr->InLoadOrderLinks);
    PoolFree(r->ldr->FullDllName.Buffer, 0, false);
    PoolFree(r->ldr->BaseDllName.Buffer, 0, false);
    PoolFree(r->ldr, 0, false);
    r->ldr = nullptr;
}

void ReleaseRec(DriverRec* r) {
    RemoveLoadedModule(r);
    if (r->drv) { DestroyDriverObject(r->drv); r->drv = nullptr; }
    if (r->has_image) { loader::UnmapImage(r->img); r->has_image = false; }
}

}  // namespace

void DriversUnloadAll() {
    for (;;) {
        std::uint32_t id = 0;
        AcquireSRWLockShared(&g_lock);
        if (!Drivers().empty()) id = Drivers().back()->id;
        ReleaseSRWLockShared(&g_lock);
        if (!id) break;
        win10xbox::drivers::UnloadDriver(id);
    }
}

}  // namespace win10xbox::drivers::detail

namespace win10xbox::drivers {
using namespace detail;

void SetImageLoadHook(ImageLoadHook hook, void* ctx) {
    g_image_hook = hook;
    g_image_hook_ctx = ctx;
}

NTSTATUS LoadDriver(const wchar_t* host_path, const LoadOptions* opts, DriverInfo* info) {
    if (!IsInitialized()) return STATUS_UNSUCCESSFUL;
    if (!host_path) return STATUS_INVALID_PARAMETER;
    auto rec = std::make_unique<DriverRec>();
    const std::wstring path = host_path;
    rec->file_name = FileName(path);
    const std::wstring stem = Stem(path);
    rec->name = (opts && !opts->driver_name.empty()) ? opts->driver_name : L"\\Driver\\" + stem;
    rec->registry_path = (opts && !opts->registry_path.empty())
                             ? opts->registry_path
                             : L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\" + stem;
    if (DriverByName(rec->name)) return STATUS_IMAGE_ALREADY_LOADED;

    NTSTATUS st = loader::MapImageFromFile(path.c_str(), loader::kDefaultMapFlags | loader::MapFlags::ReserveOnly, nullptr, &rec->img);
    if (!NT_SUCCESS(st)) {
        ERR(driver, "%ls: cannot map the image: %08lx", path.c_str(), static_cast<unsigned long>(st));
        return st;
    }
    rec->has_image = true;
    if (!rec->img.entry) {
        ERR(driver, "%ls has no entry point", path.c_str());
        loader::UnmapImage(rec->img);
        return STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
    st = BindImports(rec->img, &rec->arena, &rec->stubs, rec->file_name);
    if (NT_SUCCESS(st) && !rec->arena.Seal()) st = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(st) && g_image_hook) st = g_image_hook(rec->img, g_image_hook_ctx);
    if (NT_SUCCESS(st)) st = loader::FinalizeProtections(rec->img);
    if (!NT_SUCCESS(st)) {
        ERR(driver, "%ls: import binding / finalisation failed: %08lx", path.c_str(), static_cast<unsigned long>(st));
        loader::UnmapImage(rec->img);
        return st;
    }
    st = CreateDriverObject(rec->name, rec->registry_path, &rec->drv);
    if (!NT_SUCCESS(st)) { loader::UnmapImage(rec->img); return st; }
    DRIVER_OBJECT* drv = rec->drv;
    drv->DriverStart = rec->img.base;
    drv->DriverSize = static_cast<ULONG>(rec->img.size);
    drv->DriverInit = reinterpret_cast<PDRIVER_INITIALIZE>(rec->img.entry);
    rec->id = g_next_id++;
    InsertLoadedModule(rec.get());

    // Registry path is passed as a counted string that stays valid for the driver's lifetime.
    UNICODE_STRING* regpath = &reinterpret_cast<DriverBlob*>(drv)->regpath;
    TRACE(driver, "%ls: calling DriverEntry at %p (image %p, %zu bytes, %zu stubbed imports)", rec->name.c_str(),
          rec->img.entry, rec->img.base, rec->img.size, rec->stubs);
    KIRQL saved = SwapIrql(PASSIVE_LEVEL);
    DriverRec* raw = rec.get();
    {
        // DriverEntry may create devices and call back into the host; the record must be findable.
        AcquireSRWLockExclusive(&g_lock);
        Drivers().push_back(std::move(rec));
        ReleaseSRWLockExclusive(&g_lock);
    }
    st = drv->DriverInit(drv, regpath);
    SwapIrql(saved);
    TRACE(driver, "%ls: DriverEntry returned %08lx", raw->name.c_str(), static_cast<unsigned long>(st));
    if (!NT_SUCCESS(st)) {
        // Failed load: remove what the driver created, drop the image.
        DeleteLeftoverDevices(drv);
        std::unique_ptr<DriverRec> failed;
        AcquireSRWLockExclusive(&g_lock);
        for (auto it = Drivers().begin(); it != Drivers().end(); ++it)
            if (it->get() == raw) { failed = std::move(*it); Drivers().erase(it); break; }
        ReleaseSRWLockExclusive(&g_lock);
        if (failed) ReleaseRec(failed.get());
        return st;
    }
    ClearInitializingFlags(drv);
    NotifyImageLoaded(L"\\SystemRoot\\System32\\drivers\\" + raw->file_name, raw->img.base, raw->img.size);
    if (info) {
        info->id = raw->id;
        info->name = raw->name;
        info->driver_object = drv;
        info->image_base = raw->img.base;
        info->image_size = raw->img.size;
        info->stubbed_imports = raw->stubs;
    }
    return STATUS_SUCCESS;
}

NTSTATUS UnloadDriver(std::uint32_t id) {
    std::unique_ptr<DriverRec> rec;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = Drivers().begin(); it != Drivers().end(); ++it)
        if ((*it)->id == id) { rec = std::move(*it); Drivers().erase(it); break; }
    ReleaseSRWLockExclusive(&g_lock);
    if (!rec) return STATUS_NOT_FOUND;
    DRIVER_OBJECT* drv = rec->drv;
    TRACE(driver, "unloading %ls", rec->name.c_str());
    CloseHandlesOfDriver(drv);
    if (drv->DriverUnload && !(drv->Flags & DRVO_UNLOAD_INVOKED)) {
        drv->Flags |= DRVO_UNLOAD_INVOKED;
        KIRQL saved = SwapIrql(PASSIVE_LEVEL);
        drv->DriverUnload(drv);
        SwapIrql(saved);
    }
    WorkerFlushDpcs();   // no DPC/work item of the driver may run after its image is gone
    DeleteLeftoverDevices(drv);
    ReleaseRec(rec.get());
    return STATUS_SUCCESS;
}

}  // namespace win10xbox::drivers
