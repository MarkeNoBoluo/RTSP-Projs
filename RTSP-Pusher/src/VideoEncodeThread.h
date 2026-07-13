#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <functional>
#include <thread>
#include <atomic>

extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVFormatContext;
class VideoRawFrameQueue;
class EncodedPacketQueue;

class VideoEncodeThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    VideoEncodeThread(const PusherConfig* config,
                      VideoRawFrameQueue* inputQueue,
                      EncodedPacketQueue* outputQueue,
                      PusherStats* stats);
    ~VideoEncodeThread();

    bool start(int serial);
    void stop();
    void join();

    AVCodecContext* codecContext() const { return m_codecCtx; }
    void setErrorCallback(ErrorCallback cb);
    void setSerial(int serial);

private:
    bool openEncoder();
    bool ensureSwsContext(int srcW, int srcH, int srcFmt);
    bool convertFrame(AVFrame* src, AVFrame* dst);
    void run();
    void closeEncoder();

    const PusherConfig*   m_config;
    VideoRawFrameQueue*   m_inputQueue;
    EncodedPacketQueue*   m_outputQueue;
    PusherStats*          m_stats;

    AVCodecContext* m_codecCtx = nullptr;
    AVFrame*        m_scaledFrame = nullptr;
    SwsContext*     m_swsCtx = nullptr;
    int             m_swsSrcW = 0;
    int             m_swsSrcH = 0;
    int             m_swsSrcFmt = -1;

    const char*   m_requestedEncoder = "libx264";      // user's --hw-encoder value
    const char*   m_encoderName = "libx264";           // actual opened encoder
    AVPixelFormat m_encoderPixFmt = AV_PIX_FMT_YUV420P; // varies by encoder

    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};

    ErrorCallback m_errorCallback;
};
