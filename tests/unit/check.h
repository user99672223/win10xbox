// Tiny test harness: TEST(name) { CHECK(expr); CHECK_EQ(a,b); } Tests are
// registered statically and run by main.cpp; failures print file:line.
#pragma once
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace w10x_test {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& Cases() { static std::vector<Case> c; return c; }
inline int& Failures() { static int f = 0; return f; }
inline bool Register(const char* n, void (*f)()) { Cases().push_back({n, f}); return true; }
}

#define TEST(name) \
    static void test_##name(); \
    static const bool test_reg_##name = ::w10x_test::Register(#name, &test_##name); \
    static void test_##name()

#define CHECK(expr) do { if (!(expr)) { ++::w10x_test::Failures(); \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++::w10x_test::Failures(); \
    std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define CHECK_MSG(expr, ...) do { if (!(expr)) { ++::w10x_test::Failures(); \
    std::printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #expr); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
