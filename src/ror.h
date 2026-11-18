#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace ror
{
    using Handler = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    inline uint32_t arg(const R5900Context *ctx, int i) { return getRegU32(ctx, 4 + i); }
    inline int32_t sarg(const R5900Context *ctx, int i) { return static_cast<int32_t>(getRegU32(ctx, 4 + i)); }

    inline void ret(R5900Context *ctx, uint32_t value)
    {
        setReturnU32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    }

    uint8_t *ptr(uint8_t *rdram, uint32_t addr, uint32_t size = 1);
    std::string str(uint8_t *rdram, uint32_t addr, size_t max = 1024);
    std::string discFile(const std::string &guestPath);
    uint32_t rd32(uint8_t *rdram, uint32_t addr);
    void wr32(uint8_t *rdram, uint32_t addr, uint32_t value);
    void wr16(uint8_t *rdram, uint32_t addr, uint16_t value);
    void wr8(uint8_t *rdram, uint32_t addr, uint8_t value);

    uint32_t call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fn, std::initializer_list<uint32_t> args);

    void log(const char *fmt, ...);
    std::string format(uint8_t *rdram, R5900Context *ctx, uint32_t fmtAddr, int first);
    void halt(PS2Runtime *runtime, const char *why);

    uint64_t frame();
    int vblankHz();
    bool paused();

    Handler original(uint32_t addr);

    bool soundOn();
    void soundVblank();

    void optionsVblank(uint8_t *rdram);
    bool fastForwardHeld();
    std::string option(const char *name, const char *fallback = "");
    int optionInt(const char *name, int fallback);
    void windowSetup();
    void windowLoop();

    void gsInstall(PS2Runtime &runtime, bool pcsx2);
    uint64_t gsBackendNanos();
    void gsFrameMark(PS2Runtime *runtime, uint64_t frame, bool leaving);
    void gsVsync(PS2Runtime *runtime);
    void gsSnapshotVram(std::vector<uint8_t> &out);

    void gsPrivRegs(PS2Runtime &runtime, bool field, std::vector<uint8_t> &out);
    bool gsDumpInstall(PS2Runtime &runtime, bool pcsx2);
    bool gsDumpVif1Wanted();
    void gsDumpGif(GifPathId id, const uint8_t *data, uint32_t size);
    void gsDumpRegister(uint8_t reg, uint64_t value, const uint64_t (&packet)[4]);
    void gsDumpVif1(const uint8_t *data, uint32_t size);
    void gsDumpFrameMark(uint64_t frame);
    void gsDumpVsync();
    void gsDumpReadFifo(uint32_t bytes);

    void vif1DumpFrame(uint8_t *rdram, PS2Runtime *runtime, uint32_t magSystem, uint64_t frame);

    void profileStart(double seconds, const char *outPath);
}
