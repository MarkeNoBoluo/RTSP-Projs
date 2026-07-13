#pragma once

#include "Common.h"
#include "PusherConfig.h"
#include <functional>
#include <atomic>

class PusherStateMachine;
class PusherLifecycleManager;
class ScreenCaptureThread;
class VideoEncodeThread;
class GpuVideoEncodeThread;
class AudioEncodeThread;
class RTSPMuxThread;
class SDLAudioCapture;
class VideoRawFrameQueue;
class AudioRingBuffer;
class EncodedPacketQueue;
struct PusherStats;

class RTSPusher {
    friend class PusherLifecycleManager;

public:
    using StateCallback = std::function<void(PusherState)>;
    using ErrorCallback = std::function<void(const char*)>;

    RTSPusher();
    ~RTSPusher();

    bool open(const PusherConfig& config);
    void close();

    PusherState state() const;
    PusherStats* stats() const { return m_stats; }
    PusherLifecycleManager* lifecycle() const { return m_lifecycle; }

    void setStateCallback(StateCallback cb);
    void setErrorCallback(ErrorCallback cb);

    const PusherConfig& config() const { return m_config; }

private:
    bool startPipeline();
    void stopPipeline();

    PusherConfig        m_config;
    PusherStateMachine* m_stateMachine = nullptr;
    PusherLifecycleManager* m_lifecycle = nullptr;
    PusherStats*        m_stats = nullptr;

    VideoRawFrameQueue*    m_rawFrameQueue = nullptr;
    ScreenCaptureThread*   m_captureThread = nullptr;

    EncodedPacketQueue*    m_encodedQueue = nullptr;
    VideoEncodeThread*     m_encodeThread = nullptr;
    GpuVideoEncodeThread*  m_gpuEncodeThread = nullptr;

    RTSPMuxThread*         m_muxThread = nullptr;

    AudioRingBuffer*       m_audioRingBuffer = nullptr;
    SDLAudioCapture*       m_audioCapture = nullptr;
    AudioEncodeThread*     m_audioEncodeThread = nullptr;

    std::atomic<int> m_serial{0};

    StateCallback m_stateCallback;
    ErrorCallback m_errorCallback;
};
