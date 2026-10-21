// App.h - the IFrameworkViewSource handed to CoreApplication::Run().
//
// The matching runtimeclass Win10XboxLite.App is declared in App.idl (midl turns
// it into Win10XboxLite.winmd for the Xbox launcher); this C++ type is a plain
// C++/WinRT implementation of the same interface and is never activated through
// a WinRT factory. See App.idl.
#pragma once
#include "pch.h"

namespace uwp_lite
{
    struct App : winrt::implements<App, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource>
    {
        winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView();
    };

    // Defined in main.cpp: the headless view that runs the smoke on a worker thread.
    winrt::Windows::ApplicationModel::Core::IFrameworkView MakeView();
}
