#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

enum class PlayerState : int {
    Stopped,
    Connecting,
    Playing,
    Recovering,
    Reconnecting,
    Error,
    Closing
};

enum class HwAccelMode : int {
    Auto,   // Try DXVA2, fall back to software on failure
    Dxva2,  // Force DXVA2, fail if unavailable
    None    // Software decode only
};

struct VideoFrame {
    AVFrame* frame = nullptr;
    int64_t  pts   = AV_NOPTS_VALUE;
    int64_t  presentTime = 0;
    int      serial = 0;
};

struct ClockPoint {
    double  pts;
    int64_t systemTime;
};

// SDL_USEREVENT codes — pushed by SDL_AddTimer callbacks, consumed by main loop
enum UserEventCode : int {
    EVENT_RECONNECT = 1,
    EVENT_STATS     = 2,
    EVENT_STREAM_EOF = 3,
};
