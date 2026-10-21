// App.cpp - CreateView() and the process entry point. Same flow as host-uwp-lite's
// and the probe's tools/probe/uwp-host/App.cpp: wWinMain hands control to
// CoreApplication::Run(), which calls back into CreateView / SetWindow / Run.
#include "pch.h"
#include "App.h"

namespace w10x_uwp
{
    winrt::Windows::ApplicationModel::Core::IFrameworkView App::CreateView()
    {
        return MakeView();
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<w10x_uwp::App>());
    return 0;
}
