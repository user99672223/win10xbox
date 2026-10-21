// probe_payload_stub.cpp - placeholder keeping category A's
// loadlibrary-out-of-package / sec-image-map probes well-defined on UWP, where
// shipping a separate .dll inside the appx is a follow-up. The real
// probe_payload.dll ships only with the win32 host; on UWP these probes just
// report SKIPPED when the file isn't present (per probe_A_dynamic_code.cpp).
// This TU exists so the UWP vcxproj doesn't need special-casing for one file.
#include "pch.h"
#include <windows.h>
BOOL APIENTRY probe_payload_stub_touch() { return TRUE; }
