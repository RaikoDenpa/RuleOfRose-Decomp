#pragma once

#include <cstdint>

namespace ror
{
    void log(const char *fmt, ...);
    uint64_t frame();

    void profileStart(double seconds, const char *outPath);
}
