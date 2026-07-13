#include "AVClock.h"

extern "C" {
#include <libavutil/time.h>
}

int64_t AVClock::nowUs() const {
    return av_gettime_relative();
}

void AVClock::setVideoClock(double pts) {
    std::lock_guard<std::mutex> lock(m_videoMutex);
    m_videoPts = pts;
    m_videoSysTime = nowUs();
    m_videoReady.store(true, std::memory_order_release);
}

ClockPoint AVClock::videoClock() const {
    std::lock_guard<std::mutex> lock(m_videoMutex);
    return { m_videoPts, m_videoSysTime };
}

bool AVClock::isReady() const {
    return m_videoReady.load(std::memory_order_acquire);
}

void AVClock::setAudioClock(double pts) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioPts = pts;
    m_audioSysTime = nowUs();
}

bool AVClock::hasAudio() const {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return m_audioSysTime > 0;
}

ClockPoint AVClock::audioClock() const {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return { m_audioPts, m_audioSysTime };
}

double AVClock::drift() const {
    auto v = videoClock();
    auto a = audioClock();
    return (a.pts - v.pts) - (a.systemTime - v.systemTime) / 1000000.0;
}

void AVClock::reset() {
    {
        std::lock_guard<std::mutex> lock(m_videoMutex);
        m_videoPts = 0.0;
        m_videoSysTime = 0;
    }
    m_videoReady.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioPts = 0.0;
        m_audioSysTime = 0;
    }
}
