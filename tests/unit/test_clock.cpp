#include "check.h"
#include "win10xbox/clock.h"
#include "win10xbox/config.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace clk = win10xbox::clock;

static std::int64_t HostSys() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (std::int64_t)(((std::uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}
static std::int64_t Abs64(std::int64_t v) { return v < 0 ? -v : v; }

TEST(clock_frequency_is_fixed_default) {
    clk::Initialize(clk::Settings{});
    CHECK_EQ(clk::QpcFrequency(), 10'000'000ull);
    clk::Settings s = clk::GetSettings();
    s.scale = 3.0;
    clk::SetSettings(s);
    CHECK_EQ(clk::QpcFrequency(), 10'000'000ull);
    LARGE_INTEGER c, f;
    CHECK_EQ(clk::Clk_NtQueryPerformanceCounter(&c, &f), 0);
    CHECK_EQ((std::uint64_t)f.QuadPart, 10'000'000ull);
    CHECK(c.QuadPart > 0 || c.QuadPart == 0);
    clk::Initialize(clk::Settings{});
}

TEST(clock_qpc_monotonic_across_reads_and_scale_change) {
    clk::Initialize(clk::Settings{});
    std::uint64_t prev = clk::Qpc();
    bool ok = true;
    const double scales[] = {1.0, 8.0, 0.25, 1.0, 100.0, 0.5};
    for (double sc : scales) {
        clk::Settings s = clk::GetSettings();
        s.scale = sc;
        clk::SetSettings(s);
        for (int i = 0; i < 2000; ++i) {
            std::uint64_t q = clk::Qpc();
            if (q < prev) ok = false;
            prev = q;
        }
    }
    CHECK(ok);
    // Rdtsc and interrupt time are continuous too.
    std::uint64_t t0 = clk::Rdtsc(), i0 = clk::InterruptTime100ns();
    clk::Settings s = clk::GetSettings();
    s.scale = 0.1;
    clk::SetSettings(s);
    CHECK(clk::Rdtsc() >= t0);
    CHECK(clk::InterruptTime100ns() >= i0);
    clk::Initialize(clk::Settings{});
}

TEST(clock_scale_changes_rate) {
    clk::Initialize(clk::Settings{});
    clk::Settings s = clk::GetSettings();
    s.scale = 4.0;
    clk::SetSettings(s);
    std::uint64_t q0 = clk::Qpc();
    Sleep(100);
    std::uint64_t dq = clk::Qpc() - q0;
    // ~100ms host * 4 = 4,000,000 ticks of 10MHz; allow generous scheduler noise.
    CHECK_MSG(dq > 3'500'000ull && dq < 12'000'000ull, "dq=%llu", (unsigned long long)dq);
    clk::Initialize(clk::Settings{});
}

TEST(clock_system_time_tracks_host_and_offset_shifts_it) {
    clk::Initialize(clk::Settings{});
    std::int64_t a = clk::SystemTime100ns(), h = HostSys();
    CHECK_MSG(Abs64(a - h) < 500'000, "diff=%lld", (long long)(a - h));   // 50 ms
    clk::Settings s = clk::GetSettings();
    s.offset_100ns = 600'000'000;   // +60 s
    clk::SetSettings(s);
    std::int64_t b = clk::SystemTime100ns();
    h = HostSys();
    CHECK_MSG(Abs64(b - h - 600'000'000) < 500'000, "diff=%lld", (long long)(b - h - 600'000'000));
    // offset does not move QPC/interrupt time
    std::uint64_t i0 = clk::InterruptTime100ns();
    s.offset_100ns = -600'000'000;
    clk::SetSettings(s);
    CHECK(clk::InterruptTime100ns() - i0 < 500'000ull);
    clk::Initialize(clk::Settings{});
}

TEST(clock_set_system_time_adjusts_offset_only) {
    clk::Initialize(clk::Settings{});
    LARGE_INTEGER target, old;
    target.QuadPart = HostSys() + 36'000'000'000ll;   // +1 hour
    CHECK_EQ(clk::Clk_NtSetSystemTime(&target, &old), 0);
    CHECK(Abs64(old.QuadPart - HostSys()) < 500'000);
    LARGE_INTEGER now;
    clk::Clk_NtQuerySystemTime(&now);
    CHECK_MSG(Abs64(now.QuadPart - target.QuadPart) < 500'000, "d=%lld", (long long)(now.QuadPart - target.QuadPart));
    CHECK(Abs64(clk::GetSettings().offset_100ns - 36'000'000'000ll) < 500'000);
    CHECK(Abs64(HostSys() - clk::SystemTime100ns()) > 30'000'000'000ll);   // host untouched
    target.QuadPart = 0;
    CHECK(clk::Clk_NtSetSystemTime(&target, nullptr) != 0);
    clk::Initialize(clk::Settings{});
}

TEST(clock_freeze_holds_time_still) {
    clk::Initialize(clk::Settings{});
    Sleep(5);
    clk::Freeze(true);
    std::uint64_t q = clk::Qpc(), t = clk::Rdtsc(), i = clk::InterruptTime100ns(), k = clk::TickCountMs();
    std::int64_t s = clk::SystemTime100ns();
    Sleep(60);
    CHECK_EQ(clk::Qpc(), q);
    CHECK_EQ(clk::Rdtsc(), t);
    CHECK_EQ(clk::InterruptTime100ns(), i);
    CHECK_EQ(clk::TickCountMs(), k);
    CHECK_EQ(clk::SystemTime100ns(), s);
    CHECK(clk::GetSettings().frozen);
    clk::Freeze(false);
    Sleep(30);
    std::uint64_t q2 = clk::Qpc();
    CHECK(q2 > q);
    CHECK(q2 - q < 3'000'000ull);   // time while frozen was not counted (60ms = 600000 ticks)
    clk::Initialize(clk::Settings{});
}

TEST(clock_tick_count_and_interrupt_time_increase) {
    clk::Settings st;
    st.initial_uptime_100ns = 36'000'000'000ull;   // 1 h
    clk::Initialize(st);
    std::uint64_t t0 = clk::TickCountMs();
    CHECK(t0 >= 3'600'000ull && t0 < 3'700'000ull);
    CHECK(clk::BootTime100ns() <= clk::SystemTime100ns() - 36'000'000'000ll + 1000);
    Sleep(50);
    std::uint64_t t1 = clk::TickCountMs();
    CHECK(t1 > t0);
    ULONG tc = 0;
    CHECK_EQ(clk::Clk_NtGetTickCount(&tc), 0);
    CHECK(tc >= t1);
    clk::Initialize(clk::Settings{});
}

TEST(clock_rdtsc_follows_tsc_frequency) {
    clk::Initialize(clk::Settings{});
    std::uint64_t f = clk::TscFrequency();
    CHECK(f >= 500'000'000ull && f <= 8'000'000'000ull);
    std::uint64_t a = clk::Rdtsc();
    Sleep(50);
    std::uint64_t b = clk::Rdtsc();
    double secs = double(b - a) / double(f);
    CHECK_MSG(secs > 0.04 && secs < 0.30, "secs=%f", secs);
    // back-to-back reads strictly differ often enough to look like a real TSC
    int distinct = 0;
    std::uint64_t p = clk::Rdtsc();
    for (int i = 0; i < 1000; ++i) { std::uint64_t n = clk::Rdtsc(); if (n != p) ++distinct; CHECK(n >= p); p = n; }
    CHECK(distinct > 500);
    clk::Settings s;
    s.tsc_frequency = 2'000'000'000ull;
    clk::Initialize(s);
    CHECK_EQ(clk::TscFrequency(), 2'000'000'000ull);
    clk::Initialize(clk::Settings{});
}

TEST(clock_translate_timeout) {
    clk::Initialize(clk::Settings{});
    LARGE_INTEGER in, out;
    CHECK(clk::TranslateTimeout(nullptr, &out) == nullptr);
    clk::Settings s = clk::GetSettings();
    s.scale = 2.0;
    clk::SetSettings(s);
    in.QuadPart = -10'000'000;
    PLARGE_INTEGER r = clk::TranslateTimeout(&in, &out);
    CHECK(r == &out);
    CHECK_EQ(out.QuadPart, -5'000'000ll);
    in.QuadPart = -1;   // never rounds a real wait down to a poll
    clk::TranslateTimeout(&in, &out);
    CHECK_EQ(out.QuadPart, -1ll);
    in.QuadPart = 0;
    clk::TranslateTimeout(&in, &out);
    CHECK_EQ(out.QuadPart, 0ll);
    in.QuadPart = INT64_MIN;
    clk::TranslateTimeout(&in, &out);
    CHECK_EQ(out.QuadPart, INT64_MIN);
    s.scale = 0.5;
    clk::SetSettings(s);
    in.QuadPart = -10'000'000;
    clk::TranslateTimeout(&in, &out);
    CHECK_EQ(out.QuadPart, -20'000'000ll);
    // absolute: guest now + 10s at scale 2 -> host now + 5s
    s.scale = 2.0;
    s.offset_100ns = 123'000'000'000ll;
    clk::SetSettings(s);
    in.QuadPart = clk::SystemTime100ns() + 100'000'000;
    clk::TranslateTimeout(&in, &out);
    CHECK_MSG(Abs64(out.QuadPart - (HostSys() + 50'000'000)) < 1'000'000, "d=%lld", (long long)(out.QuadPart - HostSys()));
    in.QuadPart = clk::SystemTime100ns() - 100'000'000;   // expired
    clk::TranslateTimeout(&in, &out);
    CHECK(out.QuadPart <= HostSys() + 10'000);
    clk::Initialize(clk::Settings{});
}

TEST(clock_delay_execution_scales_timeout) {
    clk::Settings s;
    s.scale = 10.0;
    clk::Initialize(s);
    LARGE_INTEGER t;
    t.QuadPart = -5'000'000;   // 500ms guest = 50ms host
    DWORD a = GetTickCount();
    CHECK_EQ(clk::Clk_NtDelayExecution(FALSE, &t), 0);
    DWORD d = GetTickCount() - a;
    CHECK_MSG(d >= 30 && d < 300, "d=%lu", d);
    clk::Initialize(clk::Settings{});
}

TEST(clock_timer_resolution) {
    clk::Initialize(clk::Settings{});
    ULONG mn = 0, mx = 0, cur = 0, act = 0;
    CHECK_EQ(clk::Clk_NtQueryTimerResolution(&mn, &mx, &cur), 0);
    CHECK(mn >= mx && mx == 5000 && cur == mn);
    CHECK_EQ(clk::Clk_NtSetTimerResolution(10000, TRUE, &act), 0);
    CHECK_EQ(act, 10000u);
    clk::Clk_NtQueryTimerResolution(&mn, &mx, &cur);
    CHECK_EQ(cur, 10000u);
    clk::Clk_NtSetTimerResolution(1, TRUE, &act);
    CHECK_EQ(act, 5000u);
    clk::Clk_NtSetTimerResolution(0, FALSE, &act);
    CHECK_EQ(act, mn);
}

TEST(clock_time_of_day_information) {
    clk::Initialize(clk::Settings{});
    std::uint8_t buf[0x30];
    std::memset(buf, 0xCC, sizeof buf);
    ULONG ret = 0;
    CHECK_EQ(clk::Clk_NtQuerySystemInformation_TimeOfDay(buf, sizeof buf, &ret), 0);
    CHECK_EQ(ret, 0x30u);
    std::int64_t boot, cur, bias;
    std::memcpy(&boot, buf, 8); std::memcpy(&cur, buf + 8, 8); std::memcpy(&bias, buf + 16, 8);
    CHECK(Abs64(cur - HostSys()) < 500'000);
    CHECK(boot < cur && boot > 0);
    CHECK_EQ(bias, 0ll);
    CHECK(clk::Clk_NtQuerySystemInformation_TimeOfDay(buf, 8, &ret) != 0);
}

TEST(clock_shared_data_tracks_virtual_time) {
    clk::Initialize(clk::Settings{});
    CHECK(clk::VirtualSharedData() == nullptr || clk::VirtualSharedData() != nullptr);   // may exist from an earlier run
    CHECK_EQ(clk::StartSharedDataThread(), 0);
    const std::uint8_t* p = (const std::uint8_t*)clk::VirtualSharedData();
    CHECK(p != nullptr);
    if (!p) return;
    auto rd64 = [&](std::uint32_t off) { std::uint64_t v; std::memcpy(&v, p + off, 8); return v; };
    auto rd32 = [&](std::uint32_t off) { std::uint32_t v; std::memcpy(&v, p + off, 4); return v; };
    // KSYSTEM_TIME consistency: High1Time == High2Time and the quad matches.
    CHECK_EQ(rd32(clk::usd::kSystemTime + 4), rd32(clk::usd::kSystemTime + 8));
    std::int64_t sys0 = (std::int64_t)rd64(clk::usd::kSystemTime);
    CHECK_MSG(Abs64(sys0 - clk::SystemTime100ns()) < 2'000'000, "d=%lld", (long long)(sys0 - clk::SystemTime100ns()));
    Sleep(100);
    std::int64_t sys1 = (std::int64_t)rd64(clk::usd::kSystemTime);
    CHECK_MSG(sys1 - sys0 > 500'000 && sys1 - sys0 < 5'000'000, "adv=%lld", (long long)(sys1 - sys0));
    CHECK_MSG(Abs64(sys1 - clk::SystemTime100ns()) < 2'000'000, "d=%lld", (long long)(sys1 - clk::SystemTime100ns()));
    std::uint64_t it = rd64(clk::usd::kInterruptTime);
    CHECK(Abs64((std::int64_t)it - (std::int64_t)clk::InterruptTime100ns()) < 2'000'000);
    CHECK_EQ(rd32(clk::usd::kTickCountMultiplier), 0x0FA00000u);
    // GetTickCount64 formula the guest uses: (TickCountQuad * multiplier) >> 24
    std::uint64_t ms = (rd64(clk::usd::kTickCount) * rd32(clk::usd::kTickCountMultiplier)) >> 24;
    std::uint64_t vms = clk::TickCountMs();
    CHECK_MSG(ms <= vms + 5 && vms - ms < 100, "ms=%llu vms=%llu", (unsigned long long)ms, (unsigned long long)vms);
    CHECK_EQ(rd64(clk::usd::kQpcFrequency), 10'000'000ull);
    CHECK_EQ(rd32(clk::usd::kTimeZoneBias), 0u);
    const auto& cfg = win10xbox::config::Current();
    CHECK_EQ(rd32(clk::usd::kNtMajorVersion), cfg.os_major);
    CHECK_EQ(rd32(clk::usd::kNtMinorVersion), cfg.os_minor);
    CHECK_EQ(rd32(clk::usd::kNtBuildNumber), cfg.os_build);
    // an offset change reaches the page
    clk::Settings s = clk::GetSettings();
    s.offset_100ns = 3'000'000'000ll;
    clk::SetSettings(s);
    Sleep(50);
    std::int64_t sys2 = (std::int64_t)rd64(clk::usd::kSystemTime);
    CHECK(sys2 - sys1 > 2'900'000'000ll);
    clk::StopSharedDataThread();
    std::int64_t frozenv = (std::int64_t)rd64(clk::usd::kSystemTime);
    Sleep(30);
    CHECK_EQ(frozenv, (std::int64_t)rd64(clk::usd::kSystemTime));   // updater really stopped
    CHECK(clk::VirtualSharedData() == p);
    clk::Initialize(clk::Settings{});
}
