// The driver host's object-manager namespace: named devices (\Device\X) and
// symbolic links (\??\X -> \Device\X). Only what drivers create lives here;
// every other path stays with the dispatcher / VFS.
#include "../ntoskrnl/internal.h"
#include <cwctype>
#include <unordered_map>

namespace win10xbox::drivers::detail {
namespace {
struct LinkRec { std::wstring link, target; };
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::wstring, DEVICE_OBJECT*>& Devices() { static std::unordered_map<std::wstring, DEVICE_OBJECT*> m; return m; }
std::unordered_map<DEVICE_OBJECT*, std::wstring>& DevNames() { static std::unordered_map<DEVICE_OBJECT*, std::wstring> m; return m; }
std::unordered_map<std::wstring, LinkRec>& Links() { static std::unordered_map<std::wstring, LinkRec> m; return m; }

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Lock held.
DEVICE_OBJECT* ResolveLocked(const std::wstring& name, std::wstring* remainder, std::wstring* resolved) {
    std::wstring cur = NsCanonical(name);
    for (int hop = 0; hop < 8; ++hop) {
        bool followed = false;
        size_t pos = cur.size();
        while (pos > 1) {
            std::wstring prefix = cur.substr(0, pos);
            std::wstring rest = cur.substr(pos);
            auto d = Devices().find(prefix);
            if (d != Devices().end()) {
                if (remainder) *remainder = rest;
                if (resolved) {
                    auto n = DevNames().find(d->second);
                    *resolved = n != DevNames().end() ? n->second : prefix;
                }
                return d->second;
            }
            auto l = Links().find(prefix);
            if (l != Links().end()) {
                cur = NsCanonical(l->second.target) + rest;
                followed = true;
                break;
            }
            size_t slash = cur.rfind(L'\\', pos - 1);
            if (slash == std::wstring::npos || slash == 0) break;
            pos = slash;
        }
        if (!followed) return nullptr;
    }
    WARN(driver, "symbolic link chain too deep: %ls", name.c_str());
    return nullptr;
}
}  // namespace

std::wstring ToWide(const UNICODE_STRING& s) { return std::wstring(s.Buffer ? s.Buffer : L"", s.Length / sizeof(wchar_t)); }
std::wstring_view View(const UNICODE_STRING& s) { return std::wstring_view(s.Buffer ? s.Buffer : L"", s.Length / sizeof(wchar_t)); }

std::wstring NsCanonical(std::wstring_view name) {
    std::wstring s(name);
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    while (s.size() > 1 && s.back() == L'\\') s.pop_back();
    if (StartsWith(s, L"\\dosdevices\\")) s = L"\\??\\" + s.substr(12);
    else if (StartsWith(s, L"\\global??\\")) s = L"\\??\\" + s.substr(10);
    else if (s == L"\\dosdevices" || s == L"\\global??") s = L"\\??";
    return s;
}

DEVICE_OBJECT* NsFindDevice(std::wstring_view name) {
    std::wstring key = NsCanonical(name);
    AcquireSRWLockShared(&g_lock);
    auto it = Devices().find(key);
    DEVICE_OBJECT* d = it == Devices().end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    return d;
}

DEVICE_OBJECT* NsResolve(std::wstring_view name, std::wstring* remainder, std::wstring* resolved_name) {
    AcquireSRWLockShared(&g_lock);
    DEVICE_OBJECT* d = ResolveLocked(std::wstring(name), remainder, resolved_name);
    ReleaseSRWLockShared(&g_lock);
    return d;
}

NTSTATUS NsAddDevice(DEVICE_OBJECT* dev, std::wstring_view name) {
    std::wstring key = NsCanonical(name);
    AcquireSRWLockExclusive(&g_lock);
    if (Devices().count(key) || Links().count(key)) {
        ReleaseSRWLockExclusive(&g_lock);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    Devices()[key] = dev;
    DevNames()[dev] = std::wstring(name);
    ReleaseSRWLockExclusive(&g_lock);
    TRACE(driver, "device %ls created (%p)", std::wstring(name).c_str(), static_cast<void*>(dev));
    return STATUS_SUCCESS;
}

void NsRemoveDevice(DEVICE_OBJECT* dev) {
    AcquireSRWLockExclusive(&g_lock);
    auto n = DevNames().find(dev);
    if (n != DevNames().end()) {
        Devices().erase(NsCanonical(n->second));
        DevNames().erase(n);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring NsDeviceName(DEVICE_OBJECT* dev) {
    AcquireSRWLockShared(&g_lock);
    auto n = DevNames().find(dev);
    std::wstring r = n == DevNames().end() ? std::wstring() : n->second;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

NTSTATUS NsAddLink(std::wstring_view link, std::wstring_view target) {
    std::wstring key = NsCanonical(link);
    AcquireSRWLockExclusive(&g_lock);
    if (Links().count(key) || Devices().count(key)) {
        ReleaseSRWLockExclusive(&g_lock);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    Links()[key] = LinkRec{std::wstring(link), std::wstring(target)};
    ReleaseSRWLockExclusive(&g_lock);
    TRACE(driver, "symbolic link %ls -> %ls", std::wstring(link).c_str(), std::wstring(target).c_str());
    return STATUS_SUCCESS;
}

NTSTATUS NsRemoveLink(std::wstring_view link) {
    std::wstring key = NsCanonical(link);
    AcquireSRWLockExclusive(&g_lock);
    size_t n = Links().erase(key);
    ReleaseSRWLockExclusive(&g_lock);
    return n ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

bool NsQueryLink(std::wstring_view link, std::wstring* target) {
    std::wstring key = NsCanonical(link);
    AcquireSRWLockShared(&g_lock);
    auto it = Links().find(key);
    bool found = it != Links().end();
    if (found && target) *target = it->second.target;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void NsRemoveLinksTargeting(std::wstring_view device_name) {
    std::wstring want = NsCanonical(device_name);
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = Links().begin(); it != Links().end();) {
        if (NsCanonical(it->second.target) == want) it = Links().erase(it);
        else ++it;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool NsHasObjects() {
    AcquireSRWLockShared(&g_lock);
    bool any = !Devices().empty() || !Links().empty();
    ReleaseSRWLockShared(&g_lock);
    return any;
}

void NsClear() {
    AcquireSRWLockExclusive(&g_lock);
    Devices().clear();
    DevNames().clear();
    Links().clear();
    ReleaseSRWLockExclusive(&g_lock);
}

std::vector<std::pair<std::wstring, DEVICE_OBJECT*>> NsSnapshotDevices() {
    std::vector<std::pair<std::wstring, DEVICE_OBJECT*>> out;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : DevNames()) out.emplace_back(kv.second, kv.first);
    ReleaseSRWLockShared(&g_lock);
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> NsSnapshotLinks() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : Links()) out.emplace_back(kv.second.link, kv.second.target);
    ReleaseSRWLockShared(&g_lock);
    return out;
}

}  // namespace win10xbox::drivers::detail

namespace win10xbox::drivers {

std::vector<DeviceInfo> EnumerateDevices() {
    using namespace detail;
    std::vector<DeviceInfo> out;
    for (auto& kv : NsSnapshotDevices()) {
        DeviceInfo di;
        DEVICE_OBJECT* d = kv.second;
        di.name = kv.first;
        di.device_object = d;
        di.device_type = d->DeviceType;
        di.characteristics = d->Characteristics;
        di.flags = d->Flags;
        di.driver = d->DriverObject ? ToWide(d->DriverObject->DriverName) : L"";
        out.push_back(std::move(di));
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> EnumerateSymbolicLinks() { return detail::NsSnapshotLinks(); }

bool ResolveName(const wchar_t* name, std::wstring* device_name) {
    std::wstring rest, resolved;
    if (!detail::NsResolve(name, &rest, &resolved)) return false;
    if (device_name) *device_name = resolved + rest;
    return true;
}

}  // namespace win10xbox::drivers
