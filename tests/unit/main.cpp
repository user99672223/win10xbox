#include "check.h"
#include "win10xbox/core.h"
#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    win10xbox::config::Config cfg;
    cfg.trace_default_on = false;
    win10xbox::Initialize(cfg);
    int run = 0;
    for (auto& c : w10x_test::Cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = w10x_test::Failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", w10x_test::Failures() == before ? " OK " : "FAIL", c.name);
        ++run;
    }
    std::printf("%d tests, %d failures\n", run, w10x_test::Failures());
    win10xbox::Shutdown();
    return w10x_test::Failures() ? 1 : 0;
}
