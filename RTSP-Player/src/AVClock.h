#pragma once

#include "Common.h"
#include <atomic>
#include <mutex>

class AVClock {
public:
    void       setVideoClock(double pts);
    ClockPoint videoClock() const;
    bool       isReady() const;

    void       setAudioClock(double pts);
    ClockPoint audioClock() const;
    bool       hasAudio() const;

    double     drift() const;
    void       reset();

private:
    int64_t nowUs() const;

    mutable std::mutex m_videoMutex;
    double     m_videoPts = 0.0;
    int64_t    m_videoSysTime = 0;
    std::atomic<bool> m_videoReady{false};

    mutable std::mutex m_audioMutex;
    double     m_audioPts = 0.0;
    int64_t    m_audioSysTime = 0;
};
