// App.h - implementation of the XboxProbe.App runtimeclass declared in App.idl.
// The C++/WinRT codegen (cppwinrt) consumes App.idl + App.cpp and emits the
// activation-factory glue so Xbox's launcher can create our class via the
// standard WinRT activation path. Our App::CreateView() returns the real
// IFrameworkView (defined in main.cpp as struct View) that owns the probe
// thread + the D2D scrolling renderer on CoreWindow.
#pragma once
#include "App.g.h"

namespace winrt::XboxProbe::implementation
{
    struct App : AppT<App>
    {
        App() = default;
        Windows::ApplicationModel::Core::IFrameworkView CreateView();
    };
}

namespace winrt::XboxProbe::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}
