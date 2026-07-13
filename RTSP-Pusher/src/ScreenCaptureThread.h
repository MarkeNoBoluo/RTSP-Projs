#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <functional>
#include <thread>
#include <atomic>

struct AVFormatContext;
struct AVCodecContext;
class VideoRawFrameQueue;

class ScreenCaptureThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    ScreenCaptureThread(const PusherConfig* config,
                        VideoRawFrameQueue* queue,
                        PusherStats* stats);
    ~ScreenCaptureThread();

    bool start(int serial);
    void stop();
    void join();

    void setErrorCallback(ErrorCallback cb);
    void setSerial(int serial);

private:
    bool openInput();
    void run();
    void closeInput();

    const PusherConfig* m_config;
    VideoRawFrameQueue* m_queue;
    PusherStats*        m_stats;

    AVFormatContext* m_inputCtx = nullptr;
    AVCodecContext*  m_decoderCtx = nullptr;
    int m_videoStreamIndex = -1;

    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};

    ErrorCallback m_errorCallback;
};
