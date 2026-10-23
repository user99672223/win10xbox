// launcher.cpp - the on-console launcher, see launcher.h. C++/WinRT only for the gamepad
// (Windows.Gaming.Input); everything else is plain Win32 + STL.
#include "pch.h"

#include "launcher.h"

#include "host_util.h"

#include "win10xbox/gamepad.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

namespace w10x_uwp
{
namespace
{
    // The classic public-domain 5x7 ASCII font, characters 32..126. Five columns per glyph, bit 0
    // is the top row.
    const std::uint8_t kFont[95][5] = {
        {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
        {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
        {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
        {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
        {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
        {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
        {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
        {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
        {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
        {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
        {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
        {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
        {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
        {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
        {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
        {0x00, 0x7F, 0x10, 0x28, 0x44}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
        {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
        {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
        {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
        {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
        {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
        {0x00, 0x41, 0x36, 0x08, 0x00}, {0x10, 0x08, 0x08, 0x10, 0x08},
    };

    constexpr std::uint32_t kBg = 0xFF101820u;
    constexpr std::uint32_t kText = 0xFFE0E0E0u;
    constexpr std::uint32_t kDim = 0xFF8090A0u;
    constexpr std::uint32_t kTitle = 0xFFFFD040u;
    constexpr std::uint32_t kBar = 0xFF2A6FDBu;
    constexpr std::uint32_t kGood = 0xFF60D060u;
    constexpr std::uint32_t kBad = 0xFFFF6060u;

    enum Pad : unsigned { kPadUp = 1, kPadDown = 2, kPadA = 4, kPadB = 8 };

    struct Canvas
    {
        std::uint32_t* px;
        int w, h, scale;

        int Advance() const { return 6 * scale; }
        int LineHeight() const { return 10 * scale; }
        int Columns(int x) const { return std::max(0, (w - x) / Advance()); }

        void Fill(int x, int y, int fw, int fh, std::uint32_t c)
        {
            const int x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
            for (int yy = std::max(0, y); yy < y1; ++yy)
                for (int xx = std::max(0, x); xx < x1; ++xx) px[static_cast<std::size_t>(yy) * w + xx] = c;
        }

        void Text(int x, int y, const std::string& s, std::uint32_t c)
        {
            const std::size_t maxc = static_cast<std::size_t>(Columns(x));
            for (std::size_t i = 0; i < s.size() && i < maxc; ++i, x += Advance())
            {
                const unsigned char ch = static_cast<unsigned char>(s[i]);
                const std::uint8_t* g = kFont[(ch >= 32 && ch <= 126) ? ch - 32 : '?' - 32];
                for (int col = 0; col < 5; ++col)
                    for (int row = 0; row < 7; ++row)
                        if (g[col] & (1u << row)) Fill(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    };

    bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
}  // namespace

// ----------------------------------------------------------------------
std::vector<std::string> TailLines(const std::wstring& path, std::size_t n)
{
    std::vector<std::string> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    const LONGLONG keep = 64 * 1024;
    LARGE_INTEGER pos;
    pos.QuadPart = size.QuadPart > keep ? size.QuadPart - keep : 0;
    std::string text;
    if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
    {
        text.resize(static_cast<std::size_t>(size.QuadPart - pos.QuadPart));
        DWORD got = 0;
        if (!text.empty() && !ReadFile(h, &text[0], static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(h);
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else if (c != '\r')
        {
            const unsigned char u = static_cast<unsigned char>(c);
            cur.push_back(u == '\t' ? ' ' : (u < 32 || u > 126) ? '?' : c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    const std::size_t first = lines.size() > n ? lines.size() - n : 0;
    out.assign(lines.begin() + static_cast<std::ptrdiff_t>(first), lines.end());
    return out;
}

bool WriteLauncherResult(const std::wstring& path, const LauncherResult& r)
{
    std::string t;
    t += "guest=" + r.name + "\n";
    t += std::string("ran=") + (r.ran ? "yes" : "no") + "\n";
    t += "exit_code=" + std::to_string(r.exit_code) + "\n";
    t += "verdict=" + r.verdict + "\n";
    t += "stdout_last_lines=" + std::to_string(r.lines.size()) + "\n";
    for (const std::string& l : r.lines) t += "  | " + l + "\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    WriteAll(h, t.data(), t.size());
    FlushFileBuffers(h);
    CloseHandle(h);
    return true;
}

// ----------------------------------------------------------------------
Launcher::Launcher() = default;
Launcher::~Launcher() = default;

int Launcher::Pick(const std::vector<LauncherEntry>& entries, const std::string& preselect, const std::atomic<bool>& stop)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        entries_ = entries;
        sel_ = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (!preselect.empty() && IEquals(entries_[i].name, preselect)) sel_ = static_cast<int>(i);
        chosen_ = -1;
        mode_ = Mode::List;
        ++serial_;
    }
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (chosen_ >= 0) return chosen_;
        }
        if (stop.load()) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void Launcher::SetRunning()
{
    std::lock_guard<std::mutex> lock(mu_);
    mode_ = Mode::Idle;
    ++serial_;
}

void Launcher::ShowResult(const LauncherResult& r)
{
    std::lock_guard<std::mutex> lock(mu_);
    result_ = r;
    dismissed_ = false;
    mode_ = Mode::Result;
    ++serial_;
}

bool Launcher::WaitDismiss(const std::atomic<bool>& stop)
{
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (dismissed_) return true;
        }
        if (stop.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

bool Launcher::Consumes()
{
    std::lock_guard<std::mutex> lock(mu_);
    return mode_ != Mode::Idle;
}

void Launcher::Apply(Action a)
{
    if (mode_ == Mode::List)
    {
        const int n = static_cast<int>(entries_.size());
        if (a == kUp && n > 0) sel_ = (sel_ + n - 1) % n;
        else if (a == kDown && n > 0) sel_ = (sel_ + 1) % n;
        else if (a == kAccept && n > 0)
        {
            chosen_ = sel_;
            mode_ = Mode::Idle;   // the worker takes over (SetRunning follows)
        }
        ++serial_;
    }
    else if (mode_ == Mode::Result && (a == kAccept || a == kBack))
    {
        dismissed_ = true;
        mode_ = Mode::Idle;
        ++serial_;
    }
}

void Launcher::OnKey(unsigned vk)
{
    std::lock_guard<std::mutex> lock(mu_);
    switch (vk)
    {
    case 0x26: Apply(kUp); break;                 // VK_UP
    case 0x28: Apply(kDown); break;               // VK_DOWN
    case 0x0D: case 0x20: Apply(kAccept); break;  // VK_RETURN, VK_SPACE
    case 0x1B: case 0x08: Apply(kBack); break;    // VK_ESCAPE, VK_BACK
    default:
        if (pad_seen_) break;   // Windows.Gaming.Input has it
        switch (vk)             // VK_GAMEPAD_*
        {
        case 0xCB: case 0xD3: Apply(kUp); break;
        case 0xCC: case 0xD4: Apply(kDown); break;
        case 0xC3: Apply(kAccept); break;
        case 0xC4: Apply(kBack); break;
        default: break;
        }
        break;
    }
}

namespace
{
    // Windows.Gaming.Input -> the layer's gamepad store (win10xbox/gamepad.h), which the guest's XInput reads.
    // XINPUT_GAMEPAD_* bits: DPAD_UP 1, DOWN 2, LEFT 4, RIGHT 8, START 0x10, BACK 0x20, LEFT_THUMB 0x40,
    // RIGHT_THUMB 0x80, LEFT_SHOULDER 0x100, RIGHT_SHOULDER 0x200, A 0x1000, B 0x2000, X 0x4000, Y 0x8000.
    namespace wgi = winrt::Windows::Gaming::Input;

    std::uint16_t XInputButtons(wgi::GamepadButtons b)
    {
        const struct { wgi::GamepadButtons from; std::uint16_t to; } map[] = {
            {wgi::GamepadButtons::DPadUp, 0x0001}, {wgi::GamepadButtons::DPadDown, 0x0002},
            {wgi::GamepadButtons::DPadLeft, 0x0004}, {wgi::GamepadButtons::DPadRight, 0x0008},
            {wgi::GamepadButtons::Menu, 0x0010}, {wgi::GamepadButtons::View, 0x0020},
            {wgi::GamepadButtons::LeftThumbstick, 0x0040}, {wgi::GamepadButtons::RightThumbstick, 0x0080},
            {wgi::GamepadButtons::LeftShoulder, 0x0100}, {wgi::GamepadButtons::RightShoulder, 0x0200},
            {wgi::GamepadButtons::A, 0x1000}, {wgi::GamepadButtons::B, 0x2000},
            {wgi::GamepadButtons::X, 0x4000}, {wgi::GamepadButtons::Y, 0x8000}};
        std::uint16_t out = 0;
        for (const auto& m : map)
            if ((static_cast<unsigned>(b) & static_cast<unsigned>(m.from)) != 0) out = static_cast<std::uint16_t>(out | m.to);
        return out;
    }

    std::int16_t Stick(double v)   // -1..1 -> the signed-short range
    {
        v = std::max(-1.0, std::min(1.0, v));
        return static_cast<std::int16_t>(v < 0 ? v * 32768.0 : v * 32767.0);
    }

    std::uint8_t Trigger(double v) { return static_cast<std::uint8_t>(std::max(0.0, std::min(1.0, v)) * 255.0 + 0.5); }

    std::uint16_t g_last_left_motor = 0, g_last_right_motor = 0;   // the vibration last handed to the pad
}

void Launcher::PollGamepad()
{
    unsigned now = 0;
    try
    {
        const auto pads = wgi::Gamepad::Gamepads();
        if (pads.Size() == 0)
        {
            win10xbox::input::DisconnectGamepad(0);
            return;
        }
        pad_seen_ = true;
        const wgi::Gamepad pad = pads.GetAt(0);
        const wgi::GamepadReading r = pad.GetCurrentReading();
        const unsigned b = static_cast<unsigned>(r.Buttons);
        if ((b & static_cast<unsigned>(wgi::GamepadButtons::DPadUp)) != 0 || r.LeftThumbstickY > 0.6) now |= kPadUp;
        if ((b & static_cast<unsigned>(wgi::GamepadButtons::DPadDown)) != 0 || r.LeftThumbstickY < -0.6) now |= kPadDown;
        if ((b & static_cast<unsigned>(wgi::GamepadButtons::A)) != 0) now |= kPadA;
        if ((b & static_cast<unsigned>(wgi::GamepadButtons::B)) != 0) now |= kPadB;

        // The first pad is the guest's XInput user 0.
        W10xPadState st{};
        st.buttons = XInputButtons(r.Buttons);
        st.thumb_lx = Stick(r.LeftThumbstickX);
        st.thumb_ly = Stick(r.LeftThumbstickY);
        st.thumb_rx = Stick(r.RightThumbstickX);
        st.thumb_ry = Stick(r.RightThumbstickY);
        st.left_trigger = Trigger(r.LeftTrigger);
        st.right_trigger = Trigger(r.RightTrigger);
        win10xbox::input::SetGamepadState(0, st);

        // What the guest asked for with XInputSetState, handed to the pad when it changes.
        std::uint16_t left = 0, right = 0;
        win10xbox::input::GetGamepadVibration(0, &left, &right);
        if (left != g_last_left_motor || right != g_last_right_motor)
        {
            g_last_left_motor = left;
            g_last_right_motor = right;
            wgi::GamepadVibration v{};
            v.LeftMotor = left / 65535.0;
            v.RightMotor = right / 65535.0;
            pad.Vibration(v);
        }
    }
    catch (...)
    {
        return;
    }
    const unsigned edge = now & ~pad_prev_;
    pad_prev_ = now;
    if (edge == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (edge & kPadUp) Apply(kUp);
    if (edge & kPadDown) Apply(kDown);
    if (edge & kPadA) Apply(kAccept);
    if (edge & kPadB) Apply(kBack);
}

bool Launcher::Active()
{
    std::lock_guard<std::mutex> lock(mu_);
    return mode_ != Mode::Idle;
}

std::uint64_t Launcher::Serial()
{
    std::lock_guard<std::mutex> lock(mu_);
    return serial_;
}

void Launcher::Draw(std::uint32_t* bgra, int width, int height)
{
    std::lock_guard<std::mutex> lock(mu_);
    Canvas c{bgra, width, height, std::max(1, height / 270)};
    c.Fill(0, 0, width, height, kBg);
    const int margin = 6 * c.scale;
    const int lh = c.LineHeight();
    int y = margin;
    c.Text(margin, y, "win10xbox launcher", kTitle);
    y += lh * 2;

    if (mode_ == Mode::List)
    {
        const int footer = height - lh - margin;
        const int rows = std::max(1, (footer - y - lh) / lh);
        int first = 0;
        if (sel_ >= rows) first = sel_ - rows + 1;
        if (entries_.empty())
            c.Text(margin, y, "No guest programs found.", kBad);
        for (int i = first; i < static_cast<int>(entries_.size()) && i < first + rows; ++i, y += lh)
        {
            const bool on = i == sel_;
            if (on) c.Fill(margin / 2, y - c.scale * 2, width - margin, lh, kBar);
            c.Text(margin, y, (entries_[i].title.empty() ? entries_[i].name : entries_[i].title) + (entries_[i].from_localstate ? "   (LocalState\\guests)" : ""), on ? kText : kDim);
        }
        c.Text(margin, footer, "Up/Down: select    Enter or A: launch", kDim);
    }
    else if (mode_ == Mode::Result)
    {
        const int footer = height - lh - margin;
        c.Text(margin, y, "Guest: " + result_.name, kText);
        y += lh;
        const bool good = result_.ran && result_.verdict.compare(0, 4, "FAIL") != 0;
        c.Text(margin, y, result_.ran ? "Exit code: " + std::to_string(result_.exit_code) : std::string("The guest did not run"),
               good ? kGood : kBad);
        y += lh;
        c.Text(margin, y, result_.verdict, good ? kGood : kBad);
        y += lh * 2;
        c.Text(margin, y, "Last " + std::to_string(result_.lines.size()) + " line(s) of stdout:", kTitle);
        y += lh;
        for (const std::string& l : result_.lines)
        {
            if (y > footer - lh) break;
            c.Text(margin, y, l, kText);
            y += lh;
        }
        c.Text(margin, footer, "B or Esc or Enter: back to the list", kDim);
    }
}
}  // namespace w10x_uwp
