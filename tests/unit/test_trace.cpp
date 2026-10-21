#include "check.h"
#include "win10xbox/trace.h"
#include <string>
#include <windows.h>

WIN10XBOX_DECLARE_CHANNEL(testch);

static std::string g_last;
static void TestSink(const char* line, void*) { g_last = line; }

TEST(trace_channel_spec_enables_channel) {
    using namespace win10xbox::trace;
    SetEnvironmentVariableA("WIN10XBOX_DEBUG", "+testch");
    Initialize(false);
    CHECK(Enabled(&__win10xbox_dbch_testch, Class::Trace));
    CHECK(!Enabled(&__win10xbox_dbch_core, Class::Trace));
    CHECK(Enabled(&__win10xbox_dbch_core, Class::Err));
    AddSink(TestSink, nullptr);
    TRACE(testch, "hello %d", 42);
    CHECK(g_last.find("trace:testch:") != std::string::npos);
    CHECK(g_last.find("hello 42") != std::string::npos);
    SetEnvironmentVariableA("WIN10XBOX_DEBUG", "-all");
    Initialize(false);
    CHECK(!Enabled(&__win10xbox_dbch_testch, Class::Trace));
    SetEnvironmentVariableA("WIN10XBOX_DEBUG", nullptr);
}
