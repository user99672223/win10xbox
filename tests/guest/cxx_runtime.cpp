/* guest_cxx_runtime: exceptions and the C++ runtime on the layer.
 *
 *  - C++ throw/catch across frames, exception_ptr, and across a DLL boundary (guest_cxx_dll)
 *  - SEH access violation / divide by zero / RaiseException with a custom code, the unhandled-exception filter
 *  - vectored exception handlers
 *  - std::thread, std::mutex, std::condition_variable, std::call_once, magic statics
 *  - thread_local with constructors/destructors, the executable's TLS callback
 *  - static initialisation order (this module and the DLL)
 *  - std::filesystem on the VFS
 *  - a stack overflow (guard page) caught on a secondary thread
 *
 * Built with MSVC (/EHsc; static CRT and /MD) and llvm-mingw. clang's mingw target silently ignores
 * __try/__except, so there the same Seh helper is implemented with a vectored handler that restores a captured
 * context; the exceptions themselves are real either way. Prints "cxx: <name> ok" per section, returns 42.
 */
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cxx_dll.h"

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("cxx: FAIL line %d: %s\n", __LINE__, #c);                 \
            std::fflush(stdout);                                                  \
        }                                                                         \
    } while (0)

static void section(const char* name, int fail_before) {
    std::printf("cxx: %s %s\n", name, g_fail == fail_before ? "ok" : "FAILED");
    std::fflush(stdout);
}

/* ---- the executable's TLS callback ------------------------------------------------------------------------ */
static std::atomic<int> g_tls_attach{0}, g_tls_detach{0}, g_tls_process{0};

static void NTAPI tls_callback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_THREAD_ATTACH) ++g_tls_attach;
    else if (reason == DLL_THREAD_DETACH) ++g_tls_detach;
    else if (reason == DLL_PROCESS_ATTACH) ++g_tls_process;
}

#ifdef _MSC_VER
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:tls_callback_ptr")
extern "C" {
#pragma const_seg(".CRT$XLB")
extern const PIMAGE_TLS_CALLBACK tls_callback_ptr;
const PIMAGE_TLS_CALLBACK tls_callback_ptr = tls_callback;
#pragma const_seg()
}
#else
extern "C" {
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK tls_callback_ptr = tls_callback;
}
#endif

/* ---- static initialisation order -------------------------------------------------------------------------- */
static int g_order_log[8];
static int g_order_n = 0;
static int order_step(int tag) {
    g_order_log[g_order_n++] = tag;
    return tag;
}
static int g_s1 = order_step(1);
static int g_s2 = order_step(g_s1 + 1);
static int g_s3 = order_step(g_s2 + 1);

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

#ifdef _MSC_VER
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
static thread_local Guard* t_guard = nullptr;

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
        throw CxxBoom{5};
    } catch (...) {
        ep = std::current_exception();
    }
    int via_ptr = 0;
    try {
        std::rethrow_exception(ep);
    } catch (const CxxBoom& b) {
        via_ptr = b.code;
    }
    CHECK(via_ptr == 5);
    section("throw_catch", f);
}

/* ---- 2. across the DLL boundary --------------------------------------------------------------------------- */
static void cb_throw_boom() { throw CxxBoom{31}; }
static void cb_throw_std() { throw std::runtime_error("from exe via dll"); }

static void test_cxx_dll() {
    int f = g_fail;
    CHECK(cxxdll_ctor_ran() == 1);

    int code = 0;
    try {
        cxxdll_throw(11);
    } catch (const CxxBoom& b) {
        code = b.code;
    }
    CHECK(code == 11);

    code = 0;
    try {
        cxxdll_call(cb_throw_boom);
    } catch (const CxxBoom& b) {
        code = b.code;
    }
    CHECK(code == 31);

    std::string what;
    try {
        cxxdll_call(cb_throw_std);
    } catch (const std::exception& e) {
        what = e.what();
    }
    CHECK(what == "from exe via dll");

    CHECK(cxxdll_catch_inside(9) == 9);
    section("dll_boundary", f);
}

/* ---- 3. SEH: access violation, divide by zero, RaiseException, unhandled-exception filter ---------------- */
static void do_av_write(void*) { *(volatile int*)nullptr = 1; }
static void do_av_read(void*) { (void)*(volatile int*)(ULONG_PTR)0x10; }
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

static std::atomic<int> g_uef_hits{0};
static std::atomic<DWORD> g_uef_code{0};
static LONG WINAPI MyUef(EXCEPTION_POINTERS* ep) {
    ++g_uef_hits;
    g_uef_code = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

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

    LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(MyUef);
    s = SehRun(do_av_write, nullptr, true);
    CHECK(s.caught && s.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(g_uef_hits == 1 && g_uef_code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(s.filter_result == EXCEPTION_EXECUTE_HANDLER);
    SetUnhandledExceptionFilter(old);
    section("seh", f);
}

/* ---- 4. vectored handlers --------------------------------------------------------------------------------- */
static std::atomic<int> g_veh_seen{0}, g_veh_custom{0};
static LONG CALLBACK TestVeh(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xE0005678) {
        ++g_veh_custom;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_ACCESS_VIOLATION) ++g_veh_seen;
    return EXCEPTION_CONTINUE_SEARCH;
}
static int g_after_raise = 0;
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

/* ---- 5. threads, mutex, condition_variable, call_once, magic statics ------------------------------------- */
static int magic_value() {
    static int v = [] {
        static std::atomic<int> n{0};
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 1000 + ++n;
    }();
    return v;
}

static void test_threads() {
    int f = g_fail;
    std::mutex m;
    long counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < 1000; ++k) {
                std::lock_guard<std::mutex> lk(m);
                ++counter;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(counter == 4000);

    std::condition_variable cv;
    bool ready = false, done = false;
    std::thread waiter([&] {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return ready; });
        done = true;
        cv.notify_all();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::lock_guard<std::mutex> lk(m);
        ready = true;
    }
    cv.notify_all();
    {
        std::unique_lock<std::mutex> lk(m);
        bool ok = cv.wait_for(lk, std::chrono::seconds(20), [&] { return done; });
        CHECK(ok);
    }
    waiter.join();

    std::once_flag once;
    std::atomic<int> once_n{0};
    std::vector<int> magic(4);
    ts.clear();
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&, i] {
            std::call_once(once, [&] { ++once_n; });
            magic[i] = magic_value();
        });
    for (auto& t : ts) t.join();
    CHECK(once_n == 1);
    for (int i = 0; i < 4; ++i) CHECK(magic[i] == 1001);

    /* an exception thrown in a thread and caught in it */
    std::atomic<int> caught{0};
    std::thread te([&] {
        try {
            deep(3);
        } catch (const std::runtime_error&) {
            caught = 1;
        }
    });
    te.join();
    CHECK(caught == 1);
    section("threads", f);
}

/* ---- 6. thread_local and TLS callbacks -------------------------------------------------------------------- */
static std::atomic<int> g_tl_ctor{0}, g_tl_dtor{0};
struct TlObj {
    int v;
    TlObj() : v(7) { ++g_tl_ctor; }
    ~TlObj() { ++g_tl_dtor; }
};
static thread_local TlObj t_obj;
static thread_local int t_plain = 5;

static void test_thread_local() {
    int f = g_fail;
    CHECK(g_tls_process >= 1);
    int attach0 = g_tls_attach, detach0 = g_tls_detach;
    CHECK(t_obj.v == 7 && t_plain == 5);
    t_obj.v = 8;
    t_plain = 6;
    std::atomic<int> seen_obj{0}, seen_plain{0}, dll_first{0}, dll_second{0};
    std::thread t([&] {
        seen_obj = t_obj.v;       /* a fresh instance, not main's 8 */
        seen_plain = t_plain;     /* 5, not main's 6 */
        t_obj.v = 99;
        dll_first = cxxdll_tls_bump();   /* 101 */
        dll_second = cxxdll_tls_bump();  /* 102 */
    });
    t.join();
    CHECK(seen_obj == 7 && seen_plain == 5);
    CHECK(dll_first == 101 && dll_second == 102);
    CHECK(t_obj.v == 8 && t_plain == 6);
    CHECK(cxxdll_tls_bump() == 101); /* main thread has its own copy */
    CHECK(g_tl_ctor >= 2);
    CHECK(g_tl_dtor >= 1);           /* the thread's object was destroyed at thread exit */
    CHECK(g_tls_attach - attach0 == 1);
    CHECK(g_tls_detach - detach0 == 1);
    section("thread_local", f);
}

/* ---- 7. static init order --------------------------------------------------------------------------------- */
static void test_static_init() {
    int f = g_fail;
    CHECK(g_s1 == 1 && g_s2 == 2 && g_s3 == 3);
    CHECK(g_order_n == 3 && g_order_log[0] == 1 && g_order_log[1] == 2 && g_order_log[2] == 3);
    CHECK(cxxdll_ctor_ran() == 1); /* the DLL's constructors ran before main */
    section("static_init", f);
}

/* ---- 8. std::filesystem on the VFS ------------------------------------------------------------------------ */
namespace fs = std::filesystem;

static void test_filesystem() {
    int f = g_fail;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    CHECK(n > 0 && n < MAX_PATH);
    fs::path exe_path(exe);
    fs::path dir = exe_path.parent_path();
    std::error_code ec;
    CHECK(fs::exists(exe_path, ec));
    CHECK(fs::is_directory(dir, ec));
    bool found_self = false, found_dll = false;
    int entries = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        ++entries;
        if (e.path().filename() == exe_path.filename()) found_self = true;
        if (e.path().filename().wstring().find(L"guest_cxx_dll") == 0) found_dll = true;
    }
    CHECK(!ec);
    CHECK(entries >= 2 && found_self && found_dll);

    fs::path tmp = fs::temp_directory_path(ec) / "cxxrt_fs";
    CHECK(!ec);
    fs::remove_all(tmp, ec);
    CHECK(fs::create_directories(tmp / "sub", ec));
    {
        std::ofstream o(tmp / "a.txt", std::ios::binary);
        o << "hello";
    }
    {
        std::ofstream o(tmp / "sub" / "b.txt", std::ios::binary);
        o << "world!!";
    }
    CHECK(fs::file_size(tmp / "a.txt", ec) == 5);
    int files = 0, dirs = 0, total = 0;
    for (const auto& e : fs::recursive_directory_iterator(tmp, ec)) {
        ++total;
        if (e.is_directory()) ++dirs;
        else if (e.is_regular_file()) ++files;
    }
    CHECK(total == 3 && files == 2 && dirs == 1);
    std::string back;
    {
        std::ifstream i(tmp / "sub" / "b.txt", std::ios::binary);
        std::getline(i, back);
    }
    CHECK(back == "world!!");
    fs::rename(tmp / "a.txt", tmp / "c.txt", ec);
    CHECK(!ec && fs::exists(tmp / "c.txt") && !fs::exists(tmp / "a.txt"));
    CHECK(fs::remove_all(tmp, ec) == 4);
    CHECK(!fs::exists(tmp));
    section("filesystem", f);
}

/* ---- 9. stack overflow on a thread ------------------------------------------------------------------------ */
typedef void (*RecFn)(int);
static RecFn volatile g_rec = nullptr;
static volatile int g_depth = 0;
static void rec(int n) {
    volatile char pad[1024];
    pad[0] = (char)n;
    g_depth = n;
    g_rec(n + 1);
    pad[1] = pad[0];
}
static void do_overflow(void*) {
    g_rec = rec;
    rec(0);
}

struct SoResult {
    Seh seh;
};
static DWORD WINAPI so_thread(LPVOID p) {
    static_cast<SoResult*>(p)->seh = SehRun(do_overflow, nullptr);
    return 0;
}

static void test_stack_overflow() {
    int f = g_fail;
    SoResult r = {};
    HANDLE h = CreateThread(nullptr, 256 * 1024, so_thread, &r, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    CHECK(h != nullptr);
    if (h) {
        CHECK(WaitForSingleObject(h, 60000) == WAIT_OBJECT_0);
        CloseHandle(h);
    }
    CHECK(r.seh.caught && r.seh.code == EXCEPTION_STACK_OVERFLOW);
    CHECK(g_depth > 10);
    std::printf("cxx: stack overflow after %d frames\n", (int)g_depth);
    section("stack_overflow", f);
}

#ifndef _MSC_VER
static void install_guard_veh() { AddVectoredExceptionHandler(1, GuardVeh); }
#else
static void install_guard_veh() {}
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cxx: start (%s)\n",
#ifdef _MSC_VER
                "msvc"
#else
                "mingw"
#endif
    );
    install_guard_veh();
    test_cxx_basic();
    test_cxx_dll();
    test_seh();
    test_veh();
    test_threads();
    test_thread_local();
    test_static_init();
    test_filesystem();
    test_stack_overflow();
    std::printf("cxx: done failures=%d\n", g_fail);
    return g_fail ? 1 : 42;
}
