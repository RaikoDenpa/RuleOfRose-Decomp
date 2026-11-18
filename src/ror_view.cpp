#include "ror.h"

#include <cstring>

namespace
{
    constexpr uint32_t kViewSetup = 0x228b80;
    constexpr uint32_t kMainView = 0x7506b0;
    constexpr uint32_t kWidenX[] = {0x08, 0x0c, 0x40, 0x44};
    constexpr float kWiden = 4.0f / 3.0f;

    float rdf(uint8_t *rdram, uint32_t addr)
    {
        const uint32_t u = ror::rd32(rdram, addr);
        float f;
        std::memcpy(&f, &u, sizeof f);
        return f;
    }

    void wrf(uint8_t *rdram, uint32_t addr, float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        ror::wr32(rdram, addr, u);
    }
}

void ror_view_setup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool wide = ror::optionInt("widescreen", 0) == 1;
    static uint32_t s_widened = 0;
    if (wide && ror::arg(ctx, 1) == kMainView && ror::rd32(rdram, kMainView + 0x08) != s_widened)
    {
        const float before = rdf(rdram, kMainView + 0x08);
        for (uint32_t off : kWidenX)
            wrf(rdram, kMainView + off, rdf(rdram, kMainView + off) * kWiden);
        s_widened = ror::rd32(rdram, kMainView + 0x08);
        static int logged = 0;
        if (logged++ < 20)
            ror::log("widescreen: view x %.3f -> %.3f (y %.3f)", before, rdf(rdram, kMainView + 0x08),
                     rdf(rdram, kMainView + 0x10));
    }
    ror::original(kViewSetup)(rdram, ctx, runtime);
}
