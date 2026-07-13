#pragma once

#include "PacketQueue.h"
#include <thread>
#include <atomic>
#include <functional>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

class PlayerStateMachine;
class PlayerStats;

class DemuxThread {
public:
    using StreamErrorCallback = std::function<void()>;
    using EndOfStreamCallback = std::function<void()>;

    DemuxThread(PlayerStateMachine* sm, PlayerStats* stats,
                PacketQueue* videoQueue, PacketQueue* audioQueue);
    ~DemuxThread();

    void prepareForOpen();
    void setContext(AVFormatContext* fmtCtx, AVStream* videoStream, AVStream* audioStream);
    void setStreamErrorCallback(StreamErrorCallback cb) { m_onStreamError = std::move(cb); }
    void setEndOfStreamCallback(EndOfStreamCallback cb) { m_onEndOfStream = std::move(cb); }
    void setSerial(int serial) { m_serial = serial; }

    void start();
    void stop();
    void join();

    static int interruptCallback(void* opaque);

private:
    void run();

    PlayerStateMachine* m_stateMachine;
    PlayerStats*        m_stats;
    AVFormatContext*    m_fmtCtx = nullptr;
    AVStream*           m_videoStream = nullptr;
    AVStream*           m_audioStream = nullptr;
    PacketQueue*        m_videoQueue;
    PacketQueue*        m_audioQueue;

    std::atomic<bool>   m_abort{false};
    std::atomic<int>    m_serial{0};
    int64_t             m_lastReadTime = 0;

    StreamErrorCallback m_onStreamError;
    EndOfStreamCallback m_onEndOfStream;
    std::thread         m_thread;
};
