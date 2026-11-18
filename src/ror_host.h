#pragma once

namespace ror
{
    bool hostWindowActive(void *hwnd);
    void hostWindowTitleSuffix(void *hwnd, const wchar_t *suffix);

    void rumble(int small, int large);
}
