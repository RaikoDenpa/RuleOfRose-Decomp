#pragma once

#include "common/WindowInfo.h"

#include <optional>

namespace ror_pcsx2
{
    void log(int level, const char* fmt, ...);

    bool isOpen();

    std::optional<WindowInfo> renderWindow();
    void countPresent();

    bool vif1Open();
    void vif1Close();
    bool vif1IsOpen();
    void vif1Counters(unsigned long long &streams, unsigned long long &stalls);
}
