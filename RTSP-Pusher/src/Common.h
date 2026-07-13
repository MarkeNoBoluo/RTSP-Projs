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

enum class PusherState : int {
    Stopped,
    Opening,
    Streaming,
    Recovering,
    Reconnecting,
    Error,
    Closing
};

// SDL_USEREVENT codes — pushed by SDL_AddTimer callbacks or lifecycle manager
enum UserEventCode : int {
    EVENT_RECONNECT = 1,
    EVENT_STATS     = 2,
    EVENT_DURATION  = 3,
};
