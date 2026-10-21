// winexbox headless backend: windows, message pump, input injection, monitor
// query, software surface read-back. Case names start with "winexbox_" so
// `test_core winexbox_` (ctest unit.winexbox) runs just these.
#include "check.h"
#include "winexbox/driver.h"
#include <windows.h>
#include <vector>

namespace wx = winexbox;

namespace {

struct Recorder : wx::IEventSink {
    std::vector<wx::InputEvent> input;
    std::vector<std::pair<wx::WindowId, wx::IEventSink::WindowEvent>> window;
    void OnInput(const wx::InputEvent& ev) override { input.push_back(ev); }
    void OnWindowEvent(wx::WindowId h, WindowEvent e, const wx::Rect&) override { window.emplace_back(h, e); }
};

struct WakeCounter { int n = 0; };
void WakeCb(void* ctx) { ++static_cast<WakeCounter*>(ctx)->n; }

constexpr wx::WindowId kWinA = 0x10010;
constexpr wx::WindowId kWinB = 0x10020;
constexpr wx::WindowId kDesk = 0x10001;

wx::WindowRects RectsAt(int l, int t, int r, int b) {
    wx::WindowRects wr;
    wr.window = wr.client = wr.visible = wx::Rect{l, t, r, b};
    return wr;
}

}  // namespace

TEST(winexbox_select_backend_returns_headless) {
    wx::BackendConfig cfg;
    cfg.backend = win10xbox::config::DisplayBackend::Headless;
    auto d = wx::SelectBackend(cfg);
    CHECK(d != nullptr);
    CHECK_EQ(std::string(d->Name()), std::string("headless"));
    // D3D12 is not implemented yet: selection falls back to headless.
    cfg.backend = win10xbox::config::DisplayBackend::D3D12;
    auto d2 = wx::SelectBackend(cfg);
    CHECK(d2 != nullptr);
    CHECK_EQ(std::string(d2->Name()), std::string("headless"));
}

TEST(winexbox_monitor_query) {
    auto d = wx::CreateHeadlessDriver();
    wx::DisplayConfig dc = d->QueryDisplay();
    CHECK_EQ(dc.monitors.size(), (size_t)1);
    const wx::MonitorInfo& m = dc.monitors[0];
    CHECK(m.primary);
    CHECK_EQ(m.monitor.Width(), 1920);
    CHECK_EQ(m.monitor.Height(), 1080);
    CHECK(m.work == m.monitor);
    CHECK_EQ(m.current.width, 1920u);
    CHECK_EQ(m.current.height, 1080u);
    CHECK_EQ(m.current.bits_per_pixel, 32u);
    CHECK(!m.modes.empty());
    CHECK(!dc.gpu.name.empty());

    wx::DeviceCaps caps = d->QueryDeviceCaps();
    CHECK_EQ(caps.horz_res, 1920);
    CHECK_EQ(caps.vert_res, 1080);
    CHECK_EQ(caps.bits_pixel, 32);

    // Mode switch: supported mode succeeds, unsupported reports BADMODE (-2).
    CHECK_EQ(d->ChangeDisplayMode("", wx::DisplayMode{1280, 720, 32, 60}), 0);
    CHECK_EQ(d->QueryDisplay().monitors[0].monitor.Width(), 1280);
    CHECK_EQ(d->QueryDeviceCaps().vert_res, 720);
    CHECK_EQ(d->ChangeDisplayMode("", wx::DisplayMode{123, 45, 32, 60}), -2);
    CHECK_EQ(d->QueryDisplay().monitors[0].monitor.Width(), 1280);
}

TEST(winexbox_window_lifecycle) {
    auto d = wx::CreateHeadlessDriver();
    CHECK(!d->WindowCreate(0));
    CHECK(d->WindowCreate(kDesk));
    d->DesktopSetWindow(kDesk);
    CHECK(d->WindowCreate(kWinA));
    CHECK(d->WindowCreate(kWinB));

    wx::HeadlessWindowInfo info;
    CHECK(wx::GetHeadlessWindowInfo(*d, kDesk, &info));
    CHECK(info.is_desktop);
    CHECK_EQ(info.rects.window.Width(), 1920);

    wx::WindowRects wr = RectsAt(100, 50, 500, 350);
    CHECK(d->WindowPosChanging(kWinA, 0, false, wr));
    CHECK(!d->WindowPosChanging(kWinA, 0, false, RectsAt(0, 0, 0, 0)));
    d->WindowSetParent(kWinA, kDesk, 0);
    d->WindowSetText(kWinA, u"hello");
    d->WindowSetStyle(kWinA, GWL_STYLE, 0x10CF0000u, 0);
    CHECK_EQ(d->WindowShow(kWinA, wx::ShowCmd::Show, nullptr, 0), wx::kShowDefault);
    auto surf = d->CreateWindowSurface(kWinA, false, wr.visible);
    CHECK(surf != nullptr);
    d->WindowPosChanged(kWinA, 0, 0, 0, wr, surf.get());

    CHECK(wx::GetHeadlessWindowInfo(*d, kWinA, &info));
    CHECK(info.visible);
    CHECK_EQ(info.parent, kDesk);
    CHECK(info.text == u"hello");
    CHECK_EQ(info.style, 0x10CF0000u);
    CHECK(info.rects.client == wr.client);

    wx::Region rgn;
    rgn.rects.push_back(wx::Rect{0, 0, 10, 10});
    d->WindowSetRegion(kWinA, &rgn, true);
    CHECK(wx::GetHeadlessWindowInfo(*d, kWinA, &info) && info.has_region);
    d->WindowSetRegion(kWinA, nullptr, true);
    CHECK(wx::GetHeadlessWindowInfo(*d, kWinA, &info) && !info.has_region);

    d->WindowDestroy(kWinA);
    CHECK(!wx::GetHeadlessWindowInfo(*d, kWinA, &info));
    CHECK(d->WindowSurface(kWinA) == nullptr);
    CHECK(surf->Bits() != nullptr);   // the caller's reference keeps the pixels alive
    wx::HeadlessStats st = wx::GetHeadlessStats(*d);
    CHECK_EQ(st.windows_created, 3u);
    CHECK_EQ(st.windows_destroyed, 1u);
}

TEST(winexbox_key_event_pumped_to_focus_window) {
    auto d = wx::CreateHeadlessDriver();
    CHECK(d->WindowCreate(kWinA));
    CHECK(d->WindowCreate(kWinB));
    d->WindowActivate(kWinB, kWinA);

    Recorder rec;
    d->SetEventSink(&rec);
    WakeCounter wake;
    d->SetWakeCallback(&WakeCb, &wake);

    CHECK(d->WakeHandle() != nullptr);
    CHECK_EQ(WaitForSingleObject(static_cast<HANDLE>(d->WakeHandle()), 0), (DWORD)WAIT_TIMEOUT);
    CHECK(!d->ProcessEvents(0));          // nothing queued yet

    wx::InputEvent ev;
    ev.kind = wx::InputKind::KeyDown;
    ev.vk = 'A';
    ev.scan = 0x1E;
    d->InjectInput(ev);
    ev.kind = wx::InputKind::KeyUp;
    d->InjectInput(ev);

    CHECK_EQ(wake.n, 2);
    CHECK(rec.input.empty());             // not delivered until pumped
    CHECK_EQ(WaitForSingleObject(static_cast<HANDLE>(d->WakeHandle()), 0), (DWORD)WAIT_OBJECT_0);

    CHECK(d->ProcessEvents(QS_ALLINPUT));
    CHECK_EQ(rec.input.size(), (size_t)2);
    if (rec.input.size() == 2) {
        CHECK(rec.input[0].kind == wx::InputKind::KeyDown);
        CHECK(rec.input[1].kind == wx::InputKind::KeyUp);
        CHECK_EQ(rec.input[0].vk, (std::uint16_t)'A');
        CHECK_EQ(rec.input[0].scan, (std::uint16_t)0x1E);
        CHECK_EQ(rec.input[0].target, kWinB);     // routed to the focus window
        CHECK(rec.input[0].time_ms != 0);
    }
    // Queue drained: wake event reset, a second pump delivers nothing.
    CHECK_EQ(WaitForSingleObject(static_cast<HANDLE>(d->WakeHandle()), 0), (DWORD)WAIT_TIMEOUT);
    CHECK(!d->ProcessEvents(QS_ALLINPUT));
    CHECK_EQ(wx::GetHeadlessStats(*d).events_delivered, 2u);

    // Window events share the same queue.
    d->InjectWindowEvent(kWinA, wx::IEventSink::WindowEvent::CloseRequested, wx::Rect{});
    CHECK(d->ProcessEvents(0));
    CHECK_EQ(rec.window.size(), (size_t)1);
}

TEST(winexbox_mouse_moves_cursor_and_clips) {
    auto d = wx::CreateHeadlessDriver();
    Recorder rec;
    d->SetEventSink(&rec);

    wx::InputEvent mv;
    mv.kind = wx::InputKind::MouseMove;
    mv.x = 640;
    mv.y = 360;
    d->InjectInput(mv);
    wx::Point p;
    CHECK(d->CursorGetPos(&p));
    CHECK_EQ(p.x, 640);
    CHECK_EQ(p.y, 360);

    // Off-screen positions clamp to the monitor.
    mv.x = 5000;
    mv.y = -30;
    d->InjectInput(mv);
    d->CursorGetPos(&p);
    CHECK_EQ(p.x, 1919);
    CHECK_EQ(p.y, 0);

    wx::Rect clip{100, 100, 200, 200};
    CHECK(d->CursorClip(&clip, false));
    d->CursorGetPos(&p);
    CHECK_EQ(p.x, 199);
    CHECK_EQ(p.y, 100);
    CHECK(d->CursorClip(nullptr, true));
    CHECK(d->CursorSetPos(10, 20));
    d->CursorGetPos(&p);
    CHECK_EQ(p.x, 10);

    d->CursorSet(0, 0x4242);
    CHECK_EQ(d->CursorCurrent(), (wx::CursorId)0x4242);
    d->CursorDestroy(0x4242);
    CHECK_EQ(d->CursorCurrent(), (wx::CursorId)0);
    CHECK(d->ProcessEvents(0));
    CHECK_EQ(rec.input.size(), (size_t)2);
}

TEST(winexbox_surface_draw_and_read_back) {
    auto d = wx::CreateHeadlessDriver();
    CHECK(d->WindowCreate(kWinA));
    wx::Rect area{200, 100, 456, 228};   // 256x128 window
    CHECK(d->CreateWindowSurface(kWinB, false, area) == nullptr);   // unknown window
    CHECK(d->CreateWindowSurface(kWinA, false, wx::Rect{}) == nullptr);   // empty rect

    auto s = d->CreateWindowSurface(kWinA, false, area);
    CHECK(s != nullptr);
    CHECK_EQ(s->Width(), 256);
    CHECK_EQ(s->Height(), 128);
    CHECK_EQ(s->Stride(), 256 * 4);
    CHECK(s->Format() == wx::PixelFormat::Bgra8888);
    CHECK(s->SurfaceRect() == area);
    CHECK(d->WindowSurface(kWinA).get() == s.get());
    CHECK_EQ(wx::ReadSurfacePixel(*s, 10, 10), 0u);   // zero-initialised

    wx::FillSurfaceRect(*s, wx::Rect{10, 20, 60, 70}, 0xFF3366CCu);
    CHECK_EQ(wx::ReadSurfacePixel(*s, 10, 20), 0xFF3366CCu);
    CHECK_EQ(wx::ReadSurfacePixel(*s, 59, 69), 0xFF3366CCu);
    CHECK_EQ(wx::ReadSurfacePixel(*s, 60, 69), 0u);   // right edge exclusive
    CHECK_EQ(wx::ReadSurfacePixel(*s, 9, 20), 0u);
    // Byte order is B,G,R,A in memory.
    const std::uint8_t* px = s->Bits() + 20 * s->Stride() + 10 * 4;
    CHECK_EQ(px[0], (std::uint8_t)0xCC);
    CHECK_EQ(px[1], (std::uint8_t)0x66);
    CHECK_EQ(px[2], (std::uint8_t)0x33);
    CHECK_EQ(px[3], (std::uint8_t)0xFF);

    // A fill larger than the surface clips instead of overrunning.
    wx::FillSurfaceRect(*s, wx::Rect{-50, -50, 5000, 5000}, 0xFF000001u);
    CHECK_EQ(wx::ReadSurfacePixel(*s, 255, 127), 0xFF000001u);

    // Present is a no-op that only retires dirty state.
    wx::HeadlessStats before = wx::GetHeadlessStats(*d);
    d->Invalidate(kWinA, nullptr);
    d->Present();
    wx::HeadlessStats after = wx::GetHeadlessStats(*d);
    CHECK_EQ(after.presents, before.presents + 1);
    CHECK_EQ(after.flushes, before.flushes + 1);
    d->Present();                                   // nothing dirty any more
    CHECK_EQ(wx::GetHeadlessStats(*d).flushes, after.flushes);
    CHECK_EQ(wx::ReadSurfacePixel(*s, 255, 127), 0xFF000001u);   // pixels untouched
}
