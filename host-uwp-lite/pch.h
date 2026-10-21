// pch.h - umbrella include for the two C++/WinRT translation units (main.cpp,
// App.cpp). Not a real precompiled header here: the standalone CMake build
// includes it explicitly, so smoke.cpp (plain Win32 + the win10xbox public API,
// no WinRT) never sees the winrt headers.
//
// The winrt/*.h projection headers come from the Windows SDK's
// Include/<ver>/cppwinrt directory, which vcvars puts on INCLUDE.
#pragma once
#include <windows.h>
#include <unknwn.h>   // before winrt/base.h: enables classic COM interop
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
