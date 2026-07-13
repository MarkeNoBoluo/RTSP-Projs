#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <functional>
#include <thread>
#include <atomic>

struct AVFormatContext;
struct AVCodecContext;
class EncodedPacketQueue;

class RTSPMuxThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    RTSPMuxThread(const PusherConfig* config,
                  EncodedPacketQueue* packetQueue,
                  PusherStats* stats);
    ~RTSPMuxThread();

    bool open(AVCodecContext* videoCodecCtx, AVCodecContext* audioCodecCtx);
    bool start(int serial);
    void stop();
    void join();
    void close();

    AVFormatContext* outputContext() const { return m_outputCtx; }
    int videoStreamIndex() const { return m_videoStreamIndex; }
    int audioStreamIndex() const { return m_audioStreamIndex; }

    void setErrorCallback(ErrorCallback cb);
    void setSerial(int serial);

private:
    bool createVideoStream(AVCodecContext* codecCtx);
    int  createAudioStream(AVCodecContext* codecCtx);
    bool writeHeader();
    void run();
    void writeTrailer();

    const PusherConfig*  m_config;
    EncodedPacketQueue*  m_packetQueue;
    PusherStats*         m_stats;

    AVFormatContext* m_outputCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};

    bool m_headerWritten = false;

    ErrorCallback m_errorCallback;
};
