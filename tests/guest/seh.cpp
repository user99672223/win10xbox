/* guest_seh: structured and C++ exception handling through the layer.
 *
 * Exceptions are raised by guest code, delivered by the kernel to the host ntdll's KiUserExceptionDispatcher, and
 * unwound with the guest's own .pdata (docs/DECISIONS.md E2). This guest covers:
 *
 *  - C++ throw/catch across frames (destructors, rethrow, exception_ptr)
 *  - C++ throw/catch across a DLL boundary (guest_seh_dll), unwinding through a DLL frame
 *  - SEH: __try/__except around an access violation (read and write), a divide by zero, RaiseException with a
 *    custom code and parameters, an access violation raised from a DLL frame, the unhandled-exception filter
 *  - vectored exception handlers: one that sees an access violation and passes it on, one that continues
 *    execution after a raised exception, removal
 *
 * Built with MSVC (/EHsc, static CRT) and llvm-mingw. __try/__except is used natively where the compiler provides
 * it (MSVC; clang with -fms-extensions when the configure-time check in tests/guest/CMakeLists.txt links, which
 * defines SEH_NATIVE). Otherwise the same Seh helper is implemented with a vectored handler that restores a
 * captured context; the exceptions themselves are real either way. Prints "seh: <name> ok" per section and returns
 * 42.
 */
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include "seh_dll.h"

#if defined(_MSC_VER) || defined(SEH_NATIVE)
#define SEH_USE_TRY 1
#endif

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("seh: FAIL line %d: %s\n", __LINE__, #c);                 \
            std::fflush(stdout);                                                  \
        }                                                                         \
    } while (0)

static void section(const char* name, int fail_before) {
    std::printf("seh: %s %s\n", name, g_fail == fail_before ? "ok" : "FAILED");
    std::fflush(stdout);
}

/* ---- SEH helper ------------------------------------------------------------------------------------------- */
struct Seh {
    bool caught;
    DWORD code;
    DWORD nparams;
    ULONG_PTR info[4];
    LONG filter_result; /* what UnhandledExceptionFilter returned, when asked */
};
typedef void (*SehFn)(void*);

static void Record(Seh* s, EXCEPTION_POINTERS* ep, bool via_uef) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    s->caught = true;
    s->code = r->ExceptionCode;
    s->nparams = r->NumberParameters;
    for (DWORD i = 0; i < 4 && i < r->NumberParameters; ++i) s->info[i] = r->ExceptionInformation[i];
    if (via_uef) s->filter_result = UnhandledExceptionFilter(ep);
}

#ifdef SEH_USE_TRY
static LONG SehFilter(EXCEPTION_POINTERS* ep, Seh* s, bool via_uef) {
    Record(s, ep, via_uef);
    return EXCEPTION_EXECUTE_HANDLER;
}
static Seh SehRun(SehFn fn, void* arg, bool via_uef = false) {
    Seh s = {};
    __try {
        fn(arg);
    } __except (SehFilter(GetExceptionInformation(), &s, via_uef)) {
    }
    return s;
}
#else
struct Guard {
    CONTEXT ctx;
    Seh s;
    volatile LONG fired;
    bool via_uef;
    Guard* prev;
};
static Guard* t_guard = nullptr;

static LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* ep) {
    Guard* g = t_guard;
    if (!g) return EXCEPTION_CONTINUE_SEARCH;
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0x406D1388 /* SetThreadName */ || c == DBG_PRINTEXCEPTION_C || c == 0xE06D7363 /* C++ throw */ ||
        c == 0x20474343 /* gcc */ || (c & 0xFFFFFF00) == 0x20474300)
        return EXCEPTION_CONTINUE_SEARCH;
    Record(&g->s, ep, g->via_uef);
    g->fired = 1;
    t_guard = g->prev;
    *ep->ContextRecord = g->ctx;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline, optnone)) static Seh SehRun(SehFn fn, void* arg, bool via_uef = false) {
    Guard g;
    std::memset(&g, 0, sizeof g);
    g.via_uef = via_uef;
    g.prev = t_guard;
    t_guard = &g;
    RtlCaptureContext(&g.ctx);
    if (!g.fired) {
        fn(arg);
        t_guard = g.prev;
    }
    Seh s = g.s;
    return s;
}
#endif

/* ---- 1. C++ exceptions across frames ---------------------------------------------------------------------- */
static int g_dtors = 0;
struct Sentinel {
    ~Sentinel() { ++g_dtors; }
};

static int deep(int n) {
    Sentinel s;
    if (n == 0) throw std::runtime_error("deep");
    return deep(n - 1) + 1;
}

static void test_cxx_basic() {
    int f = g_fail;
    g_dtors = 0;
    std::string what;
    try {
        deep(5);
    } catch (const std::exception& e) {
        what = e.what();
    }
    CHECK(what == "deep");
    CHECK(g_dtors == 6);

    int got = 0;
    try {
        throw 77;
    } catch (int v) {
        got = v;
    }
    CHECK(got == 77);

    bool rethrown = false;
    try {
        try {
            throw std::logic_error("inner");
        } catch (...) {
            throw;
        }
    } catch (const std::logic_error& e) {
        rethrown = std::strcmp(e.what(), "inner") == 0;
    }
    CHECK(rethrown);

    std::exception_ptr ep;
    try {
        throw SehBoom{5};
    } catch (...) {
        ep = std::current_exception();
    }
    int via_ptr = 0;
    try {
        std::rethrow_exception(ep);
    } catch (const SehBoom& b) {
        via_ptr = b.code;
    }
    CHECK(via_ptr == 5);
    section("throw_catch", f);
}

/* ---- 2. across the DLL boundary --------------------------------------------------------------------------- */
static void cb_throw_boom() { throw SehBoom{31}; }
static void cb_throw_std() { throw std::runtime_error("from exe via dll"); }

static void test_cxx_dll() {
    int f = g_fail;
    CHECK(sehdll_ctor_ran() == 1);

    int code = 0;
    try {
        sehdll_throw(11);
    } catch (const SehBoom& b) {
        code = b.code;
    }
    CHECK(code == 11);

    int dtors = sehdll_dtors();
    code = 0;
    try {
        sehdll_call(cb_throw_boom);
    } catch (const SehBoom& b) {
        code = b.code;
    }
    CHECK(code == 31);
    CHECK(sehdll_dtors() == dtors + 1); /* the DLL frame was unwound, its destructor ran */

    std::string what;
    try {
        sehdll_call(cb_throw_std);
    } catch (const std::exception& e) {
        what = e.what();
    }
    CHECK(what == "from exe via dll");
    CHECK(sehdll_dtors() == dtors + 2);

    CHECK(sehdll_catch_inside(9) == 9);
    section("dll_boundary", f);
}

/* ---- 3. SEH: access violation, divide by zero, RaiseException, DLL frame ---------------------------------- */
static void do_av_write(void*) {
    int* volatile p = nullptr;
    *p = 1;
}
static void do_av_read(void*) {
    int* volatile p = reinterpret_cast<int*>(static_cast<ULONG_PTR>(0x10));
    (void)*p;
}
static void do_div0(void*) {
    volatile int z = 0;
    volatile int r = 10 / z;
    (void)r;
}
static void do_raise(void*) {
    ULONG_PTR args[2] = {0x1111, 0x2222};
    RaiseException(0xE0001234, 0, 2, args);
}
static void do_nothing(void*) {}
static void do_dll_fault(void*) { sehdll_fault(); }

static void test_seh() {
    int f = g_fail;
    Seh s = SehRun(do_nothing, nullptr);
    CHECK(!s.caught);

    s = SehRun(do_av_write, nullptr);
    CHECK(s.caught && s.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(s.nparams == 2 && s.info[0] == 1 && s.info[1] == 0);

    s = SehRun(do_av_read, nullptr);
    CHECK(s.caught && s.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(s.nparams == 2 && s.info[0] == 0 && s.info[1] == 0x10);

    s = SehRun(do_div0, nullptr);
    CHECK(s.caught && s.code == EXCEPTION_INT_DIVIDE_BY_ZERO);

    s = SehRun(do_raise, nullptr);
    CHECK(s.caught && s.code == 0xE0001234);
    CHECK(s.nparams == 2 && s.info[0] == 0x1111 && s.info[1] == 0x2222);

    s = SehRun(do_dll_fault, nullptr);
    CHECK(s.caught && s.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(s.nparams == 2 && s.info[0] == 1 && s.info[1] == 0);
    section("seh", f);
}

/* ---- 4. vectored handlers --------------------------------------------------------------------------------- */
static volatile LONG g_veh_seen = 0, g_veh_custom = 0;
static LONG CALLBACK TestVeh(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xE0005678) {
        InterlockedIncrement(&g_veh_custom);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_ACCESS_VIOLATION) InterlockedIncrement(&g_veh_seen);
    return EXCEPTION_CONTINUE_SEARCH;
}
static volatile int g_after_raise = 0;
static void do_raise_veh(void*) {
    RaiseException(0xE0005678, 0, 0, nullptr);
    g_after_raise = 1; /* reached only if the VEH continued execution */
}

static void test_veh() {
    int f = g_fail;
    PVOID h = AddVectoredExceptionHandler(1, TestVeh);
    CHECK(h != nullptr);
    Seh s = SehRun(do_raise_veh, nullptr);
    CHECK(!s.caught && g_veh_custom == 1 && g_after_raise == 1);
    s = SehRun(do_av_write, nullptr);
    CHECK(s.caught && g_veh_seen == 1);
    CHECK(RemoveVectoredExceptionHandler(h) != 0);
    s = SehRun(do_av_write, nullptr);
    CHECK(s.caught && g_veh_seen == 1); /* removed: not called again */
    section("veh", f);
}

/* ---- 5. the unhandled-exception filter -------------------------------------------------------------------- */
static volatile LONG g_uef_hits = 0;
static volatile DWORD g_uef_code = 0;
static LONG WINAPI MyUef(EXCEPTION_POINTERS* ep) {
    InterlockedIncrement(&g_uef_hits);
    g_uef_code = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void test_uef() {
    int f = g_fail;
    LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(MyUef);
    Seh s = SehRun(do_av_write, nullptr, true);
    CHECK(s.caught && s.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(g_uef_hits == 1 && g_uef_code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(s.filter_result == EXCEPTION_EXECUTE_HANDLER);
    SetUnhandledExceptionFilter(old);
    section("uef", f);
}

#ifndef SEH_USE_TRY
static void install_guard_veh() { AddVectoredExceptionHandler(1, GuardVeh); }
#else
static void install_guard_veh() {}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("seh: start (%s)\n",
#if defined(_MSC_VER)
                "msvc __try"
#elif defined(SEH_NATIVE)
                "clang __try"
#else
                "clang, vectored-handler __try emulation"
#endif
    );
    install_guard_veh();
    test_cxx_basic();
    test_cxx_dll();
    test_seh();
    test_veh();
    test_uef();
    std::printf("seh: done failures=%d\n", g_fail);
    return g_fail ? 1 : 42;
}
