#include "ror_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <mutex>
#include <string>

namespace ror
{
    void log(const char *fmt, ...);
}

namespace
{
    using SetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_VIBRATION *);
    using GetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

    std::mutex g_mutex;
    bool g_loaded = false;
    SetStateFn g_setState = nullptr;
    GetStateFn g_getState = nullptr;
    int g_pad = -1;
    DWORD g_lastScan = 0;
    int g_small = 0, g_large = 0;

    struct StopAtExit
    {
        ~StopAtExit()
        {
            if (g_setState && g_pad >= 0 && (g_small || g_large))
            {
                XINPUT_VIBRATION v{};
                g_setState(static_cast<DWORD>(g_pad), &v);
            }
        }
    } g_stopAtExit;

    bool load()
    {
        if (g_loaded)
            return g_setState != nullptr;
        g_loaded = true;
        HMODULE dll = LoadLibraryW(L"xinput1_4.dll");
        if (!dll)
            dll = LoadLibraryW(L"xinput9_1_0.dll");
        if (dll)
        {
            g_setState = reinterpret_cast<SetStateFn>(GetProcAddress(dll, "XInputSetState"));
            g_getState = reinterpret_cast<GetStateFn>(GetProcAddress(dll, "XInputGetState"));
        }
        if (!g_setState || !g_getState)
        {
            g_setState = nullptr;
            ror::log("rumble: no XInput on this system");
        }
        return g_setState != nullptr;
    }

    int findPad()
    {
        const DWORD now = GetTickCount();
        if (g_pad >= 0 || now - g_lastScan < 2000)
            return g_pad;
        g_lastScan = now;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            XINPUT_STATE st{};
            if (g_getState(i, &st) == ERROR_SUCCESS)
            {
                g_pad = static_cast<int>(i);
                ror::log("rumble: XInput controller %d", g_pad);
                break;
            }
        }
        return g_pad;
    }
}

bool ror::hostWindowActive(void *hwnd)
{
    HWND fg = GetForegroundWindow();
    return fg && (fg == static_cast<HWND>(hwnd) || IsChild(static_cast<HWND>(hwnd), fg));
}

void ror::hostWindowTitleSuffix(void *hwnd, const wchar_t *suffix)
{
    static std::wstring base;
    HWND w = static_cast<HWND>(hwnd);
    if (base.empty())
    {
        wchar_t t[256] = {};
        GetWindowTextW(w, t, 256);
        base = t;
    }
    SetWindowTextW(w, (base + (suffix ? suffix : L"")).c_str());
}

void ror::rumble(int small, int large)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!load())
        return;
    small = small ? 1 : 0;
    large = large < 0 ? 0 : large > 255 ? 255 : large;
    if (small == g_small && large == g_large && g_pad >= 0)
        return;
    if (findPad() < 0)
        return;
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = static_cast<WORD>(large * 257);
    v.wRightMotorSpeed = small ? 0xc000 : 0;
    if (g_setState(static_cast<DWORD>(g_pad), &v) != ERROR_SUCCESS)
    {
        g_pad = -1;
        return;
    }
    g_small = small;
    g_large = large;
}
