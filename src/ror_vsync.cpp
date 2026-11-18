#include "ror.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto kVblank = std::chrono::microseconds(20000);
    constexpr auto kFastVblank = std::chrono::microseconds(5000);

    constexpr uint32_t kRxVblankCount = 0x2f1f90;
    constexpr uint32_t kMagVblankHandler = 0x1176ec;
    constexpr uint32_t kVsyncCount = 0x29819c;
    constexpr uint32_t kVsyncInterval = 0x2981a0;
    constexpr uint32_t kMovieTick = 0x1827e8;
    constexpr uint32_t kMovieObject = 0x7ddfc0;
    constexpr uint32_t kMovieWait = 0x24cec8;
    constexpr uint32_t kSceDmaSync = 0x25d458;
    constexpr uint32_t kGsSwapEnv = 0x25b908;

    bool g_started = false;
    Clock::time_point g_next;
    uint32_t g_vblanks = 0;
    uint32_t g_rxBase = 0;
    uint32_t g_handler = 0;
    uint32_t g_signals = 0;
    uint64_t g_frames = 0;
    uint32_t g_interval = 0;
    uint32_t g_bursts = 0, g_burstMax = 0, g_dropped = 0;

    void vblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ror::wr32(rdram, kRxVblankCount, ++g_vblanks);
        if (const uint32_t interval = ror::rd32(rdram, kVsyncInterval); interval != g_interval)
        {
            ror::log("VBlank interval %u -> %u (frame %llu, VBlank %u)", g_interval, interval,
                     static_cast<unsigned long long>(g_frames), g_vblanks);
            g_interval = interval;
        }
        ror::gsDumpVsync();
        ror::gsVsync(runtime);
        ror::soundVblank();
        ror::optionsVblank(rdram);
        if (g_handler == kMagVblankHandler)
        {
            const uint32_t count = ror::rd32(rdram, kVsyncCount) + 1;
            ror::wr32(rdram, kVsyncCount, count);
            ror::call(rdram, ctx, runtime, kMovieTick, {count});
            if (ror::rd32(rdram, kVsyncInterval) <= count)
            {
                ror::wr32(rdram, kVsyncCount, 0);
                ++g_signals;
            }
        }
        else if (g_handler)
        {
            ror::call(rdram, ctx, runtime, g_handler, {});
        }
    }

    void pump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint64_t fastUntil = []
        {
            const char *env = std::getenv("ROR_FAST_UNTIL");
            return env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        const auto period = g_frames < fastUntil || ror::fastForwardHeld() ? kFastVblank : kVblank;
        const auto now = Clock::now();
        if (!g_started)
        {
            g_started = true;
            g_next = now + period;
            return;
        }
        int n = 0;
        for (; now >= g_next; ++n)
        {
            if (n == 8)
            {
                g_dropped += static_cast<uint32_t>((now - g_next) / period);
                g_next = now + period;
                break;
            }
            vblank(rdram, ctx, runtime);
            g_next += period;
        }
        if (n > 1)
        {
            ++g_bursts;
            g_burstMax = std::max<uint32_t>(g_burstMax, static_cast<uint32_t>(n));
        }
    }

    void waitVblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        pump(rdram, ctx, runtime);
        const uint32_t start = g_vblanks;
        while (g_vblanks == start && !runtime->isStopRequested())
        {
            std::this_thread::sleep_until(g_next);
            pump(rdram, ctx, runtime);
        }
    }

    uint64_t maxFrames()
    {
        static const uint64_t n = []
        {
            const char *env = std::getenv("ROR_MAX_FRAMES");
            return env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        return n;
    }
}

uint64_t ror::frame() { return g_frames; }

void ror_InitThread(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 1); }
void ror_rxThInit(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_rxThCreate(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("rxThCreate(entry %08x) refused: no RX threads in the port (ra %08x)", ror::arg(ctx, 0), getRegU32(ctx, 31));
    ror::ret(ctx, static_cast<uint32_t>(-1));
}

void ror_rxThWait(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_rxThQueueCallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("rx worker callback %08x(%08x, %08x, %08x) dropped (ra %08x)", ror::arg(ctx, 0), ror::arg(ctx, 1),
             ror::arg(ctx, 2), ror::arg(ctx, 3), getRegU32(ctx, 31));
    ror::ret(ctx, 1);
}

void ror_rxThQueuedCount(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }
void ror_rxThAlarmCallback(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }

void ror_rxWaitVblank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int32_t n = ror::sarg(ctx, 0);
    pump(rdram, ctx, runtime);
    if (n < 0)
    {
        ror::ret(ctx, g_vblanks);
        return;
    }
    if (n == 0)
        waitVblank(rdram, ctx, runtime);
    else
        while (g_vblanks < g_rxBase + static_cast<uint32_t>(n) && n > 1 && !runtime->isStopRequested())
            waitVblank(rdram, ctx, runtime);
    g_rxBase = g_vblanks;
    ror::ret(ctx, 0);
}

void ror_gs_set_vblank_handler(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t prev = g_handler;
    g_handler = ror::arg(ctx, 0);
    ror::log("vblank handler %08x (was %08x)", g_handler, prev);
    ror::ret(ctx, prev);
}

void ror_wait_vsync_swap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static Clock::time_point s_leave{};
    static double s_workMs = 0, s_waitMs = 0;
    static uint64_t s_gsNs = 0;
    const auto enter = Clock::now();
    if (s_leave != Clock::time_point{})
        s_workMs += std::chrono::duration<double, std::milli>(enter - s_leave).count();
    ror::gsFrameMark(runtime, g_frames, false);
    const uint32_t self = ror::arg(ctx, 0);
    if (ror::rd32(rdram, kMovieObject) != 0)
        ror::call(rdram, ctx, runtime, kMovieWait, {0});
    ror::wr32(rdram, self + 0x30, 0);
    pump(rdram, ctx, runtime);
    if (ror::rd32(rdram, kVsyncInterval) <= ror::rd32(rdram, kVsyncCount))
        waitVblank(rdram, ctx, runtime);
    while (g_signals == 0 && !runtime->isStopRequested())
        waitVblank(rdram, ctx, runtime);
    g_signals = 0;
    const uint32_t channel = ror::rd32(rdram, self + 0x160);
    for (int i = 0; ror::call(rdram, ctx, runtime, kSceDmaSync, {channel, 1, 0}) != 0; ++i)
    {
        if (i == 50 || runtime->isStopRequested())
        {
            ror::log("wait_vsync_swap: DMA channel %08x still busy after %d VBlanks, going on", channel, i);
            break;
        }
        waitVblank(rdram, ctx, runtime);
    }
    ror::wr32(rdram, self + 0x28, 0);
    ror::call(rdram, ctx, runtime, kGsSwapEnv, {self + 0x170, ror::rd32(rdram, self) & 1});

    ++g_frames;
    s_leave = Clock::now();
    s_waitMs += std::chrono::duration<double, std::milli>(s_leave - enter).count();
    if (g_frames == 1 || g_frames % 250 == 0)
    {
        const double n = g_frames == 1 ? 1.0 : 250.0;
        const uint64_t gsNs = ror::gsBackendNanos();
        ror::log("frame %llu done (VBlank %u; per frame: game %.1f ms incl. GS raster backend %.1f ms, vsync wait %.1f ms; "
                 "catch-up: %u bursts, largest %u VBlanks, %u dropped)",
                 static_cast<unsigned long long>(g_frames), g_vblanks, s_workMs / n, (gsNs - s_gsNs) / 1e6 / n, s_waitMs / n,
                 g_bursts, g_burstMax, g_dropped);
        s_workMs = s_waitMs = 0;
        s_gsNs = gsNs;
        g_bursts = g_burstMax = g_dropped = 0;
    }
    ror::gsFrameMark(runtime, g_frames, true);
    ror::vif1DumpFrame(rdram, runtime, self, g_frames);
    static const uint64_t s_profileFrame = []
    {
        const char *env = std::getenv("ROR_PROFILE");
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    if (s_profileFrame && g_frames == s_profileFrame)
    {
        const char *colon = std::strchr(std::getenv("ROR_PROFILE"), ':');
        ror::profileStart(colon ? std::atof(colon + 1) : 10.0, "profile.tsv");
    }
    static const uint64_t s_gameShot = []
    {
        const char *env = std::getenv("ROR_GAME_SCREENSHOT");
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    if (s_gameShot && g_frames == s_gameShot)
    {
        const std::string name = "host0:ror_shot_" + std::to_string(g_frames) + ".bmp";
        const uint32_t p = runtime->guestMalloc(static_cast<uint32_t>(name.size() + 1), 16u);
        if (uint8_t *s = p ? ror::ptr(rdram, p, static_cast<uint32_t>(name.size() + 1)) : nullptr)
        {
            std::memcpy(s, name.c_str(), name.size() + 1);
            ror::log("game screenshot requested: %s (MagSystem %08x, file name pointer was %08x)", name.c_str(), self,
                     ror::rd32(rdram, self + 0x34));
            ror::wr32(rdram, self + 0x34, p);
            ror::wr32(rdram, self + 0x38, 1);
        }
    }
    if (maxFrames() && g_frames >= maxFrames())
    {
        ror::log("reached ROR_MAX_FRAMES = %llu, stopping", static_cast<unsigned long long>(maxFrames()));
        runtime->requestStop();
    }
    ror::ret(ctx, 0);
}
