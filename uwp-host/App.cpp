// App.cpp - CreateView() returns the real IFrameworkView implementation.
// winrt_make_XboxProbe_App_implementation() is emitted by cppwinrt from App.idl
// and lets the factory instantiate us.
#include "pch.h"
#include "App.h"
#include "App.g.cpp"

// Declared in main.cpp: our actual CoreWindow + D2D scrolling renderer.
namespace probe_host { winrt::Windows::ApplicationModel::Core::IFrameworkView make_view(); }

namespace winrt::XboxProbe::implementation
{
    winrt::Windows::ApplicationModel::Core::IFrameworkView App::CreateView()
    {
        return probe_host::make_view();
    }
}

// Entry point: C++/WinRT activation metadata tells Xbox's launcher WHICH class
// to represent as the app tile, but the launched exe still runs its own startup.
// wWinMain is where we hand control to CoreApplication::Run(App), which the
// Windows runtime then calls back into for CreateView / SetWindow / Run.
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(
        winrt::make<winrt::XboxProbe::implementation::App>());
    return 0;
}
