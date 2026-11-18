#pragma once

#include <cstddef>
#include <vector>

namespace ror
{
    void log(const char *fmt, ...);

    bool pcsx2Open();

    void pcsx2WindowInit(void *parent);
    void pcsx2WindowLoop();
    void pcsx2WindowShutdown();

    void pcsx2Vsync(const void *regs, size_t size);
    void pcsx2Vif1(const void *data, size_t size);
    void pcsx2Path3(const void *data, size_t size);
    bool pcsx2GsState(std::vector<unsigned char> &out);
    bool pcsx2Vu1State(std::vector<unsigned char> &out);
    bool pcsx2ReadFifo(void *buf, size_t size);

    bool pcsx2Screenshot(const char *path);
}
