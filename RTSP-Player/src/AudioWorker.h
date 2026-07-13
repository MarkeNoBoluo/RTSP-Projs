#pragma once

#include <atomic>
#include <cstdio>
#include <thread>

class PacketQueue;
class AVClock;
class AudioRingBuffer;
class PlayerStats;

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
}
#endif

class AudioWorker {
public:
    AudioWorker(AVCodecContext* codecCtx, AVRational timeBase,
                PacketQueue* queue, AVClock* clock,
                AudioRingBuffer* ringBuffer, PlayerStats* stats);
    ~AudioWorker();

    void setSerial(int serial) { m_serial.store(serial, std::memory_order_release); }
    bool isReady() const { return m_ready.load(std::memory_order_acquire); }

    void start();
    void stop();
    void join();

private:
    void run();
    // void writeWavHeader();
    // void updateWavHeader();

    AVCodecContext*    m_codecCtx;
    AVRational         m_timeBase;
    PacketQueue*       m_queue;
    AVClock*           m_clock;
    AudioRingBuffer*   m_ringBuffer;
    PlayerStats*       m_stats     = nullptr;

    SwrContext*        m_swrCtx   = nullptr;
    // FILE*              m_wavFile  = nullptr;
    int                m_dataSize = 0;
    // bool               m_writeWavEnabled = false;

    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_ready{false};
    std::atomic<int>   m_serial{0};
    std::thread        m_thread;

    double m_syntheticAudioPts = 0.0;
    bool   m_syntheticPtsValid = false;

    static constexpr int kTargetRate     = 48000;
    static constexpr int kTargetChannels = 2;
    static constexpr int kTargetFormat   = AV_SAMPLE_FMT_S16;
    // static constexpr const char* kWavPath = "audio_output.wav";
};
