#pragma once

#include <algorithm>
#include <cstdint>

namespace ror
{
    struct Adpcm
    {
        int h1 = 0, h2 = 0;
        void frame(const uint8_t *f, int16_t *out, int step)
        {
            static const int kCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
            int shift = f[0] & 0x0f;
            if (shift > 12)
                shift = 9;
            const int filter = (f[0] >> 4) < 5 ? (f[0] >> 4) : 0;
            const int c0 = kCoef[filter][0], c1 = kCoef[filter][1];
            for (int i = 0; i < 28; ++i)
            {
                const int nib = (f[2 + i / 2] >> ((i & 1) * 4)) & 0x0f;
                int s = ((nib & 8 ? nib - 16 : nib) << 12) >> shift;
                s += (h1 * c0 + h2 * c1 + 32) >> 6;
                s = std::clamp(s, -32768, 32767);
                out[i * step] = static_cast<int16_t>(s);
                h2 = h1;
                h1 = s;
            }
        }
    };
}
