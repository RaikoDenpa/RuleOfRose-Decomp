#include "ror.h"
#include "ror_pcsx2host.h"

#include "raylib.h"

#include "game_overrides.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#define ROR_TRACE(addr, name)
#define ROR_BIND(addr, fn, name) void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
#include "ror_bindings.inc"
#undef ROR_BIND
#undef ROR_TRACE

extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void *process, unsigned int code);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);

namespace
{
    struct Binding
    {
        uint32_t addr;
        ror::Handler fn;
        const char *name;
    };

    const Binding kBindings[] = {
#define ROR_TRACE(addr, name)
#define ROR_BIND(addr, fn, name) {addr, &fn, name},
#include "ror_bindings.inc"
#undef ROR_BIND
#undef ROR_TRACE
    };
    constexpr size_t kCount = sizeof(kBindings) / sizeof(kBindings[0]);

    struct Trace
    {
        uint32_t addr;
        const char *name;
        ror::Handler original;
        int calls;
    };

    Trace g_traces[] = {
#define ROR_BIND(addr, fn, name)
#define ROR_TRACE(addr, name) {addr, name, nullptr, 0},
#include "ror_bindings.inc"
#undef ROR_TRACE
#undef ROR_BIND
        {0, nullptr, nullptr, 0},
    };
    constexpr size_t kTraceCount = sizeof(g_traces) / sizeof(g_traces[0]) - 1;
    constexpr int kTraceLimit = 20;

    struct TraceWindow
    {
        bool set = false;
        uint64_t from = 0, to = 0;
        TraceWindow()
        {
            const char *e = std::getenv("ROR_TRACE_FRAMES");
            unsigned long long a = 0, b = 0;
            if (e && std::sscanf(e, "%llu-%llu", &a, &b) == 2)
                set = true, from = a, to = b;
        }
        bool show(int &calls) const
        {
            if (!set)
                return calls++ < kTraceLimit;
            const uint64_t f = ror::frame();
            return f >= from && f <= to;
        }
    };
    const TraceWindow g_traceWindow;

    std::atomic<R5900Context *> g_ctx{nullptr};
    std::atomic<uint64_t> g_calls{0};

    template <size_t I>
    void trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<bool> seen{false};
        g_ctx.store(ctx, std::memory_order_relaxed);
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (!seen.exchange(true))
            ror::log("first call %08x %s (ra %08x)", kBindings[I].addr, kBindings[I].name, getRegU32(ctx, 31));
        kBindings[I].fn(rdram, ctx, runtime);
    }

    void dumpWatchRing();

    void watchdog(PS2Runtime *runtime)
    {
        uint64_t last = 0;
        int idle = 0;
        EeThreadSnapshot lastAlive{};
        bool reportedDormant = false;
        while (!runtime->isStopRequested())
        {
            for (int tick = 0; tick < 20 && !runtime->isStopRequested(); ++tick)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                for (const EeThreadSnapshot &t : runtime->eeScheduler().snapshot().threads)
                {
                    if (t.id != 1)
                        continue;
                    if (t.status != EeThreadStatus::Dormant)
                    {
                        lastAlive = t;
                        reportedDormant = false;
                    }
                    else if (!reportedDormant && lastAlive.id == 1)
                    {
                        reportedDormant = true;
                        ror::log("watchdog: main thread went dormant; last seen pc %08x ra %08x sp %08x, invocation depth %u",
                                 lastAlive.pc, lastAlive.ra, lastAlive.sp, lastAlive.invocationDepth);
                        dumpWatchRing();
                    }
                }
            }
            const uint64_t calls = g_calls.load(std::memory_order_relaxed);
            R5900Context *ctx = g_ctx.load(std::memory_order_relaxed);
            if (calls != last || !ctx)
            {
                last = calls;
                idle = 0;
                continue;
            }
            if (++idle != 5 && idle % 30 != 0)
                continue;
            uint32_t lo = 0xffffffffu, hi = 0, pc = 0;
            for (int i = 0; i < 2000; ++i)
            {
                pc = *reinterpret_cast<volatile uint32_t *>(&ctx->pc);
                lo = std::min(lo, pc);
                hi = std::max(hi, pc);
                if ((i & 63) == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            ror::log("watchdog: no override called for %d s; guest pc %08x (sampled range %08x-%08x), ra %08x, sp %08x",
                     idle, pc, lo, hi, getRegU32(ctx, 31), getRegU32(ctx, 29));
            static const char *kStatus[] = {"running", "ready", "waiting", "waiting+suspended", "suspended", "dormant"};
            static const char *kWait[] = {"-", "sleep", "sema", "eventflag", "vsync", "external", "mpeg"};
            const EeKernelSnapshot snap = runtime->eeScheduler().snapshot();
            for (const EeThreadSnapshot &t : snap.threads)
                ror::log("watchdog:   thread %d %s wait %s %d, pc %08x ra %08x sp %08x, entry %08x prio %d%s", t.id,
                         kStatus[static_cast<int>(t.status)], kWait[static_cast<int>(t.waitReason)], t.waitId, t.pc, t.ra,
                         t.sp, t.entry, t.currentPriority, t.id == snap.runningThreadId ? " (current)" : "");
            for (const EeSemaphoreSnapshot &s : snap.semaphores)
                if (s.waiters)
                    ror::log("watchdog:   sema %d count %d/%d, %u waiting", s.id, s.count, s.maxCount, s.waiters);
        }
        for (int s = 1; s <= 15; ++s)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (s == 15)
            {
                ror::log("watchdog: the game thread did not stop within 15 s: forcing exit (code 3)");
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
            if (s != 5 && s != 10)
                continue;
            static const char *cdb = std::getenv("ROR_HANG_CDB");
            if (cdb && s == 5)
            {
                const std::string cmd = "\"\"" + std::string(cdb) + "\" -p " + std::to_string(GetCurrentProcessId()) +
                                        " -logo hang_stacks.txt -c \"~*kn 40; qd\" > nul 2>&1\"";
                ror::log("watchdog: capturing host stacks with cdb (hang_stacks.txt)");
                std::system(cmd.c_str());
            }
            R5900Context *ctx = g_ctx.load(std::memory_order_relaxed);
            uint32_t lo = 0xffffffffu, hi = 0, pc = 0;
            for (int i = 0; ctx && i < 2000; ++i)
            {
                pc = *reinterpret_cast<volatile uint32_t *>(&ctx->pc);
                lo = std::min(lo, pc);
                hi = std::max(hi, pc);
                if ((i & 63) == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            ror::log("watchdog: still running %d s after the stop request; guest pc %08x (sampled range %08x-%08x), "
                     "ra %08x, last override call count %llu", s, pc, lo, hi, ctx ? getRegU32(ctx, 31) : 0u,
                     static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)));
            const EeKernelSnapshot snap = runtime->eeScheduler().snapshot();
            for (const EeThreadSnapshot &t : snap.threads)
                ror::log("watchdog:   thread %d status %d wait %d %d, pc %08x ra %08x%s", t.id, static_cast<int>(t.status),
                         static_cast<int>(t.waitReason), t.waitId, t.pc, t.ra,
                         t.id == snap.runningThreadId ? " (current)" : "");
        }
    }

    template <size_t... I>
    constexpr std::array<ror::Handler, kCount> makeTrampolines(std::index_sequence<I...>)
    {
        return {&trampoline<I>...};
    }
    constexpr auto kTrampolines = makeTrampolines(std::make_index_sequence<kCount>{});

    template <size_t I>
    void traceTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Trace &t = g_traces[I];
        const uint32_t ra = getRegU32(ctx, 31);
        const bool show = g_traceWindow.show(t.calls);
        if (show)
            ror::log("trace %08x %s(%08x, %08x, %08x, %08x; %08x, %08x, %08x, %08x) from %08x", t.addr, t.name,
                     ror::arg(ctx, 0), ror::arg(ctx, 1), ror::arg(ctx, 2), ror::arg(ctx, 3), ror::arg(ctx, 4),
                     ror::arg(ctx, 5), ror::arg(ctx, 6), ror::arg(ctx, 7), ra);
        static const bool wide = std::getenv("ROR_TRACE_WIDE") != nullptr;
        if (show && wide)
            ror::log("trace %08x   a0 %016llx a1 %016llx f12 %g f13 %g", t.addr,
                     static_cast<unsigned long long>(GPR_U64(ctx, 4)), static_cast<unsigned long long>(GPR_U64(ctx, 5)),
                     ctx->f[12], ctx->f[13]);
        t.original(rdram, ctx, runtime);
        if (!show)
            return;
        if (ctx->pc == ra && wide)
            ror::log("trace %08x %s -> %016llx, f0 %g", t.addr, t.name, static_cast<unsigned long long>(GPR_U64(ctx, 2)),
                     ctx->f[0]);
        else if (ctx->pc == ra)
            ror::log("trace %08x %s -> %08x", t.addr, t.name, getRegU32(ctx, 2));
        else
            ror::log("trace %08x %s left at pc %08x (ra %08x)", t.addr, t.name, ctx->pc, ra);
    }

    template <size_t... I>
    constexpr std::array<ror::Handler, sizeof...(I)> makeTraceTrampolines(std::index_sequence<I...>)
    {
        return {&traceTrampoline<I>...};
    }
    constexpr auto kTraceTrampolines = makeTraceTrampolines(std::make_index_sequence<kTraceCount>{});

    struct WatchHit
    {
        uint64_t frame, value;
        uint32_t addr, size, pc, ra, sp;
    };
    uint32_t g_watchValue = 0;
    bool g_watchValueSet = false;
    uint64_t g_watchFrom = 0;
    std::atomic<uint64_t> g_watchHits{0};
    WatchHit g_watchRing[256];

    void logWatchHit(const char *tag, const WatchHit &h)
    {
        ror::log("watch%s: frame %llu, %u-byte store at %08x = %016llx (pc %08x ra %08x sp %08x)", tag,
                 static_cast<unsigned long long>(h.frame), h.size, h.addr, static_cast<unsigned long long>(h.value), h.pc,
                 h.ra, h.sp);
    }

    void watchHit(uint32_t addr, uint32_t size, uint64_t lo, uint64_t hi, const R5900Context *ctx)
    {
        const uint64_t frame = ror::frame();
        if (frame < g_watchFrom)
            return;
        if (g_watchValueSet)
        {
            bool match = false;
            for (uint32_t i = 0; i < size && i < 16; i += 4)
                match |= static_cast<uint32_t>((i < 8 ? lo >> (i * 8) : hi >> ((i - 8) * 8))) == g_watchValue;
            if (!match)
                return;
        }
        const WatchHit h{frame, lo, addr, size, ctx ? ctx->pc : 0u, ctx ? getRegU32(ctx, 31) : 0u,
                         ctx ? getRegU32(ctx, 29) : 0u};
        const uint64_t n = g_watchHits.fetch_add(1);
        g_watchRing[n % 256] = h;
        if (n < 200)
            logWatchHit("", h);
    }

    void dumpWatchRing()
    {
        const uint64_t n = g_watchHits.load();
        if (!n)
            return;
        ror::log("watch: the last %llu of %llu stores:", static_cast<unsigned long long>(std::min<uint64_t>(n, 256)),
                 static_cast<unsigned long long>(n));
        for (uint64_t i = n > 256 ? n - 256 : 0; i < n; ++i)
            logWatchHit(" (last)", g_watchRing[i % 256]);
    }

    void installWatch()
    {
        const char *e = std::getenv("ROR_WATCH");
        unsigned lo = 0, hi = 0, value = 0;
        if (!e)
            return;
        const int n = std::sscanf(e, "%x-%x=%x", &lo, &hi, &value);
        if (n < 2 || hi <= lo)
        {
            ror::log("ROR_WATCH=%s: expected LO-HI[=VALUE][@FRAME], hex range and value", e);
            return;
        }
        if (const char *at = std::strchr(e, '@'))
            g_watchFrom = std::strtoull(at + 1, nullptr, 10);
        g_watchValue = value;
        g_watchValueSet = n == 3;
        g_ps2WriteWatch.fn = watchHit;
        g_ps2WriteWatch.lo = lo;
        g_ps2WriteWatch.hi = hi;
        ror::log("watch: guest stores to %08x-%08x%s", lo, hi, g_watchValueSet ? " (value filter)" : "");
    }

    void apply(PS2Runtime &runtime)
    {
        installWatch();
        runtime.reserveGuestHeapArena(0x1EC0000u, 0x1F00000u);
        if (IsWindowReady())
            SetWindowState(FLAG_WINDOW_ALWAYS_RUN);
        else
            SetConfigFlags(FLAG_WINDOW_ALWAYS_RUN);
        ror::gsInstall(runtime, ror::pcsx2Open());
        constexpr std::pair<uint32_t, const char *> kOnlyWith[] = {{0x135928, "ROR_MENU_SWAP"}};
        size_t ok = 0, skipped = 0;
        for (size_t i = 0; i < kCount; ++i)
        {
            const auto cond = std::find_if(std::begin(kOnlyWith), std::end(kOnlyWith),
                                           [&](const auto &c) { return c.first == kBindings[i].addr; });
            if (cond != std::end(kOnlyWith) && !std::getenv(cond->second))
            {
                ++skipped;
                continue;
            }
            if (runtime.replaceFunction(kBindings[i].addr, kTrampolines[i]))
                ++ok;
            else
                ror::log("cannot bind %08x %s", kBindings[i].addr, kBindings[i].name);
        }
        for (size_t i = 0; i < kTraceCount; ++i)
        {
            g_traces[i].original = runtime.lookupFunction(g_traces[i].addr);
            runtime.replaceFunction(g_traces[i].addr, kTraceTrampolines[i]);
        }
        ror::log("Rule of Rose overrides: %zu of %zu bound, %zu diagnostic left unbound, %zu traced", ok, kCount, skipped,
                 kTraceCount);
        std::thread(watchdog, &runtime).detach();
    }

    std::mutex g_logMutex;
    FILE *g_log = nullptr;
}

PS2_REGISTER_GAME_OVERRIDE("Rule of Rose (PAL, SLES_542.18)", "SLES_542.18", 0x00100008u, 0u, &apply)

void ror_nop(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 0); }
void ror_one(uint8_t *, R5900Context *ctx, PS2Runtime *) { ror::ret(ctx, 1); }

namespace ror
{
    uint8_t *ptr(uint8_t *rdram, uint32_t addr, uint32_t size)
    {
        uint8_t *first = getMemPtr(rdram, addr);
        if (!first || size == 0)
            return first;
        uint8_t *last = getMemPtr(rdram, addr + size - 1);
        return (last && last - first == static_cast<ptrdiff_t>(size - 1)) ? first : nullptr;
    }

    std::string str(uint8_t *rdram, uint32_t addr, size_t max)
    {
        std::string s;
        for (size_t i = 0; i < max; ++i)
        {
            const uint8_t *p = getMemPtr(rdram, addr + static_cast<uint32_t>(i));
            if (!p || *p == 0)
                break;
            s.push_back(static_cast<char>(*p));
        }
        return s;
    }

    uint32_t rd32(uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        if (const uint8_t *p = ptr(rdram, addr, 4))
            std::memcpy(&v, p, 4);
        return v;
    }

    void wr32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (uint8_t *p = ptr(rdram, addr, 4))
            std::memcpy(p, &value, 4);
    }

    void wr16(uint8_t *rdram, uint32_t addr, uint16_t value)
    {
        if (uint8_t *p = ptr(rdram, addr, 2))
            std::memcpy(p, &value, 2);
    }

    void wr8(uint8_t *rdram, uint32_t addr, uint8_t value)
    {
        if (uint8_t *p = ptr(rdram, addr, 1))
            *p = value;
    }

    uint32_t call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fn, std::initializer_list<uint32_t> args)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t pc = ctx->pc;
        int r = 4;
        for (uint32_t a : args)
        {
            SET_GPR_U32(ctx, r, a);
            ++r;
        }
        SET_GPR_U32(ctx, 31, ra);
        ctx->pc = fn;
        runtime->lookupFunction(fn)(rdram, ctx, runtime);
        int resumes = 0;
        while (ctx->pc != ra && ctx->pc != 0u && !runtime->isStopRequested() && resumes < 100000)
        {
            ++resumes;
            runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
        }
        const uint32_t v0 = getRegU32(ctx, 2);
        if (ctx->pc != ra && !runtime->isStopRequested())
            log("call %08x returned to %08x, expected %08x (after %d resumes)", fn, ctx->pc, ra, resumes);
        SET_GPR_U32(ctx, 31, ra);
        ctx->pc = pc;
        return v0;
    }

    void log(const char *fmt, ...)
    {
        char line[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_log)
        {
            const char *path = std::getenv("ROR_LOG");
            g_log = std::fopen(path && *path ? path : "ror.log", "w");
        }
        const unsigned long long f = frame();
        if (g_log)
        {
            std::fprintf(g_log, "[f%05llu] %s\n", f, line);
            std::fflush(g_log);
        }
        std::fprintf(stderr, "[ror f%05llu] %s\n", f, line);
    }

    void halt(PS2Runtime *runtime, const char *why)
    {
        log("HALT: %s", why);
        if (IsDebuggerPresent())
            __debugbreak();
        else
            runtime->requestStop();
    }
}
