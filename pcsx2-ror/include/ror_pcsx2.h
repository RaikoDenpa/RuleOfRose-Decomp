#ifndef ROR_PCSX2_H
#define ROR_PCSX2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ROR_PCSX2_EXPORTS)
#define ROR_PCSX2_API __declspec(dllexport)
#else
#define ROR_PCSX2_API __declspec(dllimport)
#endif

#define ROR_PCSX2_API_VERSION 5

enum
{
    ROR_PCSX2_OK = 0,
    ROR_PCSX2_E_VERSION = -1,
    ROR_PCSX2_E_ARG = -2,
    ROR_PCSX2_E_BUFFER = -3,
    ROR_PCSX2_E_STATE = -4,
    ROR_PCSX2_E_FAILED = -5
};

enum
{
    ROR_PCSX2_LOG_ERROR = 0,
    ROR_PCSX2_LOG_WARNING = 1,
    ROR_PCSX2_LOG_INFO = 2
};

typedef void (*ror_pcsx2_log_fn)(void *user, int level, const char *message);

ROR_PCSX2_API int ror_pcsx2_api_version(void);

ROR_PCSX2_API int ror_pcsx2_open(int api_version, ror_pcsx2_log_fn log, void *user);

ROR_PCSX2_API int ror_pcsx2_version(char *buf, size_t size);

ROR_PCSX2_API void ror_pcsx2_close(void);

#define ROR_PCSX2_OPEN_VIF1 1u

ROR_PCSX2_API int ror_pcsx2_gs_config(const char *path);

ROR_PCSX2_API int ror_pcsx2_gs_open(void *window, int width, int height, unsigned flags);

ROR_PCSX2_API int ror_pcsx2_gs_resize(int width, int height);

ROR_PCSX2_API int ror_pcsx2_gs_vsync(const void *regs, size_t size);

ROR_PCSX2_API int ror_pcsx2_gs_load_state(const void *state, size_t size, const void *regs, size_t regs_size);

ROR_PCSX2_API int ror_pcsx2_gs_transfer(int path, const void *data, size_t size);

ROR_PCSX2_API int ror_pcsx2_gs_save_state(void *buf, size_t size, size_t *needed);

ROR_PCSX2_API int ror_pcsx2_gs_read_fifo(void *buf, size_t size);

ROR_PCSX2_API int ror_pcsx2_gs_screenshot(const char *path);

typedef struct ror_pcsx2_gs_counters
{
    unsigned long long vsyncs;
    unsigned long long presents;
    unsigned long long vif1_streams;
    unsigned long long vif1_stalls;
    unsigned long long gs_finish;
    unsigned long long gs_signal;
    unsigned long long gs_label;
} ror_pcsx2_gs_counters;
ROR_PCSX2_API int ror_pcsx2_gs_get_counters(ror_pcsx2_gs_counters *out, size_t size);

ROR_PCSX2_API void ror_pcsx2_gs_close(void);

ROR_PCSX2_API int ror_pcsx2_vif1_transfer(const void *data, size_t size);

ROR_PCSX2_API int ror_pcsx2_vu1_wait(void);

#define ROR_PCSX2_VU1_STATE_SIZE 33480
ROR_PCSX2_API int ror_pcsx2_vu1_get_state(void *buf, size_t size);
ROR_PCSX2_API int ror_pcsx2_vu1_set_state(const void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
