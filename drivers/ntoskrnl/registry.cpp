// ntoskrnl registry helpers drivers use instead of raw Zw* calls: RtlQueryRegistryValues(Ex),
// RtlWriteRegistryValue, RtlDeleteRegistryValue, RtlCheckRegistryKey, RtlCreateRegistryKey. MIT.
//
// They are built on the dispatcher's NtOpenKey/NtQueryValueKey/... (not on the host ntdll's own
// Rtl*Registry*), so a driver sees the emulated registry exactly like the guest does. The behaviour
// follows the documented RTL_QUERY_REGISTRY_TABLE semantics (SUBKEY, TOPKEY, REQUIRED, NOVALUE, NOEXPAND,
// DIRECT, DELETE, NOSTRING, TYPECHECK); REG_EXPAND_SZ is expanded with the host environment.
#include "internal.h"

namespace ntk {
using namespace win10xbox::drivers::detail;

namespace {

constexpr ULONG kRelAbsolute = 0, kRelServices = 1, kRelControl = 2, kRelWindowsNt = 3, kRelDeviceMap = 4, kRelUser = 5,
                kRelMask = 0x3FFFFFFF, kRelHandle = 0x40000000;
constexpr ULONG kQuerySubkey = 0x1, kQueryTopkey = 0x2, kQueryRequired = 0x4, kQueryNovalue = 0x8, kQueryNoexpand = 0x10,
                kQueryDirect = 0x20, kQueryDelete = 0x40, kQueryNostring = 0x80, kQueryTypecheck = 0x100;
constexpr ULONG kRegNone = 0, kRegSz = 1, kRegExpandSz = 2, kRegLink = 6, kRegMultiSz = 7;

struct QueryEntry {
    PVOID routine;
    ULONG flags;
    PWSTR name;
    PVOID context;
    ULONG default_type;
    PVOID default_data;
    ULONG default_length;
};
using QueryRoutine = NTSTATUS(NTAPI*)(PWSTR name, ULONG type, PVOID data, ULONG length, PVOID context, PVOID entry_context);

struct PartialInfo { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; };

using FnOpenKey = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using FnCreateKey = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
using FnQueryValue = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
using FnEnumValue = NTSTATUS(NTAPI*)(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
using FnSetValue = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
using FnDeleteValue = NTSTATUS(NTAPI*)(HANDLE, PUNICODE_STRING);
using FnClose = NTSTATUS(NTAPI*)(HANDLE);

template <class Fn> Fn Nt(const char* name) { return reinterpret_cast<Fn>(ResolveNtFunction(name)); }

constexpr ACCESS_MASK kKeyRead = 0x20019, kKeyWrite = 0x20006, kKeyAll = 0xF003F;

std::wstring BasePath(ULONG relative_to) {
    switch (relative_to & kRelMask) {
    case kRelAbsolute: return L"";
    case kRelServices: return L"\\Registry\\Machine\\System\\CurrentControlSet\\Services";
    case kRelControl: return L"\\Registry\\Machine\\System\\CurrentControlSet\\Control";
    case kRelWindowsNt: return L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion";
    case kRelDeviceMap: return L"\\Registry\\Machine\\Hardware\\DeviceMap";
    case kRelUser: return L"\\Registry\\User\\.Default";
    default: return L"\\";
    }
}

UNICODE_STRING Counted(const std::wstring& s) {
    UNICODE_STRING u;
    u.Buffer = const_cast<PWSTR>(s.c_str());
    u.Length = static_cast<USHORT>(s.size() * 2);
    u.MaximumLength = u.Length + 2;
    return u;
}

// Opens (or creates) the key `relative_to`/`path`, or uses the handle passed with RTL_REGISTRY_HANDLE.
NTSTATUS OpenBase(ULONG relative_to, PCWSTR path, HANDLE* key, bool* owned, bool create, ACCESS_MASK access) {
    *owned = false;
    if (relative_to & kRelHandle) {
        *key = reinterpret_cast<HANDLE>(const_cast<PWSTR>(path));
        return STATUS_SUCCESS;
    }
    std::wstring full = BasePath(relative_to);
    if (path && *path) {
        if (!full.empty() && path[0] != L'\\') full += L"\\";
        full += path;
    }
    UNICODE_STRING name = Counted(full);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    NTSTATUS st;
    if (create) {
        auto fn = Nt<FnCreateKey>("NtCreateKey");
        if (!fn) return STATUS_NOT_IMPLEMENTED;
        st = fn(key, access, &oa, 0, nullptr, 0, nullptr);
    } else {
        auto fn = Nt<FnOpenKey>("NtOpenKey");
        if (!fn) return STATUS_NOT_IMPLEMENTED;
        st = fn(key, access, &oa);
    }
    if (NT_SUCCESS(st)) *owned = true;
    return st;
}

void CloseKey(HANDLE key) {
    if (auto fn = Nt<FnClose>("NtClose")) fn(key);
}

bool IsString(ULONG t) { return t == kRegSz || t == kRegExpandSz || t == kRegLink; }

// Delivers one value to the table entry: calls the routine or stores it with DIRECT.
NTSTATUS Deliver(const QueryEntry& e, PWSTR name, ULONG type, PVOID data, ULONG length, PVOID context) {
    if (!(e.flags & kQueryDirect)) {
        if (!e.routine) return STATUS_SUCCESS;
        return reinterpret_cast<QueryRoutine>(e.routine)(name, type, data, length, context, e.context);
    }
    if (!e.context) return STATUS_INVALID_PARAMETER;
    if (IsString(type)) {
        auto* us = static_cast<PUNICODE_STRING>(e.context);
        if (e.flags & kQueryNostring) return STATUS_INVALID_PARAMETER;
        if (!us->Buffer) {
            us->Buffer = static_cast<PWSTR>(ExAllocatePoolWithTag(PagedPool, length + sizeof(WCHAR), NTK_TAG('R', 't', 'l', 'R')));
            if (!us->Buffer) return STATUS_NO_MEMORY;
            us->MaximumLength = static_cast<USHORT>(length + sizeof(WCHAR));
        }
        if (length > us->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
        std::memcpy(us->Buffer, data, length);
        us->Length = static_cast<USHORT>(length >= 2 && static_cast<WCHAR*>(data)[length / 2 - 1] == 0 ? length - 2 : length);
        if (us->Length + sizeof(WCHAR) <= us->MaximumLength) us->Buffer[us->Length / 2] = 0;
        return STATUS_SUCCESS;
    }
    if (type == kRegMultiSz && !(e.flags & kQueryNoexpand)) {
        // DIRECT + multi string: the whole value as a UNICODE_STRING, like REG_SZ.
        auto* us = static_cast<PUNICODE_STRING>(e.context);
        if (!us->Buffer) {
            us->Buffer = static_cast<PWSTR>(ExAllocatePoolWithTag(PagedPool, length, NTK_TAG('R', 't', 'l', 'R')));
            if (!us->Buffer) return STATUS_NO_MEMORY;
            us->MaximumLength = static_cast<USHORT>(length);
        }
        if (length > us->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
        std::memcpy(us->Buffer, data, length);
        us->Length = static_cast<USHORT>(length);
        return STATUS_SUCCESS;
    }
    if (length <= sizeof(ULONG)) {
        std::memcpy(e.context, data, length);
        return STATUS_SUCCESS;
    }
    LONG first = *static_cast<LONG*>(e.context);
    if (first < 0) {                           // negative: -capacity, raw data copied to the start of the buffer
        if (length > static_cast<ULONG>(-first)) return STATUS_BUFFER_TOO_SMALL;
        std::memcpy(e.context, data, length);
    } else {                                   // {ULONG capacity -> length; ULONG type; data[]}
        if (length > static_cast<ULONG>(first)) return STATUS_BUFFER_TOO_SMALL;
        auto* hdr = static_cast<ULONG*>(e.context);
        hdr[0] = length;
        hdr[1] = type;
        std::memcpy(hdr + 2, data, length);
    }
    return STATUS_SUCCESS;
}

NTSTATUS DeliverValue(const QueryEntry& e, PWSTR name, ULONG type, std::vector<UCHAR>& data, PVOID context) {
    if ((e.flags & kQueryTypecheck) && IsString(e.default_type) != IsString(type) && e.default_type != type &&
        e.default_type != kRegNone)
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (type == kRegExpandSz && !(e.flags & kQueryNoexpand)) {
        std::wstring src(reinterpret_cast<const wchar_t*>(data.data()), data.size() / 2);
        while (!src.empty() && src.back() == 0) src.pop_back();
        DWORD n = ExpandEnvironmentStringsW(src.c_str(), nullptr, 0);
        std::wstring out(n ? n : 1, L'\0');
        if (n) ExpandEnvironmentStringsW(src.c_str(), out.data(), n);
        data.assign(reinterpret_cast<UCHAR*>(out.data()), reinterpret_cast<UCHAR*>(out.data()) + (n ? n * 2 : 2));
        type = kRegSz;
    }
    if (type == kRegMultiSz && !(e.flags & kQueryDirect) && !(e.flags & kQueryNoexpand)) {
        // one call per string
        const WCHAR* p = reinterpret_cast<const WCHAR*>(data.data());
        const WCHAR* end = p + data.size() / 2;
        while (p < end && *p) {
            size_t len = wcslen(p);
            NTSTATUS st = Deliver(e, name, kRegSz, const_cast<WCHAR*>(p), static_cast<ULONG>((len + 1) * 2), context);
            if (!NT_SUCCESS(st)) return st;
            p += len + 1;
        }
        return STATUS_SUCCESS;
    }
    return Deliver(e, name, type, data.data(), static_cast<ULONG>(data.size()), context);
}

NTSTATUS QueryOne(HANDLE key, const QueryEntry& e, PVOID context) {
    auto query = Nt<FnQueryValue>("NtQueryValueKey");
    if (!query) return STATUS_NOT_IMPLEMENTED;
    UNICODE_STRING name = {};
    if (e.name) {
        name.Buffer = e.name;
        name.Length = static_cast<USHORT>(wcslen(e.name) * 2);
        name.MaximumLength = name.Length + 2;
    }
    std::vector<UCHAR> buf(256);
    ULONG needed = 0;
    NTSTATUS st = query(key, &name, 2 /* KeyValuePartialInformation */, buf.data(), static_cast<ULONG>(buf.size()), &needed);
    if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL) {
        buf.resize(needed + 16);
        st = query(key, &name, 2, buf.data(), static_cast<ULONG>(buf.size()), &needed);
    }
    std::vector<UCHAR> data;
    ULONG type;
    if (NT_SUCCESS(st)) {
        auto* pi = reinterpret_cast<PartialInfo*>(buf.data());
        type = pi->Type;
        data.assign(pi->Data, pi->Data + pi->DataLength);
    } else {
        if (e.flags & kQueryRequired) return STATUS_OBJECT_NAME_NOT_FOUND;
        if (e.default_type == kRegNone) return STATUS_SUCCESS;
        type = e.default_type;
        ULONG len = e.default_length;
        if (IsString(type) && len == 0 && e.default_data) len = static_cast<ULONG>((wcslen(static_cast<PCWSTR>(e.default_data)) + 1) * 2);
        const UCHAR* p = static_cast<const UCHAR*>(e.default_data);
        data.assign(p, p + len);
    }
    NTSTATUS rc = DeliverValue(e, e.name, type, data, context);
    if (NT_SUCCESS(st) && (e.flags & kQueryDelete)) {
        if (auto del = Nt<FnDeleteValue>("NtDeleteValueKey")) del(key, &name);
    }
    return rc;
}

NTSTATUS EnumerateAll(HANDLE key, const QueryEntry& e, PVOID context) {
    auto enumerate = Nt<FnEnumValue>("NtEnumerateValueKey");
    if (!enumerate) return STATUS_NOT_IMPLEMENTED;
    struct FullInfo { ULONG TitleIndex, Type, DataOffset, DataLength, NameLength; WCHAR Name[1]; };
    for (ULONG index = 0;; ++index) {
        std::vector<UCHAR> buf(512);
        ULONG needed = 0;
        NTSTATUS st = enumerate(key, index, 1 /* KeyValueFullInformation */, buf.data(), static_cast<ULONG>(buf.size()), &needed);
        if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL) {
            buf.resize(needed + 16);
            st = enumerate(key, index, 1, buf.data(), static_cast<ULONG>(buf.size()), &needed);
        }
        if (st == STATUS_NO_MORE_ENTRIES) break;
        if (!NT_SUCCESS(st)) return st;
        auto* fi = reinterpret_cast<FullInfo*>(buf.data());
        std::wstring vname(fi->Name, fi->NameLength / 2);
        std::vector<UCHAR> data(buf.data() + fi->DataOffset, buf.data() + fi->DataOffset + fi->DataLength);
        NTSTATUS rc = DeliverValue(e, vname.data(), fi->Type, data, context);
        if (!NT_SUCCESS(rc)) return rc;
    }
    return STATUS_SUCCESS;
}

}  // namespace

NTSTATUS NTAPI RtlQueryRegistryValues(ULONG relative_to, PCWSTR path, PVOID table, PVOID context, PVOID environment) {
    (void)environment;
    HANDLE base = nullptr;
    bool base_owned = false;
    NTSTATUS st = OpenBase(relative_to, path, &base, &base_owned, false, kKeyRead);
    if (!NT_SUCCESS(st)) return st;
    HANDLE cur = base;
    bool cur_owned = false;
    st = STATUS_SUCCESS;
    for (auto* e = static_cast<QueryEntry*>(table); e->routine || e->name; ++e) {
        if (e->flags & kQueryTopkey) {
            if (cur_owned) CloseKey(cur);
            cur = base;
            cur_owned = false;
        }
        if (e->flags & kQuerySubkey) {
            if (!e->name) { st = STATUS_INVALID_PARAMETER; break; }
            std::wstring sub = e->name;
            UNICODE_STRING us = Counted(sub);
            OBJECT_ATTRIBUTES oa;
            InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, cur, nullptr);
            HANDLE next = nullptr;
            auto open = Nt<FnOpenKey>("NtOpenKey");
            NTSTATUS os = open ? open(&next, kKeyRead, &oa) : STATUS_NOT_IMPLEMENTED;
            if (!NT_SUCCESS(os)) { st = os; break; }   // a missing subkey fails the query, like the real routine
            if (cur_owned) CloseKey(cur);
            cur = next;
            cur_owned = true;
            continue;   // a SUBKEY entry only switches the current key for the entries that follow
        }
        if (!e->name) {
            if (!e->routine) continue;
            st = EnumerateAll(cur, *e, context);
        } else if (e->flags & kQueryNovalue) {
            st = e->routine ? reinterpret_cast<QueryRoutine>(e->routine)(e->name, kRegNone, nullptr, 0, context, e->context)
                            : STATUS_SUCCESS;
        } else {
            st = QueryOne(cur, *e, context);
        }
        if (!NT_SUCCESS(st)) break;
    }
    if (cur_owned) CloseKey(cur);
    if (base_owned) CloseKey(base);
    return st;
}

NTSTATUS NTAPI RtlQueryRegistryValuesEx(ULONG relative_to, PCWSTR path, PVOID table, PVOID context, PVOID environment) {
    return RtlQueryRegistryValues(relative_to, path, table, context, environment);
}

NTSTATUS NTAPI RtlWriteRegistryValue(ULONG relative_to, PCWSTR path, PCWSTR value_name, ULONG type, PVOID data, ULONG length) {
    HANDLE key = nullptr;
    bool owned = false;
    NTSTATUS st = OpenBase(relative_to, path, &key, &owned, false, kKeyWrite);
    if (!NT_SUCCESS(st)) return st;
    auto set = Nt<FnSetValue>("NtSetValueKey");
    UNICODE_STRING name = {};
    if (value_name) {
        name.Buffer = const_cast<PWSTR>(value_name);
        name.Length = static_cast<USHORT>(wcslen(value_name) * 2);
        name.MaximumLength = name.Length + 2;
    }
    st = set ? set(key, &name, 0, type, data, length) : STATUS_NOT_IMPLEMENTED;
    if (owned) CloseKey(key);
    return st;
}

NTSTATUS NTAPI RtlDeleteRegistryValue(ULONG relative_to, PCWSTR path, PCWSTR value_name) {
    HANDLE key = nullptr;
    bool owned = false;
    NTSTATUS st = OpenBase(relative_to, path, &key, &owned, false, kKeyWrite);
    if (!NT_SUCCESS(st)) return st;
    auto del = Nt<FnDeleteValue>("NtDeleteValueKey");
    UNICODE_STRING name = {};
    if (value_name) {
        name.Buffer = const_cast<PWSTR>(value_name);
        name.Length = static_cast<USHORT>(wcslen(value_name) * 2);
        name.MaximumLength = name.Length + 2;
    }
    st = del ? del(key, &name) : STATUS_NOT_IMPLEMENTED;
    if (owned) CloseKey(key);
    return st;
}

NTSTATUS NTAPI RtlCheckRegistryKey(ULONG relative_to, PCWSTR path) {
    HANDLE key = nullptr;
    bool owned = false;
    NTSTATUS st = OpenBase(relative_to, path, &key, &owned, false, kKeyRead);
    if (NT_SUCCESS(st) && owned) CloseKey(key);
    return st;
}

NTSTATUS NTAPI RtlCreateRegistryKey(ULONG relative_to, PWSTR path) {
    HANDLE key = nullptr;
    bool owned = false;
    NTSTATUS st = OpenBase(relative_to, path, &key, &owned, true, kKeyAll);
    if (NT_SUCCESS(st) && owned) CloseKey(key);
    return st;
}

}  // namespace ntk

namespace win10xbox::drivers::detail {
namespace {
const Export kRegTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
    X(RtlQueryRegistryValues), X(RtlQueryRegistryValuesEx), X(RtlWriteRegistryValue), X(RtlDeleteRegistryValue),
    X(RtlCheckRegistryKey), X(RtlCreateRegistryKey),
#undef X
};
}  // namespace
ExportGroup RegistryExports() { return {kRegTable, sizeof(kRegTable) / sizeof(kRegTable[0])}; }
}  // namespace win10xbox::drivers::detail
