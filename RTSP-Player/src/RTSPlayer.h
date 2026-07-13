#pragma once

#include "Common.h"
#include <functional>

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class PlayerStats;
class StreamLifecycleManager;
class IRenderer;

class RTSPlayer {
public:
    using StateCallback = std::function<void(PlayerState)>;
    using ErrorCallback = std::function<void(const char*)>;
    using EndOfStreamCallback = std::function<void()>;

    RTSPlayer();
    ~RTSPlayer();

    bool open(const char* url);
    void close();

    void         setRenderer(IRenderer* renderer);
    IRenderer*   renderer() const;
    PlayerStats* stats()    const;
    PlayerState  state()    const;

    void setStateCallback(StateCallback cb);
    void setErrorCallback(ErrorCallback cb);
    void setEndOfStreamCallback(EndOfStreamCallback cb);
    void setTransport(const char* transport);
    void setAudioEnabled(bool v);
    void setSetptsZero(bool v);
    void setHwAccel(const char* mode);

    void videoRefresh();

    int pktSerial() const;

private:
    PlayerStateMachine*     m_stateMachine;
    PacketQueue*            m_videoQueue;
    PacketQueue*            m_audioQueue;
    VideoFrameQueue*        m_frameQueue;
    AVClock*                m_clock;
    PlayerStats*            m_stats;
    IRenderer*              m_renderer = nullptr;
    StreamLifecycleManager* m_lifecycle;

    double   m_frameTimer       = 0.0;
    double   m_frameLastPts     = 0.0;
    int64_t  m_lastDropUs       = 0;
    int      m_frameTimerSerial  = -1;
    int64_t  m_lastRenderUs     = 0;
    bool     m_inVideoRefresh   = false;
    bool     m_fastCatchUp      = false;
    bool     m_setptsZero       = false;
    bool     m_lowLatency       = false;
    int      m_fastCatchUpFrameCount = 0;
    int64_t  m_videoStallBeginUs = 0;   // 0 = not in stall
};
