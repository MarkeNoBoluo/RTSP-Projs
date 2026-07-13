#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <SDL.h>
#include <functional>
#include <thread>
#include <atomic>

struct AVCodecContext;
struct SwrContext;
struct AVFrame;
class AudioRingBuffer;
class EncodedPacketQueue;

class AudioEncodeThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    AudioEncodeThread(const PusherConfig* config,
                      AudioRingBuffer* ringBuffer,
                      EncodedPacketQueue* outputQueue,
                      PusherStats* stats);
    ~AudioEncodeThread();

    bool start(const SDL_AudioSpec& inputSpec, int serial);
    void stop();
    void join();

    AVCodecContext* codecContext() const { return m_codecCtx; }
    void setErrorCallback(ErrorCallback cb);
    void setSerial(int serial);

private:
    bool openEncoder();
    bool ensureResampler(const SDL_AudioSpec& inputSpec);
    void run();
    void closeEncoder();

    const PusherConfig* m_config;
    AudioRingBuffer*    m_ringBuffer;
    EncodedPacketQueue* m_outputQueue;
    PusherStats*        m_stats;

    AVCodecContext* m_codecCtx = nullptr;
    SwrContext*     m_swrCtx = nullptr;
    AVFrame*        m_inputFrame = nullptr;
    AVFrame*        m_resampledFrame = nullptr;
    int             m_inputSampleCount = 0;  // samples per frame
    int             m_outputSampleCount = 0;

    // Input format from SDL
    int m_srcSampleRate = 0;
    int m_srcChannels = 0;
    int m_srcFormat = 0;

    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};

    ErrorCallback m_errorCallback;
};
