// Headless winexbox backend (docs/DECISIONS.md D8): windows are in-memory state,
// surfaces are malloc'd BGRA buffers, present is a no-op, input arrives through
// InjectInput(), and there is exactly one virtual monitor. No real display, no
// D3D. Implements the whole IWinexboxDriver contract so win32u can run on it.
#include "winexbox/driver.h"
#include "internal.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <variant>

WIN10XBOX_DECLARE_CHANNEL(winex);

namespace winexbox {
namespace {

Rect Intersect(const Rect& a, const Rect& b) {
    Rect r{std::max(a.left, b.left), std::max(a.top, b.top),
           std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
    if (r.Empty()) return Rect{};
    return r;
}

Rect Union(const Rect& a, const Rect& b) {
    if (a.Empty()) return b;
    if (b.Empty()) return a;
    return Rect{std::min(a.left, b.left), std::min(a.top, b.top),
                std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

struct SharedStats {
    // Guarded by its own mutex: surfaces may outlive the driver and flush late.
    std::mutex mu;
    HeadlessStats s;
};

class HeadlessSurface final : public ISurface {
public:
    HeadlessSurface(const Rect& rect, std::shared_ptr<SharedStats> stats)
        : rect_(rect), w_(rect.Width()), h_(rect.Height()), stats_(std::move(stats)) {
        stride_ = w_ * 4;
        bits_.assign(static_cast<size_t>(stride_) * static_cast<size_t>(h_), 0);
    }
    std::uint8_t* Bits() override { return bits_.data(); }
    std::int32_t Width() const override { return w_; }
    std::int32_t Height() const override { return h_; }
    std::int32_t Stride() const override { return stride_; }
    PixelFormat Format() const override { return PixelFormat::Bgra8888; }
    Rect SurfaceRect() const override { return rect_; }

    void Flush(const Rect* dirty) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            // Nothing to show anywhere: just retire the dirty region.
            dirty_ = Rect{};
        }
        (void)dirty;
        std::lock_guard<std::mutex> g(stats_->mu);
        ++stats_->s.flushes;
    }

    void MarkDirty(const Rect& r) {
        std::lock_guard<std::mutex> g(mu_);
        dirty_ = Union(dirty_, Intersect(r, Rect{0, 0, w_, h_}));
    }
    Rect Dirty() const {
        std::lock_guard<std::mutex> g(mu_);
        return dirty_;
    }

private:
    Rect rect_;
    std::int32_t w_, h_, stride_ = 0;
    std::vector<std::uint8_t> bits_;
    mutable std::mutex mu_;
    Rect dirty_;
    std::shared_ptr<SharedStats> stats_;
};

struct Window {
    WindowId parent = 0;
    WindowRects rects;
    std::uint32_t swp_flags = 0;
    bool visible = false;
    bool has_region = false;
    Region region;
    std::u16string text;
    std::uint32_t style = 0, ex_style = 0;
    bool is_desktop = false;
    std::shared_ptr<HeadlessSurface> surface;
};

struct QueuedWindowEvent {
    WindowId hwnd;
    IEventSink::WindowEvent ev;
    Rect rect;
};
using QueuedEvent = std::variant<InputEvent, QueuedWindowEvent>;

class HeadlessDriver final : public IWinexboxDriver {
public:
    explicit HeadlessDriver(const BackendConfig& cfg)
        : stats_(std::make_shared<SharedStats>()) {
        wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
        MonitorInfo m;
        m.device_name = "\\\\.\\DISPLAY1";
        m.primary = true;
        m.dpi = 96;
        m.current = DisplayMode{cfg.width, cfg.height, 32, cfg.refresh_hz};
        // Offer a few common modes next to the configured one.
        m.modes.push_back(m.current);
        const DisplayMode extra[] = {{3840, 2160, 32, 60}, {2560, 1440, 32, 60},
                                     {1920, 1080, 32, 60}, {1280, 720, 32, 60}};
        for (const auto& e : extra)
            if (std::find(m.modes.begin(), m.modes.end(), e) == m.modes.end()) m.modes.push_back(e);
        ApplyModeLocked(m, m.current);
        monitor_ = std::move(m);
        cursor_pos_ = Point{static_cast<std::int32_t>(cfg.width / 2), static_cast<std::int32_t>(cfg.height / 2)};
        TRACE(winex, "headless driver up: %ux%u@%u\n", cfg.width, cfg.height, cfg.refresh_hz);
    }
    ~HeadlessDriver() override {
        if (wake_event_) CloseHandle(wake_event_);
    }

    const char* Name() const override { return "headless"; }

    // ---- windows -----------------------------------------------------------
    bool WindowCreate(WindowId hwnd) override {
        if (!hwnd) return false;
        std::lock_guard<std::mutex> g(mu_);
        windows_[hwnd] = Window{};
        Bump(&HeadlessStats::windows_created);
        TRACE(winex, "hwnd %#llx\n", static_cast<unsigned long long>(hwnd));
        return true;
    }
    void WindowDestroy(WindowId hwnd) override {
        std::lock_guard<std::mutex> g(mu_);
        if (windows_.erase(hwnd)) Bump(&HeadlessStats::windows_destroyed);
        if (focus_ == hwnd) focus_ = 0;
        if (capture_ == hwnd) capture_ = 0;
        if (desktop_ == hwnd) desktop_ = 0;
        TRACE(winex, "hwnd %#llx\n", static_cast<unsigned long long>(hwnd));
    }
    void DesktopSetWindow(WindowId hwnd) override {
        std::lock_guard<std::mutex> g(mu_);
        desktop_ = hwnd;
        auto it = windows_.find(hwnd);
        if (it != windows_.end()) {
            it->second.is_desktop = true;
            it->second.visible = true;
            it->second.rects.window = it->second.rects.client = it->second.rects.visible = monitor_.monitor;
        }
    }
    bool DesktopCreate(const std::u16string&, std::uint32_t width, std::uint32_t height) override {
        std::lock_guard<std::mutex> g(mu_);
        // A new desktop only changes the nominal size if it is a supported mode.
        DisplayMode m = monitor_.current;
        m.width = width;
        m.height = height;
        if (std::find(monitor_.modes.begin(), monitor_.modes.end(), m) != monitor_.modes.end())
            ApplyModeLocked(monitor_, m);
        return true;
    }
    bool WindowPosChanging(WindowId hwnd, std::uint32_t, bool, const WindowRects& rects) override {
        (void)hwnd;
        // Windows with no visible area get no surface (win32u then skips drawing).
        return !rects.visible.Empty();
    }
    void WindowPosChanged(WindowId hwnd, WindowId, WindowId, std::uint32_t swp_flags,
                          const WindowRects& rects, ISurface* surface) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return;
        it->second.rects = rects;
        it->second.swp_flags = swp_flags;
        if (surface) {
            for (auto& s : surfaces_)
                if (s.get() == surface) it->second.surface = s;
        }
    }
    std::uint32_t WindowShow(WindowId hwnd, ShowCmd cmd, Rect*, std::uint32_t) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it != windows_.end()) it->second.visible = cmd != ShowCmd::Hide && cmd != ShowCmd::Minimize;
        return kShowDefault;
    }
    void WindowSetParent(WindowId hwnd, WindowId new_parent, WindowId) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it != windows_.end()) it->second.parent = new_parent;
    }
    void WindowSetStyle(WindowId hwnd, std::int32_t offset, std::uint32_t new_style, std::uint32_t) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return;
        if (offset == GWL_STYLE) it->second.style = new_style;
        else if (offset == GWL_EXSTYLE) it->second.ex_style = new_style;
    }
    void WindowSetText(WindowId hwnd, const std::u16string& text) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it != windows_.end()) it->second.text = text;
    }
    void WindowSetRegion(WindowId hwnd, const Region* region, bool) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return;
        it->second.has_region = region != nullptr;
        it->second.region = region ? *region : Region{};
    }
    void WindowSetLayered(WindowId, std::uint32_t, std::uint8_t, std::uint32_t) override {}
    void WindowActivate(WindowId hwnd, WindowId) override {
        std::lock_guard<std::mutex> g(mu_);
        focus_ = hwnd;
    }
    void WindowSetCapture(WindowId hwnd, std::uint32_t) override {
        std::lock_guard<std::mutex> g(mu_);
        capture_ = hwnd;
    }

    // ---- surfaces ------------------------------------------------------------
    std::shared_ptr<ISurface> CreateWindowSurface(WindowId hwnd, bool, const Rect& rect) override {
        if (rect.Empty()) return nullptr;
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) {
            WARN(winex, "surface for unknown hwnd %#llx\n", static_cast<unsigned long long>(hwnd));
            return nullptr;
        }
        auto s = std::make_shared<HeadlessSurface>(rect, stats_);
        surfaces_.push_back(s);
        it->second.surface = s;
        Bump(&HeadlessStats::surfaces_created);
        // Drop surfaces nobody references any more (only our list holds them).
        surfaces_.erase(std::remove_if(surfaces_.begin(), surfaces_.end(),
                                       [](const std::shared_ptr<HeadlessSurface>& p) { return p.use_count() == 1; }),
                        surfaces_.end());
        return s;
    }
    std::shared_ptr<ISurface> WindowSurface(WindowId hwnd) const override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        return it == windows_.end() ? nullptr : std::shared_ptr<ISurface>(it->second.surface);
    }
    DeviceCaps QueryDeviceCaps() const override {
        std::lock_guard<std::mutex> g(mu_);
        DeviceCaps c;
        c.horz_res = static_cast<std::int32_t>(monitor_.current.width);
        c.vert_res = static_cast<std::int32_t>(monitor_.current.height);
        c.horz_size_mm = c.horz_res * 254 / (96 * 10);
        c.vert_size_mm = c.vert_res * 254 / (96 * 10);
        c.bits_pixel = static_cast<std::int32_t>(monitor_.current.bits_per_pixel);
        c.log_pixels_x = c.log_pixels_y = static_cast<std::int32_t>(monitor_.dpi);
        c.vref_hz = static_cast<std::int32_t>(monitor_.current.refresh_hz);
        return c;
    }
    void DcAcquire(std::uint64_t, WindowId, WindowId, const Rect&, const Rect&, std::uint32_t) override {
        std::lock_guard<std::mutex> g(stats_->mu);
        ++stats_->s.dcs_outstanding;
    }
    void DcRelease(WindowId, std::uint64_t) override {
        std::lock_guard<std::mutex> g(stats_->mu);
        if (stats_->s.dcs_outstanding) --stats_->s.dcs_outstanding;
    }
    void Present() override {
        std::vector<std::shared_ptr<HeadlessSurface>> dirty;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& kv : windows_)
                if (kv.second.surface && !kv.second.surface->Dirty().Empty()) dirty.push_back(kv.second.surface);
        }
        for (auto& s : dirty) {
            Rect d = s->Dirty();
            s->Flush(&d);
        }
        Bump(&HeadlessStats::presents);
    }
    void Invalidate(WindowId hwnd, const Rect* window_rect) override {
        std::shared_ptr<HeadlessSurface> s;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = windows_.find(hwnd);
            if (it == windows_.end()) return;
            s = it->second.surface;
        }
        if (!s) return;
        Rect r{0, 0, s->Width(), s->Height()};
        if (window_rect) {
            // Window coords -> surface-local (surface rect is in window coords).
            Rect sr = s->SurfaceRect();
            r = Rect{window_rect->left - sr.left, window_rect->top - sr.top,
                     window_rect->right - sr.left, window_rect->bottom - sr.top};
        }
        s->MarkDirty(r);
    }

    // ---- message pump ------------------------------------------------------------
    void SetEventSink(IEventSink* sink) override {
        std::lock_guard<std::mutex> g(mu_);
        sink_ = sink;
    }
    bool ProcessEvents(std::uint32_t) override {
        std::deque<QueuedEvent> batch;
        IEventSink* sink;
        {
            std::lock_guard<std::mutex> g(mu_);
            batch.swap(queue_);
            sink = sink_;
        }
        bool any = false;
        for (auto& qe : batch) {
            if (!sink) break;   // nobody listening: drop, like an unmapped X11 event
            if (auto* in = std::get_if<InputEvent>(&qe)) {
                InputEvent ev = *in;
                if (!ev.target) ev.target = RouteTarget(ev);
                sink->OnInput(ev);
            } else {
                auto& we = std::get<QueuedWindowEvent>(qe);
                sink->OnWindowEvent(we.hwnd, we.ev, we.rect);
            }
            any = true;
            Bump(&HeadlessStats::events_delivered);
        }
        std::lock_guard<std::mutex> g(mu_);
        if (queue_.empty() && wake_event_) ResetEvent(wake_event_);
        return any;
    }
    void SetWakeCallback(WakeCallback cb, void* ctx) override {
        std::lock_guard<std::mutex> g(mu_);
        wake_cb_ = cb;
        wake_ctx_ = ctx;
    }
    void* WakeHandle() const override { return wake_event_; }
    void ThreadDetach() override {}

    // ---- input ---------------------------------------------------------------------
    void InjectInput(const InputEvent& ev) override {
        InputEvent e = ev;
        if (!e.time_ms) e.time_ms = static_cast<std::uint32_t>(GetTickCount64());
        Enqueue(e);
    }
    void InjectWindowEvent(WindowId hwnd, IEventSink::WindowEvent ev, const Rect& rect) override {
        Enqueue(QueuedWindowEvent{hwnd, ev, rect});
    }
    bool CursorGetPos(Point* pt) override {
        std::lock_guard<std::mutex> g(mu_);
        if (pt) *pt = cursor_pos_;
        return true;
    }
    bool CursorSetPos(std::int32_t x, std::int32_t y) override {
        std::lock_guard<std::mutex> g(mu_);
        cursor_pos_ = ClampCursorLocked(Point{x, y});
        return true;
    }
    bool CursorClip(const Rect* clip, bool reset) override {
        std::lock_guard<std::mutex> g(mu_);
        if (reset || !clip) {
            clip_active_ = false;
        } else {
            clip_ = *clip;
            clip_active_ = true;
            cursor_pos_ = ClampCursorLocked(cursor_pos_);
        }
        return true;
    }
    void CursorSet(WindowId, CursorId cursor) override {
        std::lock_guard<std::mutex> g(mu_);
        cursor_ = cursor;
    }
    void CursorDestroy(CursorId cursor) override {
        std::lock_guard<std::mutex> g(mu_);
        if (cursor_ == cursor) cursor_ = 0;
    }
    CursorId CursorCurrent() const override {
        std::lock_guard<std::mutex> g(mu_);
        return cursor_;
    }

    // ---- display -------------------------------------------------------------------
    DisplayConfig QueryDisplay() const override {
        std::lock_guard<std::mutex> g(mu_);
        DisplayConfig c;
        c.gpu.name = "winexbox Headless Adapter";
        c.gpu.pci_vendor = 0x1414;   // "Microsoft Basic Render Driver" ids
        c.gpu.pci_device = 0x008c;
        c.monitors.push_back(monitor_);
        return c;
    }
    std::int32_t ChangeDisplayMode(const std::string& device_name, const DisplayMode& mode) override {
        std::lock_guard<std::mutex> g(mu_);
        if (!device_name.empty() && device_name != monitor_.device_name) return -1;   // DISP_CHANGE_FAILED
        DisplayMode m = mode;
        if (!m.refresh_hz) m.refresh_hz = monitor_.current.refresh_hz;
        if (!m.bits_per_pixel) m.bits_per_pixel = 32;
        if (std::find(monitor_.modes.begin(), monitor_.modes.end(), m) == monitor_.modes.end())
            return -2;                                                              // DISP_CHANGE_BADMODE
        ApplyModeLocked(monitor_, m);
        auto d = windows_.find(desktop_);
        if (d != windows_.end()) d->second.rects.window = d->second.rects.client = d->second.rects.visible = monitor_.monitor;
        return 0;
    }

    // ---- introspection --------------------------------------------------------------
    HeadlessStats Stats() const {
        HeadlessStats s;
        {
            std::lock_guard<std::mutex> g(stats_->mu);
            s = stats_->s;
        }
        return s;
    }
    bool Info(WindowId hwnd, HeadlessWindowInfo* out) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return false;
        const Window& w = it->second;
        out->parent = w.parent;
        out->rects = w.rects;
        out->visible = w.visible;
        out->has_region = w.has_region;
        out->text = w.text;
        out->style = w.style;
        out->ex_style = w.ex_style;
        out->is_desktop = w.is_desktop;
        return true;
    }

private:
    static void ApplyModeLocked(MonitorInfo& m, const DisplayMode& mode) {
        m.current = mode;
        m.monitor = Rect{0, 0, static_cast<std::int32_t>(mode.width), static_cast<std::int32_t>(mode.height)};
        m.work = m.monitor;
    }
    Point ClampCursorLocked(Point p) const {
        Rect lim = clip_active_ ? clip_ : monitor_.monitor;
        p.x = std::min(std::max(p.x, lim.left), std::max(lim.left, lim.right - 1));
        p.y = std::min(std::max(p.y, lim.top), std::max(lim.top, lim.bottom - 1));
        return p;
    }
    WindowId RouteTarget(const InputEvent& ev) const {
        std::lock_guard<std::mutex> g(mu_);
        if (ev.kind == InputKind::KeyDown || ev.kind == InputKind::KeyUp) return focus_;
        return capture_ ? capture_ : focus_;
    }
    void Bump(std::uint64_t HeadlessStats::*field) {
        std::lock_guard<std::mutex> g(stats_->mu);
        ++(stats_->s.*field);
    }
    void Enqueue(QueuedEvent qe) {
        WakeCallback cb;
        void* ctx;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (auto* in = std::get_if<InputEvent>(&qe)) {
                if (in->kind == InputKind::MouseMove)
                    cursor_pos_ = ClampCursorLocked(Point{in->x, in->y});
            }
            queue_.push_back(std::move(qe));
            if (wake_event_) SetEvent(wake_event_);
            cb = wake_cb_;
            ctx = wake_ctx_;
        }
        Bump(&HeadlessStats::events_injected);
        if (cb) cb(ctx);
    }

    mutable std::mutex mu_;
    std::shared_ptr<SharedStats> stats_;
    std::map<WindowId, Window> windows_;
    std::vector<std::shared_ptr<HeadlessSurface>> surfaces_;
    std::deque<QueuedEvent> queue_;
    IEventSink* sink_ = nullptr;
    WakeCallback wake_cb_ = nullptr;
    void* wake_ctx_ = nullptr;
    HANDLE wake_event_ = nullptr;
    MonitorInfo monitor_;
    WindowId desktop_ = 0, focus_ = 0, capture_ = 0;
    Point cursor_pos_;
    Rect clip_;
    bool clip_active_ = false;
    CursorId cursor_ = 0;
};

const HeadlessDriver* AsHeadless(const IWinexboxDriver& d) {
    return std::strcmp(d.Name(), "headless") == 0 ? static_cast<const HeadlessDriver*>(&d) : nullptr;
}

}  // namespace

std::unique_ptr<IWinexboxDriver> CreateHeadlessDriver(const BackendConfig& cfg) {
    return std::make_unique<HeadlessDriver>(cfg);
}

HeadlessStats GetHeadlessStats(const IWinexboxDriver& drv) {
    const HeadlessDriver* h = AsHeadless(drv);
    return h ? h->Stats() : HeadlessStats{};
}

bool GetHeadlessWindowInfo(const IWinexboxDriver& drv, WindowId hwnd, HeadlessWindowInfo* out) {
    const HeadlessDriver* h = AsHeadless(drv);
    return h && out && h->Info(hwnd, out);
}

void FillSurfaceRect(ISurface& surface, const Rect& rect, std::uint32_t argb) {
    Rect r = Intersect(rect, Rect{0, 0, surface.Width(), surface.Height()});
    for (std::int32_t y = r.top; y < r.bottom; ++y) {
        std::uint32_t* row = reinterpret_cast<std::uint32_t*>(surface.Bits() + static_cast<size_t>(y) * static_cast<size_t>(surface.Stride()));
        for (std::int32_t x = r.left; x < r.right; ++x) row[x] = argb;
    }
    if (auto* hs = dynamic_cast<HeadlessSurface*>(&surface)) hs->MarkDirty(r);
}

std::uint32_t ReadSurfacePixel(const ISurface& surface, std::int32_t x, std::int32_t y) {
    if (x < 0 || y < 0 || x >= surface.Width() || y >= surface.Height()) return 0;
    std::uint32_t v;
    std::memcpy(&v, const_cast<ISurface&>(surface).Bits() + static_cast<size_t>(y) * static_cast<size_t>(surface.Stride()) + static_cast<size_t>(x) * 4, 4);
    return v;
}

}  // namespace winexbox
