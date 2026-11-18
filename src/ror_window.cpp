#include "ror.h"
#include "ror_host.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>

namespace
{
    std::atomic<bool> g_paused{false};
    float g_volume = 1.0f;

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
    g_volume = static_cast<float>(volume < 0 ? 0 : volume > 100 ? 100 : volume) / 100.0f;
    SetMasterVolume(g_volume);
    ror::log("window %dx%d%s, volume %d%%", GetScreenWidth(), GetScreenHeight(),
             IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? " (fullscreen)" : "", volume);
}

bool ror::paused() { return g_paused.load(std::memory_order_relaxed); }

void ror::windowLoop()
{
    if (IsKeyPressed(KEY_F11))
    {
        ToggleBorderlessWindowed();
        applyCursor();
    }
    static const bool pauseUnfocused = optionInt("pause_unfocused", 0) == 1;
    if (!pauseUnfocused)
        return;
    const bool pause = !hostWindowActive(GetWindowHandle());
    if (pause == g_paused.load(std::memory_order_relaxed))
        return;
    g_paused.store(pause, std::memory_order_relaxed);
    SetMasterVolume(pause ? 0.0f : g_volume);
    if (pause)
        rumble(0, 0);
    hostWindowTitleSuffix(GetWindowHandle(), pause ? L" - paused (click to continue)" : L"");
    ror::log(pause ? "paused: the window is in the background" : "resumed");
}
