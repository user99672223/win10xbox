// App.h - the IFrameworkViewSource handed to CoreApplication::Run().
//
// The matching runtimeclass Win10XboxUwp.App is declared in App.idl (midl turns
// it into Win10XboxUwp.winmd for the Xbox launcher); this C++ type is a plain
// C++/WinRT implementation of the same interface and is never activated through
// a WinRT factory. See App.idl.
#pragma once
#include "pch.h"

namespace w10x_uwp
{
    struct App : winrt::implements<App, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource>
    {
        winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView();
    };

    // Defined in main.cpp: the headless view that drives the Host (host.h).
    winrt::Windows::ApplicationModel::Core::IFrameworkView MakeView();
}
