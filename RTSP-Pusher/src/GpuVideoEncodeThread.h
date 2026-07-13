#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <atomic>
#include <functional>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;

class EncodedPacketQueue;

enum class GpuBackend { QSV, NVENC };

class GpuVideoEncodeThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    GpuVideoEncodeThread(const PusherConfig* config,
                         EncodedPacketQueue* outputQueue,
                         PusherStats* stats,
                         GpuBackend backend = GpuBackend::QSV);
    ~GpuVideoEncodeThread();

    bool start(int serial);
    void stop();
    void join();

    AVCodecContext* codecContext() const { return m_codecCtx; }
    void setErrorCallback(ErrorCallback cb);
    void setSerial(int serial);

private:
    bool openFilterGraph();
    bool pullInitialFrame();
    bool openEncoder();
    bool encodeFrame(AVFrame* frame, int serial, int64_t pts);
    void receivePackets(int serial, int64_t beforeUs, int64_t* maxEncodeUs, int64_t* encodeCount);
    void run();
    void close();

    const PusherConfig* m_config;
    EncodedPacketQueue* m_outputQueue;
    PusherStats* m_stats;
    GpuBackend m_backend;

    AVFilterGraph*   m_filterGraph = nullptr;
    AVFilterContext* m_sourceCtx = nullptr;
    AVFilterContext* m_hwmapCtx = nullptr;
    AVFilterContext* m_scaleCtx = nullptr;
    AVFilterContext* m_sinkCtx = nullptr;
    AVCodecContext*  m_codecCtx = nullptr;
    AVBufferRef*     m_hwFramesRef = nullptr;
    AVFrame*         m_pendingFrame = nullptr;

    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};

    ErrorCallback m_errorCallback;
};
