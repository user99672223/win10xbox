// pch.h - umbrella include for the two C++/WinRT translation units (main.cpp,
// App.cpp). Not a real precompiled header here: the standalone CMake build
// includes it explicitly, so every other source of host-uwp (host.cpp, host_*.cpp,
// launch_config.cpp, crash.cpp: plain Win32 + the win10xbox public API, no WinRT)
// never sees the winrt headers.
//
// The winrt/*.h projection headers come from the Windows SDK's
// Include/<ver>/cppwinrt directory, which vcvars puts on INCLUDE.
//
// Beyond host-uwp-lite's list: Windows.UI.Input (PointerPoint, for the pointer
// events main.cpp forwards to the winexbox backend), Windows.System (VirtualKey,
// for the key events), Windows.ApplicationModel.Activation (the activation note
// in trace.log) and Windows.Graphics.Display (DisplayInformation: view pixels ->
// physical pixels for the swap chain and the virtual monitor). Every namespace main.cpp calls into is included explicitly rather
// than relied on transitively.
#pragma once
#include <windows.h>
#include <unknwn.h>   // before winrt/base.h: enables classic COM interop
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
