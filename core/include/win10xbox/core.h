// Public entry points of the win10xbox core static library. Both hosts
// (host-win32 console, host-uwp shell) drive the layer only through this
// header plus config.h; nothing here depends on UWP/WinRT.
#pragma once
#include <cstdint>
#include <string>
#include "win10xbox/config.h"

namespace win10xbox {

struct Version { int major, minor, patch; const char* git; };
Version GetVersion();

// Initialise tracing, host-ntdll resolution, the virtual clock and the
// machine folder. Idempotent. Returns 0 on success, otherwise an NTSTATUS-like
// negative code that has already been traced.
int Initialize(const config::Config& cfg);
void Shutdown();

}  // namespace win10xbox
