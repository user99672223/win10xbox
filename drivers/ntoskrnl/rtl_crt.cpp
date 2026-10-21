// ntoskrnl Rtl*, C runtime and Dbg* surface. MIT.
//
// Rtl functions that are pure (strings, bitmaps, SIDs, time conversion,
// unwind) are forwarded to the host ntdll by name: the allow-list below.
// Anything that would reach the kernel from inside ntdll (registry, files)
// is implemented here on top of the dispatcher instead, so it sees the
// emulated registry and VFS.
#include "internal.h"
#include "win10xbox/config.h"
#undef RtlCopyMemory
#undef RtlMoveMemory
#undef RtlFillMemory
#undef RtlZeroMemory
#undef RtlSecureZeroMemory
#undef RtlCompareMemory
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace win10xbox::drivers::detail {

// Rtl* names forwarded to the host ntdll export of the same name.
extern const char* const kHostForwards[] = {
    "RtlAnsiStringToUnicodeString", "RtlUnicodeStringToAnsiString", "RtlFreeUnicodeString", "RtlFreeAnsiString",
    "RtlCompareUnicodeString", "RtlEqualUnicodeString", "RtlPrefixUnicodeString", "RtlUpcaseUnicodeString",
    "RtlDowncaseUnicodeString", "RtlUpcaseUnicodeChar", "RtlUnicodeStringToInteger", "RtlCharToInteger",
    "RtlIntegerToUnicodeString", "RtlInt64ToUnicodeString", "RtlUnicodeToMultiByteN", "RtlMultiByteToUnicodeN",
    "RtlUnicodeToMultiByteSize", "RtlMultiByteToUnicodeSize", "RtlUnicodeToUTF8N", "RtlUTF8ToUnicodeN",
    "RtlAppendStringToString", "RtlEqualString", "RtlCompareString", "RtlPrefixString", "RtlHashUnicodeString",
    "RtlCopyString", "RtlUpperString", "RtlUpcaseUnicodeStringToAnsiString", "RtlUpcaseUnicodeToMultiByteN",
    "RtlOemStringToUnicodeString", "RtlUnicodeStringToOemString", "RtlxAnsiStringToUnicodeSize",
    "RtlCompareMemoryUlong", "RtlFillMemoryUlong", "RtlFillMemoryUlonglong",
    "RtlTimeToTimeFields", "RtlTimeFieldsToTime", "RtlTimeToSecondsSince1970", "RtlSecondsSince1970ToTime",
    "RtlTimeToSecondsSince1980", "RtlSecondsSince1980ToTime",
    "RtlLengthSid", "RtlEqualSid", "RtlCopySid", "RtlInitializeSid", "RtlSubAuthoritySid", "RtlSubAuthorityCountSid",
    "RtlIdentifierAuthoritySid", "RtlLengthRequiredSid", "RtlValidSid", "RtlConvertSidToUnicodeString",
    "RtlCreateAcl", "RtlAddAccessAllowedAce", "RtlAddAccessAllowedAceEx", "RtlAddAce", "RtlGetAce", "RtlValidAcl",
    "RtlCreateSecurityDescriptor", "RtlSetDaclSecurityDescriptor", "RtlGetDaclSecurityDescriptor",
    "RtlSetOwnerSecurityDescriptor", "RtlSetGroupSecurityDescriptor", "RtlValidSecurityDescriptor",
    "RtlLengthSecurityDescriptor", "RtlAbsoluteToSelfRelativeSD", "RtlSelfRelativeToAbsoluteSD",
    "RtlInitializeBitMap", "RtlClearBit", "RtlSetBit", "RtlTestBit", "RtlClearAllBits", "RtlSetAllBits",
    "RtlFindClearBits", "RtlFindSetBits", "RtlFindClearBitsAndSet", "RtlFindSetBitsAndClear", "RtlClearBits",
    "RtlSetBits", "RtlNumberOfSetBits", "RtlNumberOfClearBits", "RtlFindFirstRunClear", "RtlFindNextForwardRunClear",
    "RtlFindLongestRunClear", "RtlAreBitsClear", "RtlAreBitsSet", "RtlFindClearRuns", "RtlFindSetBitsAndClear",
    "RtlRandom", "RtlRandomEx", "RtlUniform",
    "RtlGetCompressionWorkSpaceSize", "RtlCompressBuffer", "RtlDecompressBuffer", "RtlDecompressBufferEx",
    "RtlDecompressFragment", "RtlComputeCrc32", "RtlImageNtHeader", "RtlImageNtHeaderEx",
    "RtlImageDirectoryEntryToData", "RtlPcToFileHeader",
    "RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlUnwindEx", "RtlUnwind", "RtlCaptureContext",
    "RtlCaptureStackBackTrace", "RtlRaiseException", "RtlRaiseStatus",
    "RtlInitializeGenericTable", "RtlInsertElementGenericTable", "RtlLookupElementGenericTable",
    "RtlDeleteElementGenericTable", "RtlEnumerateGenericTable", "RtlNumberGenericTableElements",
    "RtlGetElementGenericTable", "RtlIsGenericTableEmpty", "RtlInitializeGenericTableAvl",
    "RtlInsertElementGenericTableAvl", "RtlLookupElementGenericTableAvl", "RtlDeleteElementGenericTableAvl",
    "RtlEnumerateGenericTableAvl", "RtlNumberGenericTableElementsAvl", "RtlGetElementGenericTableAvl",
    "RtlIsGenericTableEmptyAvl", "RtlEnumerateGenericTableWithoutSplayingAvl", "RtlLookupFirstMatchingElementGenericTableAvl",
    "RtlGUIDFromString", "RtlStringFromGUID", "RtlInitializeSListHead", "RtlQueryDepthSList",
    "RtlInterlockedPushEntrySList", "RtlInterlockedPopEntrySList", "RtlInterlockedFlushSList",
    "RtlSetBit", "RtlxUnicodeStringToAnsiSize", "RtlIsNameLegalDOS8Dot3", "RtlIsDosDeviceName_Ustr",
    "RtlNtStatusToDosError", "RtlNtStatusToDosErrorNoTeb", "RtlVerifyVersionInfo", "RtlLengthRequiredSid",
    "__C_specific_handler", "_local_unwind",
    nullptr,
};

// kernel export name -> host ntdll export name (different spelling, same signature).
extern const char* const kHostAliases[][2] = {
    {"ExpInterlockedPushEntrySList", "RtlInterlockedPushEntrySList"},
    {"ExpInterlockedPopEntrySList", "RtlInterlockedPopEntrySList"},
    {"ExpInterlockedFlushSList", "RtlInterlockedFlushSList"},
    {"ExQueryDepthSList", "RtlQueryDepthSList"},
    {"ExInterlockedPushEntrySList", "RtlInterlockedPushEntrySList"},
    {"ExInterlockedPopEntrySList", "RtlInterlockedPopEntrySList"},
    {"ExInterlockedFlushSList", "RtlInterlockedFlushSList"},
    {nullptr, nullptr},
};

}  // namespace win10xbox::drivers::detail

namespace ntk {
using namespace win10xbox::drivers::detail;

// ---------------------------------------------------------------------------------------------
// printf engine: MS semantics, plus the kernel extensions %wZ (UNICODE_STRING*) and %Z (ANSI_STRING*).
// ---------------------------------------------------------------------------------------------
namespace {

template <class C>
struct Out {
    C* buf;
    size_t cap;      // total capacity in characters (0 = count only)
    size_t pos = 0;  // characters produced (even beyond cap)
    void put(C c) {
        if (buf && pos < cap) buf[pos] = c;
        ++pos;
    }
    void fill(C c, int n) { for (int i = 0; i < n; ++i) put(c); }
};

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = 0, prec = -1;
    int size = 0;   // 0 int, 1 long, 2 long long, -1 short, -2 char, 3 wide-char-string marker (l on c/s), 4 narrow marker (h on c/s)
};

template <class C>
void EmitText(Out<C>& o, const Spec& sp, const C* s, size_t n) {
    int pad = sp.width > static_cast<int>(n) ? sp.width - static_cast<int>(n) : 0;
    if (!sp.left) o.fill(static_cast<C>(' '), pad);
    for (size_t i = 0; i < n; ++i) o.put(s[i]);
    if (sp.left) o.fill(static_cast<C>(' '), pad);
}

template <class C>
void EmitNumber(Out<C>& o, const Spec& sp, unsigned long long v, bool neg, int base, bool upper, const char* prefix) {
    char tmp[72];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    int zeros = sp.prec > n ? sp.prec - n : 0;
    std::string sign;
    if (neg) sign = "-";
    else if (sp.plus) sign = "+";
    else if (sp.space) sign = " ";
    std::string pre = prefix ? prefix : "";
    int total = static_cast<int>(sign.size() + pre.size()) + zeros + n;
    int pad = sp.width > total ? sp.width - total : 0;
    if (sp.zero && !sp.left && sp.prec < 0) { zeros += pad; pad = 0; }
    if (!sp.left) o.fill(static_cast<C>(' '), pad);
    for (char c : sign) o.put(static_cast<C>(c));
    for (char c : pre) o.put(static_cast<C>(c));
    o.fill(static_cast<C>('0'), zeros);
    while (n) o.put(static_cast<C>(tmp[--n]));
    if (sp.left) o.fill(static_cast<C>(' '), pad);
}

template <class C>
size_t Format(C* buf, size_t cap, const C* fmt, va_list ap) {
    Out<C> o{buf, cap};
    constexpr bool kWide = sizeof(C) == 2;
    for (const C* p = fmt; *p;) {
        if (*p != static_cast<C>('%')) { o.put(*p++); continue; }
        ++p;
        Spec sp;
        for (;; ++p) {
            if (*p == '-') sp.left = true;
            else if (*p == '+') sp.plus = true;
            else if (*p == ' ') sp.space = true;
            else if (*p == '#') sp.alt = true;
            else if (*p == '0') sp.zero = true;
            else break;
        }
        if (*p == '*') { sp.width = va_arg(ap, int); if (sp.width < 0) { sp.left = true; sp.width = -sp.width; } ++p; }
        else while (*p >= '0' && *p <= '9') sp.width = sp.width * 10 + (*p++ - '0');
        if (*p == '.') {
            ++p;
            sp.prec = 0;
            if (*p == '*') { sp.prec = va_arg(ap, int); ++p; }
            else while (*p >= '0' && *p <= '9') sp.prec = sp.prec * 10 + (*p++ - '0');
        }
        // length
        bool is_w = false, is_h = false;
        for (;;) {
            if (*p == 'l') { if (p[1] == 'l') { sp.size = 2; p += 2; } else { sp.size = 1; ++p; is_w = true; } }
            else if (*p == 'h') { if (p[1] == 'h') { sp.size = -2; p += 2; } else { sp.size = -1; ++p; is_h = true; } }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { sp.size = 2; p += 3; }
            else if (*p == 'I' && p[1] == '3' && p[2] == '2') { sp.size = 0; p += 3; }
            else if (*p == 'I') { sp.size = 2; ++p; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { sp.size = 2; ++p; }
            else if (*p == 'w') { is_w = true; ++p; if (*p == 'Z') break; }
            else if (*p == 'L') { sp.size = 2; ++p; }
            else break;
        }
        const C conv = *p;
        if (!conv) break;
        ++p;
        switch (conv) {
        case '%': o.put(static_cast<C>('%')); break;
        case 'd': case 'i': {
            long long v = sp.size == 2 ? va_arg(ap, long long) : sp.size == 1 ? va_arg(ap, long)
                        : va_arg(ap, int);
            if (sp.size == -1) v = static_cast<short>(v);
            if (sp.size == -2) v = static_cast<signed char>(v);
            EmitNumber(o, sp, v < 0 ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v), v < 0, 10, false, nullptr);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = sp.size == 2 ? va_arg(ap, unsigned long long) : sp.size == 1 ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            if (sp.size == -1) v = static_cast<unsigned short>(v);
            if (sp.size == -2) v = static_cast<unsigned char>(v);
            const int base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            const char* prefix = (sp.alt && v && base == 16) ? (conv == 'X' ? "0X" : "0x") : (sp.alt && base == 8 ? "0" : nullptr);
            EmitNumber(o, sp, v, false, base, conv == 'X', prefix);
            break;
        }
        case 'p': {
            void* v = va_arg(ap, void*);
            Spec ps = sp;
            ps.prec = 16;
            EmitNumber(o, ps, reinterpret_cast<unsigned long long>(v), false, 16, true, nullptr);
            break;
        }
        case 'c': case 'C': {
            int v = va_arg(ap, int);
            C ch = static_cast<C>(v);
            EmitText(o, sp, &ch, 1);
            break;
        }
        case 's': case 'S': {
            // %s: native width string, %S the other one; l/w force wide, h forces narrow.
            bool wide_arg = (conv == 's') ? kWide : !kWide;
            if (is_w) wide_arg = true;
            if (is_h) wide_arg = false;
            if (wide_arg) {
                const wchar_t* s = va_arg(ap, const wchar_t*);
                if (!s) s = L"(null)";
                size_t n = 0;
                while (s[n] && (sp.prec < 0 || static_cast<int>(n) < sp.prec)) ++n;
                std::basic_string<C> tmp(n, C(0));
                for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<C>(s[i]);
                EmitText(o, sp, tmp.data(), n);
            } else {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t n = 0;
                while (s[n] && (sp.prec < 0 || static_cast<int>(n) < sp.prec)) ++n;
                std::basic_string<C> tmp(n, C(0));
                for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<C>(static_cast<unsigned char>(s[i]));
                EmitText(o, sp, tmp.data(), n);
            }
            break;
        }
        case 'Z': {
            if (is_w) {   // %wZ
                auto* us = va_arg(ap, PUNICODE_STRING);
                size_t n = us && us->Buffer ? us->Length / 2 : 0;
                std::basic_string<C> tmp(n, C(0));
                for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<C>(us->Buffer[i]);
                EmitText(o, sp, tmp.data(), n);
            } else {
                auto* as = va_arg(ap, PANSI_STRING);
                size_t n = as && as->Buffer ? as->Length : 0;
                std::basic_string<C> tmp(n, C(0));
                for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<C>(static_cast<unsigned char>(as->Buffer[i]));
                EmitText(o, sp, tmp.data(), n);
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'F': case 'a': case 'A': {
            double v = va_arg(ap, double);
            char spec[16];
            int k = 0;
            spec[k++] = '%';
            if (sp.left) spec[k++] = '-';
            if (sp.plus) spec[k++] = '+';
            if (sp.zero) spec[k++] = '0';
            if (sp.alt) spec[k++] = '#';
            spec[k++] = '*'; spec[k++] = '.'; spec[k++] = '*';
            spec[k++] = static_cast<char>(conv);
            spec[k] = 0;
            char tmp[400];
            int n = std::snprintf(tmp, sizeof tmp, spec, sp.width, sp.prec < 0 ? 6 : sp.prec, v);
            for (int i = 0; i < n && i < static_cast<int>(sizeof tmp); ++i) o.put(static_cast<C>(tmp[i]));
            break;
        }
        default:
            // unknown conversion: emit it literally like the CRT does
            o.put(static_cast<C>('%'));
            o.put(conv);
            break;
        }
    }
    if (buf && cap) buf[o.pos < cap ? o.pos : cap - 1] = 0;
    return o.pos;
}

int NarrowVsnprintf(char* buf, size_t count, const char* fmt, va_list ap) {
    size_t n = Format<char>(buf, count, fmt, ap);
    if (!buf || count == 0) return static_cast<int>(n);
    return n >= count ? -1 : static_cast<int>(n);   // _vsnprintf: -1 on truncation
}
int WideVsnprintf(wchar_t* buf, size_t count, const wchar_t* fmt, va_list ap) {
    size_t n = Format<wchar_t>(buf, count, fmt, ap);
    if (!buf || count == 0) return static_cast<int>(n);
    return n >= count ? -1 : static_cast<int>(n);
}

}  // namespace

int __cdecl _vsnprintf(char* buf, size_t count, const char* fmt, va_list ap) { return NarrowVsnprintf(buf, count, fmt, ap); }
int __cdecl _vsnwprintf(wchar_t* buf, size_t count, const wchar_t* fmt, va_list ap) { return WideVsnprintf(buf, count, fmt, ap); }
int __cdecl vsprintf(char* buf, const char* fmt, va_list ap) { return static_cast<int>(Format<char>(buf, SIZE_MAX / 4, fmt, ap)); }
int __cdecl vswprintf(wchar_t* buf, const wchar_t* fmt, va_list ap) { return static_cast<int>(Format<wchar_t>(buf, SIZE_MAX / 8, fmt, ap)); }
int __cdecl vsprintf_s(char* buf, size_t size, const char* fmt, va_list ap) {
    int n = NarrowVsnprintf(buf, size, fmt, ap);
    if (n < 0 && size) buf[0] = 0;
    return n;
}
int __cdecl vswprintf_s(wchar_t* buf, size_t size, const wchar_t* fmt, va_list ap) {
    int n = WideVsnprintf(buf, size, fmt, ap);
    if (n < 0 && size) buf[0] = 0;
    return n;
}
int __cdecl sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = static_cast<int>(Format<char>(buf, SIZE_MAX / 4, fmt, ap));
    va_end(ap);
    return n;
}
int __cdecl sprintf_s(char* buf, size_t size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = NarrowVsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0 && size) buf[0] = 0;
    return n;
}
int __cdecl _snprintf(char* buf, size_t count, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = NarrowVsnprintf(buf, count, fmt, ap);
    va_end(ap);
    return n;
}
int __cdecl _snwprintf(wchar_t* buf, size_t count, const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = WideVsnprintf(buf, count, fmt, ap);
    va_end(ap);
    return n;
}
int __cdecl swprintf(wchar_t* buf, const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = static_cast<int>(Format<wchar_t>(buf, SIZE_MAX / 8, fmt, ap));
    va_end(ap);
    return n;
}
int __cdecl swprintf_s(wchar_t* buf, size_t size, const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = WideVsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0 && size) buf[0] = 0;
    return n;
}

// ---- Dbg ----
namespace {
ULONG DbgOut(const char* fmt, va_list ap) {
    char line[1024];
    int n = NarrowVsnprintf(line, sizeof line, fmt, ap);
    size_t len = n < 0 ? sizeof line - 1 : static_cast<size_t>(n);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
    line[len] = 0;
    TRACE(driver, "DbgPrint: %s", line);
    return 0;
}
}  // namespace
ULONG __cdecl DbgPrint(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    ULONG r = DbgOut(fmt, ap);
    va_end(ap);
    return r;
}
ULONG __cdecl DbgPrintEx(ULONG component, ULONG level, const char* fmt, ...) {
    (void)component; (void)level;
    va_list ap; va_start(ap, fmt);
    ULONG r = DbgOut(fmt, ap);
    va_end(ap);
    return r;
}
ULONG __cdecl vDbgPrintEx(ULONG component, ULONG level, const char* fmt, va_list ap) {
    (void)component; (void)level;
    return DbgOut(fmt, ap);
}
ULONG __cdecl vDbgPrintExWithPrefix(const char* prefix, ULONG component, ULONG level, const char* fmt, va_list ap) {
    (void)prefix; (void)component; (void)level;
    return DbgOut(fmt, ap);
}
VOID NTAPI DbgBreakPoint(void) { WARN(driver, "DbgBreakPoint ignored (no kernel debugger)"); }
BOOLEAN NTAPI KdRefreshDebuggerNotPresent(void) { return TRUE; }

// ---- Rtl strings (kernel flavour) ----
VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING dst, PCWSTR src) {
    dst->Buffer = const_cast<PWSTR>(src);
    if (!src) { dst->Length = dst->MaximumLength = 0; return; }
    size_t n = std::wcslen(src);
    if (n > 0x7FFE) n = 0x7FFE;
    dst->Length = static_cast<USHORT>(n * 2);
    dst->MaximumLength = static_cast<USHORT>(n * 2 + 2);
}
VOID NTAPI RtlInitAnsiString(PANSI_STRING dst, const char* src) {
    dst->Buffer = const_cast<PCHAR>(src);
    if (!src) { dst->Length = dst->MaximumLength = 0; return; }
    size_t n = std::strlen(src);
    if (n > 0xFFFE) n = 0xFFFE;
    dst->Length = static_cast<USHORT>(n);
    dst->MaximumLength = static_cast<USHORT>(n + 1);
}
VOID NTAPI RtlInitString(PSTRING dst, const char* src) { ntk::RtlInitAnsiString(dst, src); }
NTSTATUS NTAPI RtlInitUnicodeStringEx(PUNICODE_STRING dst, PCWSTR src) {
    if (src && std::wcslen(src) > 0x7FFE) return STATUS_NAME_TOO_LONG;
    ntk::RtlInitUnicodeString(dst, src);
    return STATUS_SUCCESS;
}
VOID NTAPI RtlCopyUnicodeString(PUNICODE_STRING dst, PCUNICODE_STRING src) {
    if (!src) { dst->Length = 0; return; }
    USHORT n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
    if (n) std::memcpy(dst->Buffer, src->Buffer, n);
    dst->Length = n;
    if (dst->Length < dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
}
NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING dst, PCWSTR src) {
    if (!src) return STATUS_SUCCESS;
    size_t n = std::wcslen(src) * 2;
    if (dst->Length + n > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(reinterpret_cast<char*>(dst->Buffer) + dst->Length, src, n);
    dst->Length = static_cast<USHORT>(dst->Length + n);
    if (dst->Length < dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI RtlAppendUnicodeStringToString(PUNICODE_STRING dst, PCUNICODE_STRING src) {
    if (!src || !src->Length) return STATUS_SUCCESS;
    if (dst->Length + src->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(reinterpret_cast<char*>(dst->Buffer) + dst->Length, src->Buffer, src->Length);
    dst->Length = static_cast<USHORT>(dst->Length + src->Length);
    if (dst->Length < dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
    return STATUS_SUCCESS;
}

// ---- Rtl memory ----
VOID NTAPI RtlCopyMemory(PVOID d, const void* s, SIZE_T n) { std::memcpy(d, s, n); }
VOID NTAPI RtlMoveMemory(PVOID d, const void* s, SIZE_T n) { std::memmove(d, s, n); }
VOID NTAPI RtlFillMemory(PVOID d, SIZE_T n, UCHAR c) { std::memset(d, c, n); }
VOID NTAPI RtlZeroMemory(PVOID d, SIZE_T n) { std::memset(d, 0, n); }
PVOID NTAPI RtlSecureZeroMemory(PVOID d, SIZE_T n) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(d);
    while (n--) *p++ = 0;
    return d;
}
SIZE_T NTAPI RtlCompareMemory(const void* a, const void* b, SIZE_T n) {
    const unsigned char* x = static_cast<const unsigned char*>(a);
    const unsigned char* y = static_cast<const unsigned char*>(b);
    SIZE_T i = 0;
    while (i < n && x[i] == y[i]) ++i;
    return i;
}

// ---- version ----
NTSTATUS NTAPI RtlGetVersion(PVOID info) {
    const auto& c = win10xbox::config::Current();
    struct VI { ULONG size, major, minor, build, platform; WCHAR csd[128]; USHORT sp_major, sp_minor, suite; UCHAR product, reserved; };
    auto* v = static_cast<VI*>(info);
    if (v->size < 5 * sizeof(ULONG) + 128 * sizeof(WCHAR)) return STATUS_INVALID_PARAMETER;
    v->major = c.os_major;
    v->minor = c.os_minor;
    v->build = c.os_build;
    v->platform = 2;
    std::memset(v->csd, 0, sizeof v->csd);
    std::wcsncpy(v->csd, c.os_csd.c_str(), 127);
    if (v->size >= sizeof(VI)) { v->sp_major = v->sp_minor = 0; v->suite = 0; v->product = 1; v->reserved = 0; }
    return STATUS_SUCCESS;
}
BOOLEAN NTAPI RtlIsNtDdiVersionAvailable(ULONG version) { return version <= 0x0A00000C; }
BOOLEAN NTAPI RtlIsServicePackVersionInstalled(ULONG version) { (void)version; return FALSE; }

// ---- CRT ----
namespace {
template <class C> int CmpI(const C* a, const C* b, size_t n, bool bounded) {
    for (size_t i = 0; !bounded || i < n; ++i) {
        int x = std::towlower(static_cast<wint_t>(static_cast<std::make_unsigned_t<C>>(a[i])));
        int y = std::towlower(static_cast<wint_t>(static_cast<std::make_unsigned_t<C>>(b[i])));
        if (x != y) return x < y ? -1 : 1;
        if (!x) break;
    }
    return 0;
}
}  // namespace
void* __cdecl crt_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
void* __cdecl crt_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
void* __cdecl crt_memset(void* d, int c, size_t n) { return std::memset(d, c, n); }
int __cdecl crt_memcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
const void* __cdecl crt_memchr(const void* s, int c, size_t n) { return std::memchr(s, c, n); }
size_t __cdecl crt_strlen(const char* s) { return std::strlen(s); }
size_t __cdecl crt_strnlen(const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) ++i; return i; }
char* __cdecl crt_strcpy(char* d, const char* s) { return std::strcpy(d, s); }
char* __cdecl crt_strncpy(char* d, const char* s, size_t n) { return std::strncpy(d, s, n); }
char* __cdecl crt_strcat(char* d, const char* s) { return std::strcat(d, s); }
char* __cdecl crt_strncat(char* d, const char* s, size_t n) { return std::strncat(d, s, n); }
int __cdecl crt_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
int __cdecl crt_strncmp(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n); }
int __cdecl crt_stricmp(const char* a, const char* b) { return CmpI<char>(a, b, 0, false); }
int __cdecl crt_strnicmp(const char* a, const char* b, size_t n) { return CmpI<char>(a, b, n, true); }
const char* __cdecl crt_strchr(const char* s, int c) { return std::strchr(s, c); }
const char* __cdecl crt_strrchr(const char* s, int c) { return std::strrchr(s, c); }
const char* __cdecl crt_strstr(const char* s, const char* t) { return std::strstr(s, t); }
size_t __cdecl crt_wcslen(const wchar_t* s) { return std::wcslen(s); }
size_t __cdecl crt_wcsnlen(const wchar_t* s, size_t n) { size_t i = 0; while (i < n && s[i]) ++i; return i; }
wchar_t* __cdecl crt_wcscpy(wchar_t* d, const wchar_t* s) { return std::wcscpy(d, s); }
wchar_t* __cdecl crt_wcsncpy(wchar_t* d, const wchar_t* s, size_t n) { return std::wcsncpy(d, s, n); }
wchar_t* __cdecl crt_wcscat(wchar_t* d, const wchar_t* s) { return std::wcscat(d, s); }
wchar_t* __cdecl crt_wcsncat(wchar_t* d, const wchar_t* s, size_t n) { return std::wcsncat(d, s, n); }
int __cdecl crt_wcscmp(const wchar_t* a, const wchar_t* b) { return std::wcscmp(a, b); }
int __cdecl crt_wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) { return std::wcsncmp(a, b, n); }
int __cdecl crt_wcsicmp(const wchar_t* a, const wchar_t* b) { return CmpI<wchar_t>(a, b, 0, false); }
int __cdecl crt_wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return CmpI<wchar_t>(a, b, n, true); }
const wchar_t* __cdecl crt_wcschr(const wchar_t* s, wchar_t c) { return std::wcschr(s, c); }
const wchar_t* __cdecl crt_wcsrchr(const wchar_t* s, wchar_t c) { return std::wcsrchr(s, c); }
const wchar_t* __cdecl crt_wcsstr(const wchar_t* s, const wchar_t* t) { return std::wcsstr(s, t); }
int __cdecl crt_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int __cdecl crt_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int __cdecl crt_towupper(int c) { return std::towupper(static_cast<wint_t>(c)); }
int __cdecl crt_towlower(int c) { return std::towlower(static_cast<wint_t>(c)); }
int __cdecl crt_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int __cdecl crt_isdigit(int c) { return c >= '0' && c <= '9'; }
int __cdecl crt_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int __cdecl crt_isxdigit(int c) { return crt_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
unsigned long __cdecl crt_strtoul(const char* s, char** end, int base) { return std::strtoul(s, end, base); }
long __cdecl crt_strtol(const char* s, char** end, int base) { return std::strtol(s, end, base); }
unsigned long long __cdecl crt_strtoull(const char* s, char** end, int base) { return std::strtoull(s, end, base); }
unsigned long __cdecl crt_wcstoul(const wchar_t* s, wchar_t** end, int base) { return std::wcstoul(s, end, base); }
size_t __cdecl crt_mbstowcs(wchar_t* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = static_cast<unsigned char>(s[i]);
    if (i < n) d[i] = 0;
    return i;
}
size_t __cdecl crt_wcstombs(char* d, const wchar_t* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = static_cast<char>(s[i]);
    if (i < n) d[i] = 0;
    return i;
}
void __cdecl crt_qsort(void* base, size_t n, size_t size, int(__cdecl* cmp)(const void*, const void*)) { std::qsort(base, n, size, cmp); }
void* __cdecl crt_bsearch(const void* key, const void* base, size_t n, size_t size, int(__cdecl* cmp)(const void*, const void*)) {
    return std::bsearch(key, base, n, size, cmp);
}
void __cdecl crt_purecall(void) { ERR(driver, "_purecall: pure virtual function called in a driver"); RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, nullptr); }
// 64-bit shifts the compiler emits as calls on 32-bit targets are not needed on x64.

}  // namespace ntk

namespace win10xbox::drivers::detail {
namespace {
const Export kRtlTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
#define XA(name, fn) {name, reinterpret_cast<void*>(&ntk::fn)}
    X(_vsnprintf), X(_vsnwprintf), X(vsprintf), X(vswprintf), X(vsprintf_s), X(vswprintf_s), X(sprintf), X(sprintf_s),
    X(_snprintf), X(_snwprintf), X(swprintf), X(swprintf_s),
    X(DbgPrint), X(DbgPrintEx), X(vDbgPrintEx), X(vDbgPrintExWithPrefix), X(DbgBreakPoint), X(KdRefreshDebuggerNotPresent),
    X(RtlInitUnicodeString), X(RtlInitAnsiString), X(RtlInitString), X(RtlInitUnicodeStringEx), X(RtlCopyUnicodeString),
    X(RtlAppendUnicodeToString), X(RtlAppendUnicodeStringToString),
    X(RtlCopyMemory), X(RtlMoveMemory), X(RtlFillMemory), X(RtlZeroMemory), X(RtlSecureZeroMemory), X(RtlCompareMemory),
    X(RtlGetVersion), X(RtlIsNtDdiVersionAvailable), X(RtlIsServicePackVersionInstalled),
    XA("memcpy", crt_memcpy), XA("memmove", crt_memmove), XA("memset", crt_memset), XA("memcmp", crt_memcmp),
    XA("memchr", crt_memchr), XA("strlen", crt_strlen), XA("strnlen", crt_strnlen), XA("strcpy", crt_strcpy),
    XA("strncpy", crt_strncpy), XA("strcat", crt_strcat), XA("strncat", crt_strncat), XA("strcmp", crt_strcmp),
    XA("strncmp", crt_strncmp), XA("_stricmp", crt_stricmp), XA("_strnicmp", crt_strnicmp), XA("strchr", crt_strchr),
    XA("strrchr", crt_strrchr), XA("strstr", crt_strstr), XA("wcslen", crt_wcslen), XA("wcsnlen", crt_wcsnlen),
    XA("wcscpy", crt_wcscpy), XA("wcsncpy", crt_wcsncpy), XA("wcscat", crt_wcscat), XA("wcsncat", crt_wcsncat),
    XA("wcscmp", crt_wcscmp), XA("wcsncmp", crt_wcsncmp), XA("_wcsicmp", crt_wcsicmp), XA("_wcsnicmp", crt_wcsnicmp),
    XA("wcschr", crt_wcschr), XA("wcsrchr", crt_wcsrchr), XA("wcsstr", crt_wcsstr), XA("toupper", crt_toupper),
    XA("tolower", crt_tolower), XA("towupper", crt_towupper), XA("towlower", crt_towlower), XA("isspace", crt_isspace),
    XA("isdigit", crt_isdigit), XA("isalpha", crt_isalpha), XA("isxdigit", crt_isxdigit), XA("strtoul", crt_strtoul),
    XA("strtol", crt_strtol), XA("strtoull", crt_strtoull), XA("wcstoul", crt_wcstoul), XA("mbstowcs", crt_mbstowcs),
    XA("wcstombs", crt_wcstombs), XA("qsort", crt_qsort), XA("bsearch", crt_bsearch), XA("_purecall", crt_purecall),
#undef X
#undef XA
};
}  // namespace
ExportGroup RtlExports() { return {kRtlTable, sizeof(kRtlTable) / sizeof(kRtlTable[0])}; }
}  // namespace win10xbox::drivers::detail
