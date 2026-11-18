#include "ror.h"

#include "raylib.h"

#include <algorithm>

namespace
{
    void applyCursor()
    {
        if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            HideCursor();
        else
            ShowCursor();
    }
}

void ror::windowSetup()
{
    SetWindowMinSize(320, 224);
    const int w = optionInt("window_width", 0), h = optionInt("window_height", 0);
    const int m = GetCurrentMonitor();
    if (w >= 320 && h >= 224)
    {
        SetWindowSize(w, h);
        SetWindowPosition(std::max(0, (GetMonitorWidth(m) - w) / 2), std::max(32, (GetMonitorHeight(m) - h) / 2));
    }
    if (optionInt("fullscreen", 0) == 1)
        ToggleBorderlessWindowed();
    applyCursor();
    const int volume = optionInt("volume", 100);
    SetMasterVolume(static_cast<float>(volume < 0 ? 0 : volume > 100 ? 100 : volume) / 100.0f);
    ror::log("window %dx%d%s, volume %d%%", GetScreenWidth(), GetScreenHeight(),
             IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? " (fullscreen)" : "", volume);
}

void ror::windowLoop()
{
    if (IsKeyPressed(KEY_F11))
    {
        ToggleBorderlessWindowed();
        applyCursor();
    }
}
