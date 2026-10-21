/*
 * kmtest.h - a minimal in-kernel test harness in the style of ReactOS's kmtests (ok(), ok_eq_*(),
 * ok_irql(), trace(), skip()). MIT, written from scratch: it only mirrors the macro names so test code
 * written for kmtest ports with little change. Results are collected per run and returned to the caller
 * (the IOCTL that ran the tests) instead of going through a user-mode kmtest runner.
 */
#ifndef KMTEST_H
#define KMTEST_H

#include "ntk/ntk.h"

typedef struct _KMT_RESULTS {
    ULONG Executed;
    ULONG Failed;
    ULONG Skipped;
} KMT_RESULTS;

extern KMT_RESULTS KmtResults;

static void KmtReport(int good, const char *expr, const char *file, int line, const char *fmt, ...) {
    UNREFERENCED_PARAMETER(file);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(fmt);
    ++KmtResults.Executed;
    if (!good) {
        ++KmtResults.Failed;
        DbgPrint("kmtest: %s:%d: test failed: %s\n", file, line, expr);
    }
}

#define KMT_RESET() (KmtResults.Executed = KmtResults.Failed = KmtResults.Skipped = 0)

#define ok(cond, ...) KmtReport((cond) ? 1 : 0, #cond, __FILE__, __LINE__, __VA_ARGS__)
/* Arguments are evaluated exactly once. */
#define KMT_OK_EQ(type, value, expected, spec) \
    do { \
        type _kmt_v = (type)(value), _kmt_e = (type)(expected); \
        KmtReport(_kmt_v == _kmt_e, #value " == " #expected, __FILE__, __LINE__, "value " spec " expected " spec, _kmt_v, _kmt_e); \
    } while (0)
#define ok_eq_pointer(value, expected) KMT_OK_EQ(PVOID, value, expected, "%p")
#define ok_eq_int(value, expected) KMT_OK_EQ(int, value, expected, "%d")
#define ok_eq_hex(value, expected) KMT_OK_EQ(ULONG, value, expected, "0x%08lx")
#define ok_eq_ulong(value, expected) KMT_OK_EQ(ULONG, value, expected, "%lu")
#define ok_eq_bool(value, expected) KMT_OK_EQ(int, !!(value), !!(expected), "%d")
#define ok_eq_wstr(value, expected) ok(KmtWstrEq((value), (expected)), "wide strings differ")
#define ok_irql(irql) ok_eq_int(KeGetCurrentIrql(), (irql))
#define ok_bool_true(value, desc) ok((value) == TRUE, desc)
#define ok_bool_false(value, desc) ok((value) == FALSE, desc)
#define ok_ntstatus(status, expected) ok_eq_hex((status), (expected))
#define skip(cond, ...) \
    do { \
        if (!(cond)) { ++KmtResults.Skipped; } \
    } while (0)
#define trace(...) DbgPrint("kmtest: " __VA_ARGS__)

static int KmtWstrEq(const WCHAR *a, const WCHAR *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

#endif
