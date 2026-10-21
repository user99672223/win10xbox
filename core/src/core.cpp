#include "win10xbox/core.h"
#include "win10xbox/trace.h"
#include <windows.h>

#ifndef WIN10XBOX_GIT_REV
#define WIN10XBOX_GIT_REV "unknown"
#endif

namespace win10xbox {

namespace config {
Config& Current() { static Config c; return c; }
}

Version GetVersion() { return {0, 0, 1, WIN10XBOX_GIT_REV}; }

static bool g_initialized = false;

int Initialize(const config::Config& cfg) {
    if (g_initialized) return 0;
    config::Current() = cfg;
    trace::Initialize(cfg.trace_default_on);
    TRACE(core, "win10xbox %d.%d.%d (%s) initialising, machine_root=%ls",
          GetVersion().major, GetVersion().minor, GetVersion().patch,
          GetVersion().git, cfg.machine_root.c_str());
    g_initialized = true;
    return 0;
}

void Shutdown() {
    if (!g_initialized) return;
    TRACE(core, "shutdown");
    trace::Shutdown();
    g_initialized = false;
}

}  // namespace win10xbox
