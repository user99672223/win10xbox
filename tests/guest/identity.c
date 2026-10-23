/* guest_identity: the machine identity (DECISIONS.md D18) read through every path a program has, and compared.
 *
 * Prints every value on its own line as `path=value`, then `identity=consistent` and returns 42 only when all of them
 * agree: the CPU brand string (CPUID leaves 0x80000002..4, the registry's ProcessorNameString, the SMBIOS type 4
 * ProcessorVersion, WMI Win32_Processor.Name), the family/model/stepping (CPUID leaf 1, the registry's Identifier,
 * the SMBIOS ProcessorId, GetNativeSystemInfo), the OS version (RtlGetVersion, GetVersionExW, VerifyVersionInfoW,
 * RtlGetNtVersionNumbers, the registry's CurrentVersion key, KUSER_SHARED_DATA) and the CPU's own claims (hypervisor
 * leaf 0x40000000 all zeros, CPUID.1:ECX[31] clear, RDRAND advertised and returning a value).
 *
 * Plain C for MSVC and mingw. WMI is required, except when the host sets WIN10XBOX_TEST_NO_WMI=1 (a host that cannot
 * run Wine's wbemprox); the line `wmi.status=` says which. Run through the layer; on a bare machine it fails by design. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wbemidl.h>
#include <intrin.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_BRAND "AMD 4700S 8-Core Processor"
#define EXPECT_FAMILY 23u
#define EXPECT_MODEL 136u
#define EXPECT_STEPPING 1u
#define EXPECT_MAJOR 10u
#define EXPECT_MINOR 0u
#define EXPECT_BUILD 19045u
#define EXPECT_UBR 3803u

static int g_fail;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        g_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: \"%s\" != \"%s\"\n", what, got, want);
        g_fail++;
    }
}

static void check_uint(const char *what, unsigned long got, unsigned long want) {
    if (got != want) {
        printf("FAIL %s: %lu != %lu\n", what, got, want);
        g_fail++;
    }
}

static void narrow(const wchar_t *w, char *out, size_t cap) {
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    out[i] = 0;
}

/* family/model/stepping from a CPUID.1 EAX signature */
static void decode_signature(unsigned eax, unsigned *family, unsigned *model, unsigned *stepping) {
    unsigned base_family = (eax >> 8) & 0xF;
    *family = base_family + (base_family == 0xF ? (eax >> 20) & 0xFF : 0);
    *model = ((eax >> 4) & 0xF) | (base_family >= 6 ? ((eax >> 16) & 0xF) << 4 : 0);
    *stepping = eax & 0xF;
}

static void check_fms(const char *path, unsigned f, unsigned m, unsigned s) {
    char what[64];
    printf("%s.family=%u\n%s.model=%u\n%s.stepping=%u\n", path, f, path, m, path, s);
    _snprintf(what, sizeof what, "%s.family", path);
    check_uint(what, f, EXPECT_FAMILY);
    _snprintf(what, sizeof what, "%s.model", path);
    check_uint(what, m, EXPECT_MODEL);
    _snprintf(what, sizeof what, "%s.stepping", path);
    check_uint(what, s, EXPECT_STEPPING);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
}

/* ---- CPUID ---------------------------------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("rdrnd")))
#endif
static int rdrand_once(void) {
    unsigned v = 0;
    return _rdrand32_step(&v);
}

static void test_cpuid(void) {
    int r[4];
    char vendor[13], brand[49];
    unsigned family, model, stepping, i, leaf1_eax;

    __cpuid(r, 0);
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    printf("cpuid.vendor=%s\n", vendor);
    check_str("cpuid.vendor", vendor, "AuthenticAMD");

    __cpuid(r, 1);
    leaf1_eax = (unsigned)r[0];
    printf("cpuid.1.eax=%08x\ncpuid.1.ecx=%08x\n", (unsigned)r[0], (unsigned)r[2]);
    decode_signature((unsigned)r[0], &family, &model, &stepping);
    check_fms("cpuid", family, model, stepping);
    printf("cpuid.hypervisor_bit=%u\ncpuid.rdrand_bit=%u\n", ((unsigned)r[2] >> 31) & 1, ((unsigned)r[2] >> 30) & 1);
    check(((unsigned)r[2] >> 31) == 0, "CPUID.1:ECX[31] (hypervisor present) is clear");
    check(((unsigned)r[2] >> 30) & 1, "CPUID.1:ECX[30] (RDRAND) is set");

    memset(brand, 0, sizeof brand);
    for (i = 0; i < 3; i++) {
        __cpuid(r, (int)(0x80000002u + i));
        memcpy(brand + 16 * i, r, 16);
    }
    trim(brand);
    printf("cpuid.brand=%s\n", brand);
    check_str("cpuid.brand", brand, EXPECT_BRAND);

    __cpuid(r, (int)0x80000001u);
    printf("cpuid.80000001.eax=%08x\n", (unsigned)r[0]);
    check_uint("cpuid.80000001.eax equals cpuid.1.eax", (unsigned)r[0], leaf1_eax);

    __cpuid(r, 0x40000000);
    printf("cpuid.40000000=%08x %08x %08x %08x\n", (unsigned)r[0], (unsigned)r[1], (unsigned)r[2], (unsigned)r[3]);
    check(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0, "hypervisor leaf 0x40000000 is all zeros");

    {
        int ok = 0;
        for (i = 0; i < 16; i++) ok += rdrand_once() ? 1 : 0;
        printf("rdrand.success=%d/16\n", ok);
        check(ok > 0, "RDRAND returned a value at least once in 16 tries");
    }
}

/* ---- the registry ----------------------------------------------------------------------------------------------- */
static int reg_str(HKEY root, const wchar_t *path, const wchar_t *name, char *out, size_t cap) {
    HKEY k;
    wchar_t buf[256];
    DWORD type = 0, size = sizeof buf - sizeof(wchar_t);
    LONG rc;
    out[0] = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    memset(buf, 0, sizeof buf);
    rc = RegQueryValueExW(k, name, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return 0;
    narrow(buf, out, cap);
    return 1;
}

static int reg_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *out) {
    HKEY k;
    DWORD type = 0, size = sizeof *out;
    LONG rc;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    rc = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

static void test_registry(void) {
    static const wchar_t cpu0[] = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    char brand[128], ident[128], vendor[64];
    unsigned f = 0, m = 0, s = 0;
    DWORD mhz = 0;

    check(reg_str(HKEY_LOCAL_MACHINE, cpu0, L"ProcessorNameString", brand, sizeof brand), "registry ProcessorNameString");
    trim(brand);
    printf("registry.brand=%s\n", brand);
    check_str("registry.brand", brand, EXPECT_BRAND);

    check(reg_str(HKEY_LOCAL_MACHINE, cpu0, L"Identifier", ident, sizeof ident), "registry Identifier");
    printf("registry.identifier=%s\n", ident);
    check(sscanf(ident, "AMD64 Family %u Model %u Stepping %u", &f, &m, &s) == 3, "registry Identifier parses");
    check_fms("registry", f, m, s);

    check(reg_str(HKEY_LOCAL_MACHINE, cpu0, L"VendorIdentifier", vendor, sizeof vendor), "registry VendorIdentifier");
    printf("registry.vendor=%s\n", vendor);
    check_str("registry.vendor", vendor, "AuthenticAMD");

    printf("registry.mhz=%lu\n", reg_dword(HKEY_LOCAL_MACHINE, cpu0, L"~MHz", &mhz) ? (unsigned long)mhz : 0ul);
    check(mhz != 0, "registry ~MHz");
}

/* ---- SMBIOS via GetSystemFirmwareTable('RSMB') --------------------------------------------------------------------- */
#define RSMB_SIG (((DWORD)'R' << 24) | ((DWORD)'S' << 16) | ((DWORD)'M' << 8) | (DWORD)'B')

static char g_smbios_brand[128];
static unsigned g_smbios_cores, g_smbios_threads;

/* the string with 1-based index `idx` of the structure whose formatted area starts at p (length len) */
static const char *smbios_string(const unsigned char *p, unsigned len, const unsigned char *end, unsigned idx) {
    const char *s = (const char *)p + len;
    unsigned i;
    if (!idx) return "";
    for (i = 1; i < idx; i++) {
        if (s >= (const char *)end) return "";
        s += strlen(s) + 1;
    }
    return s < (const char *)end ? s : "";
}

static void test_smbios(void) {
    UINT need = GetSystemFirmwareTable(RSMB_SIG, 0, NULL, 0), got;
    unsigned char *buf, *p, *end, *type4 = NULL;
    unsigned long table_len;
    unsigned seen_bios = 0, seen_system = 0, seen_end = 0, major, minor;
    unsigned f = 0, m = 0, s = 0;
    unsigned long long id;

    printf("smbios.size=%u\n", need);
    check(need > 8, "GetSystemFirmwareTable('RSMB') returns a table");
    if (need <= 8) return;
    buf = (unsigned char *)malloc(need);
    got = GetSystemFirmwareTable(RSMB_SIG, 0, buf, need);
    check_uint("GetSystemFirmwareTable second call", got, need);
    major = buf[1];
    minor = buf[2];
    memcpy(&table_len, buf + 4, 4);
    table_len &= 0xFFFFFFFFul;
    printf("smbios.version=%u.%u\nsmbios.length=%lu\n", major, minor, table_len);
    check(table_len + 8 == need, "SMBIOS length field matches the buffer");
    check(major >= 2, "SMBIOS major version");

    p = buf + 8;
    end = p + (table_len < need - 8 ? table_len : need - 8);
    while (p + 4 <= end) {
        unsigned char type = p[0], len = p[1];
        unsigned char *next;
        if (len < 4 || p + len > end) {
            check(0, "SMBIOS structure is well formed");
            break;
        }
        next = p + len;
        while (next + 1 < end && (next[0] || next[1])) next++;   /* strings end at a double NUL */
        next += 2;
        if (type == 0) seen_bios++;
        if (type == 1) seen_system++;
        if (type == 4 && !type4) type4 = p;
        if (type == 127) { seen_end++; break; }
        p = next;
    }
    printf("smbios.types=bios:%u system:%u processor:%u end:%u\n", seen_bios, seen_system, type4 ? 1u : 0u, seen_end);
    check(seen_bios == 1 && seen_system == 1 && seen_end == 1, "SMBIOS has type 0, type 1 and the end-of-table structure");
    check(type4 != NULL, "SMBIOS has a type 4 (processor) structure");
    if (!type4) { free(buf); return; }

    {
        const unsigned char *end4 = end;
        const char *ver = smbios_string(type4, type4[1], end4, type4[0x10]);
        const char *vendor = smbios_string(type4, type4[1], end4, type4[7]);
        unsigned eax;
        memcpy(&id, type4 + 8, 8);
        eax = (unsigned)(id & 0xFFFFFFFFu);
        strncpy(g_smbios_brand, ver, sizeof g_smbios_brand - 1);
        trim(g_smbios_brand);
        printf("smbios.brand=%s\nsmbios.vendor=%s\nsmbios.processor_id=%016llx\n", g_smbios_brand, vendor, id);
        check_str("smbios.brand", g_smbios_brand, EXPECT_BRAND);
        check_str("smbios.vendor", vendor, "AuthenticAMD");
        decode_signature(eax, &f, &m, &s);
        check_fms("smbios", f, m, s);
        {
            int r[4];
            __cpuid(r, 1);
            check_uint("smbios ProcessorId EAX equals CPUID.1:EAX", eax, (unsigned)r[0]);
            check_uint("smbios ProcessorId EDX equals CPUID.1:EDX", (unsigned long)(id >> 32), (unsigned)r[3]);
        }
        if (type4[1] >= 0x30) {
            g_smbios_cores = type4[0x2A] | (type4[0x2B] << 8);
            g_smbios_threads = type4[0x2E] | (type4[0x2F] << 8);
        } else {
            g_smbios_cores = type4[0x23];
            g_smbios_threads = type4[0x25];
        }
        printf("smbios.cores=%u\nsmbios.threads=%u\nsmbios.family_code=%u\n", g_smbios_cores, g_smbios_threads, type4[6]);
        check(g_smbios_cores > 0 && g_smbios_threads >= g_smbios_cores, "SMBIOS core and thread counts");
    }
    free(buf);
}

/* ---- WMI ----------------------------------------------------------------------------------------------------------- */
static const GUID kClsidWbemLocator = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const GUID kIidIWbemLocator = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

static int wmi_required(void) {
    char v[8];
    DWORD n = GetEnvironmentVariableA("WIN10XBOX_TEST_NO_WMI", v, sizeof v);
    return !(n > 0 && n < sizeof v && v[0] == '1');
}

static void test_wmi(void) {
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    ULONG ret = 0;
    HRESULT hr;
    BSTR ns, lang, query;
    VARIANT v;
    char name[128], mfr[128];
    int cores = -1;
    const int required = wmi_required();

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("wmi.CoInitializeEx=%08lx\n", (unsigned long)hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) goto unavailable;
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL,
                              EOAC_NONE, NULL);
    printf("wmi.CoInitializeSecurity=%08lx\n", (unsigned long)hr);   /* RPC_E_TOO_LATE is fine; not checked */

    hr = CoCreateInstance(&kClsidWbemLocator, NULL, CLSCTX_INPROC_SERVER, &kIidIWbemLocator, (void **)&loc);
    printf("wmi.CoCreateInstance=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) goto unavailable;

    ns = SysAllocString(L"ROOT\\CIMV2");
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    printf("wmi.ConnectServer=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) goto fail;
    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);   /* not a proxy in process: result ignored */

    lang = SysAllocString(L"WQL");
    query = SysAllocString(L"SELECT Name, Manufacturer, NumberOfCores FROM Win32_Processor");
    hr = IWbemServices_ExecQuery(svc, lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(lang);
    SysFreeString(query);
    printf("wmi.ExecQuery=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) goto fail;
    hr = IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret);
    printf("wmi.Next=%08lx count=%lu\n", (unsigned long)hr, (unsigned long)ret);
    if (FAILED(hr) || ret != 1) goto fail;

    name[0] = mfr[0] = 0;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"Name", 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal) narrow(v.bstrVal, name, sizeof name);
    VariantClear(&v);
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"Manufacturer", 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal) narrow(v.bstrVal, mfr, sizeof mfr);
    VariantClear(&v);
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"NumberOfCores", 0, &v, NULL, NULL)) && (v.vt == VT_I4 || v.vt == VT_UI4)) cores = (int)v.lVal;
    VariantClear(&v);
    trim(name);
    printf("wmi.status=available\nwmi.name=%s\nwmi.manufacturer=%s\nwmi.cores=%d\n", name, mfr, cores);
    check_str("wmi.name", name, EXPECT_BRAND);
    check_str("wmi.manufacturer", mfr, "AuthenticAMD");
    check(cores > 0 && (unsigned)cores == g_smbios_cores, "wmi.cores equals the SMBIOS core count");
    IWbemClassObject_Release(obj);
    IEnumWbemClassObject_Release(en);
    IWbemServices_Release(svc);
    IWbemLocator_Release(loc);
    return;

fail:
    printf("wmi.status=query-failed\n");
    check(0, "WMI Win32_Processor query");
    return;
unavailable:
    if (required) {
        printf("wmi.status=failed\n");
        check(0, "WMI is available (CoCreateInstance of WbemLocator)");
    } else {
        printf("wmi.status=unavailable (WIN10XBOX_TEST_NO_WMI=1)\n");
    }
}

/* ---- the version --------------------------------------------------------------------------------------------------- */
typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW *);
typedef void (WINAPI *RtlGetNtVersionNumbersFn)(DWORD *, DWORD *, DWORD *);
typedef BOOL (WINAPI *GetVersionExWFn)(OSVERSIONINFOW *);
typedef BOOL (WINAPI *VerifyVersionInfoWFn)(OSVERSIONINFOEXW *, DWORD, DWORDLONG);
typedef ULONGLONG (WINAPI *VerSetConditionMaskFn)(ULONGLONG, DWORD, BYTE);

static void version_line(const char *path, unsigned major, unsigned minor, unsigned build) {
    char what[64];
    printf("version.%s=%u.%u.%u\n", path, major, minor, build);
    _snprintf(what, sizeof what, "version.%s major", path);
    check_uint(what, major, EXPECT_MAJOR);
    _snprintf(what, sizeof what, "version.%s minor", path);
    check_uint(what, minor, EXPECT_MINOR);
    _snprintf(what, sizeof what, "version.%s build", path);
    check_uint(what, build, EXPECT_BUILD);
}

/* KUSER_SHARED_DATA fields, read at their fixed addresses: the compiler emits mov reg,[abs] or mov reg,imm; mov reg,[reg] */
#define KUSER_U32(off) (*(volatile const unsigned int *)((ULONG_PTR)0x7FFE0000u + (off)))

static void test_version(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    static const wchar_t cv[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    OSVERSIONINFOEXW ver;
    char buf[64];
    DWORD d, maj = 0, min = 0, bld = 0;
    unsigned ku_build, ku_major, ku_minor;
    RtlGetVersionFn rtl_get_version = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
    RtlGetNtVersionNumbersFn nt_numbers = (RtlGetNtVersionNumbersFn)GetProcAddress(ntdll, "RtlGetNtVersionNumbers");
    GetVersionExWFn get_version_ex = (GetVersionExWFn)GetProcAddress(k32, "GetVersionExW");
    VerifyVersionInfoWFn verify = (VerifyVersionInfoWFn)GetProcAddress(k32, "VerifyVersionInfoW");
    VerSetConditionMaskFn set_mask = (VerSetConditionMaskFn)GetProcAddress(k32, "VerSetConditionMask");

    check(rtl_get_version != NULL, "ntdll!RtlGetVersion");
    if (rtl_get_version) {
        memset(&ver, 0, sizeof ver);
        ver.dwOSVersionInfoSize = sizeof ver;
        rtl_get_version(&ver);
        version_line("RtlGetVersion", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber);
        printf("version.RtlGetVersion.platform=%lu\nversion.RtlGetVersion.product_type=%u\n", (unsigned long)ver.dwPlatformId,
               (unsigned)ver.wProductType);
        check_uint("platform id (VER_PLATFORM_WIN32_NT)", ver.dwPlatformId, 2);
        check_uint("product type (VER_NT_WORKSTATION)", ver.wProductType, 1);
    }

    if (get_version_ex) {
        OSVERSIONINFOW v2;
        memset(&v2, 0, sizeof v2);
        v2.dwOSVersionInfoSize = sizeof v2;
        check(get_version_ex(&v2), "GetVersionExW");
        version_line("GetVersionExW", v2.dwMajorVersion, v2.dwMinorVersion, v2.dwBuildNumber);
    }

    if (nt_numbers) {
        nt_numbers(&maj, &min, &bld);
        version_line("RtlGetNtVersionNumbers", maj, min, bld & 0x0FFFFFFFu);
    }

    if (verify && set_mask) {
        OSVERSIONINFOEXW want;
        ULONGLONG mask = 0;
        BOOL eq, newer;
        memset(&want, 0, sizeof want);
        want.dwOSVersionInfoSize = sizeof want;
        want.dwMajorVersion = EXPECT_MAJOR;
        want.dwMinorVersion = EXPECT_MINOR;
        want.dwBuildNumber = EXPECT_BUILD;
        mask = set_mask(mask, VER_MAJORVERSION, VER_EQUAL);
        mask = set_mask(mask, VER_MINORVERSION, VER_EQUAL);
        mask = set_mask(mask, VER_BUILDNUMBER, VER_EQUAL);
        eq = verify(&want, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask);
        want.dwBuildNumber = EXPECT_BUILD + 1;
        newer = verify(&want, VER_BUILDNUMBER, set_mask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL));
        printf("version.VerifyVersionInfoW.equal=%d\nversion.VerifyVersionInfoW.build_plus_one_ge=%d\n", eq ? 1 : 0, newer ? 1 : 0);
        check(eq, "VerifyVersionInfoW says exactly 10.0.19045");
        check(!newer, "VerifyVersionInfoW says the build is below 19046");
    }

    if (reg_str(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild", buf, sizeof buf)) {
        printf("version.registry.CurrentBuild=%s\n", buf);
        check_uint("registry CurrentBuild", strtoul(buf, NULL, 10), EXPECT_BUILD);
    } else {
        check(0, "registry CurrentBuild");
    }
    if (reg_str(HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber", buf, sizeof buf))
        check_uint("registry CurrentBuildNumber", strtoul(buf, NULL, 10), EXPECT_BUILD);
    if (reg_dword(HKEY_LOCAL_MACHINE, cv, L"CurrentMajorVersionNumber", &d)) {
        printf("version.registry.CurrentMajorVersionNumber=%lu\n", (unsigned long)d);
        check_uint("registry CurrentMajorVersionNumber", d, EXPECT_MAJOR);
    } else {
        check(0, "registry CurrentMajorVersionNumber");
    }
    if (reg_dword(HKEY_LOCAL_MACHINE, cv, L"CurrentMinorVersionNumber", &d)) {
        printf("version.registry.CurrentMinorVersionNumber=%lu\n", (unsigned long)d);
        check_uint("registry CurrentMinorVersionNumber", d, EXPECT_MINOR);
    } else {
        check(0, "registry CurrentMinorVersionNumber");
    }
    if (reg_dword(HKEY_LOCAL_MACHINE, cv, L"UBR", &d)) {
        printf("version.registry.UBR=%lu\n", (unsigned long)d);
        check_uint("registry UBR", d, EXPECT_UBR);
    } else {
        check(0, "registry UBR");
    }
    if (reg_str(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", buf, sizeof buf)) {
        printf("version.registry.DisplayVersion=%s\n", buf);
        check_str("registry DisplayVersion", buf, "22H2");
    } else {
        check(0, "registry DisplayVersion");
    }

    ku_build = KUSER_U32(0x260);
    ku_major = KUSER_U32(0x26C);
    ku_minor = KUSER_U32(0x270);
    version_line("KUSER_SHARED_DATA", ku_major, ku_minor, ku_build);
    printf("version.expected=%u.%u.%u.%u\n", EXPECT_MAJOR, EXPECT_MINOR, EXPECT_BUILD, EXPECT_UBR);
}

/* ---- GetNativeSystemInfo ---------------------------------------------------------------------------------------- */
static void test_system_info(void) {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    printf("sysinfo.level=%u\nsysinfo.revision=%04x\n", (unsigned)si.wProcessorLevel, (unsigned)si.wProcessorRevision);
    check_uint("GetNativeSystemInfo wProcessorLevel", si.wProcessorLevel, EXPECT_FAMILY);
    check_uint("GetNativeSystemInfo wProcessorRevision", si.wProcessorRevision, (EXPECT_MODEL << 8) | EXPECT_STEPPING);
    check_uint("GetNativeSystemInfo wProcessorArchitecture (AMD64)", si.wProcessorArchitecture, PROCESSOR_ARCHITECTURE_AMD64);
}

int main(void) {
    test_cpuid();
    test_registry();
    test_smbios();
    test_wmi();
    test_version();
    test_system_info();
    if (g_fail) {
        printf("identity=MISMATCH (%d checks failed)\n", g_fail);
        fflush(stdout);
        return 1;
    }
    printf("identity=consistent\n");
    fflush(stdout);
    return 42;
}
